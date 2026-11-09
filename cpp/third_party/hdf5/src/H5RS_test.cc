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
// Tests internal reference-counted string primitives (H5RS_*), including
// copy/wrap construction, reference counting, copy-on-modify unwrapping,
// string/character/formatted concatenation, buffer growth, and comparisons
// (third_party/hdf5/src/H5RS.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5RS.c
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5RSprivate.h

// clang-format off
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// clang-format on

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#include "third_party/hdf5/src/H5RSprivate.h"
}  // extern "C"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5RS_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5RS_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5RS_TEST_HAS_ASAN_OR_LSAN) &&                         \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5RS_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5RS_TEST_HAS_OTHER_SAN) &&                       \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5RS_TEST_HAS_OTHER_SAN 1
#endif

namespace hdf5 {
namespace {

// Matches H5RS_ALLOC_SIZE in third_party/hdf5/src/H5RS.c.
constexpr size_t kInitialAllocSize = 256;

struct RefStrDeleter {
  void operator()(H5RS_str_t* rs) const {
    if (rs != nullptr) {
      H5RS_decr(rs);
    }
  }
};

using ScopedRefStr = std::unique_ptr<H5RS_str_t, RefStrDeleter>;

class H5RsTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_GE(H5open(), 0); }
};

TEST_F(H5RsTest, CreateCopiesEmptyShortBoundaryAndLongStrings) {
  // 1. Empty string allocates internal storage and skips memcpy.
  ScopedRefStr empty_rs(H5RS_create(""));
  ASSERT_NE(empty_rs, nullptr);
  EXPECT_EQ(H5RS_len(empty_rs.get()), 0U);
  EXPECT_STREQ(H5RS_get_str(empty_rs.get()), "");
  EXPECT_EQ(H5RS_get_count(empty_rs.get()), 1U);

  // 2. Short string is copied rather than aliasing the caller buffer.
  std::string mutable_src = "hdf5_ref_string";
  ScopedRefStr short_rs(H5RS_create(mutable_src.c_str()));
  ASSERT_NE(short_rs, nullptr);
  EXPECT_NE(H5RS_get_str(short_rs.get()), mutable_src.c_str());
  mutable_src[0] = 'X';
  EXPECT_EQ(H5RS_len(short_rs.get()), 15U);
  EXPECT_STREQ(H5RS_get_str(short_rs.get()), "hdf5_ref_string");
  EXPECT_EQ(H5RS_get_count(short_rs.get()), 1U);

  // 3. Exact boundary (255 + NUL == 256) and larger inputs double capacity.
  constexpr auto kTestLengths =
      std::to_array<size_t>({kInitialAllocSize - 1, kInitialAllocSize, 600});
  for (const size_t len : kTestLengths) {
    const std::string expected(len, 'a');
    ScopedRefStr sized_rs(H5RS_create(expected.c_str()));
    ASSERT_NE(sized_rs, nullptr);
    EXPECT_EQ(H5RS_len(sized_rs.get()), len);
    EXPECT_STREQ(H5RS_get_str(sized_rs.get()), expected.c_str());
  }
}

TEST_F(H5RsTest, CreateNullAllocatesLazilyAcrossAppendApis) {
  // Note: H5RS_create(nullptr) leaves rs->s == nullptr until a non-empty
  // append runs, and H5RS_decr assumes rs->s is non-null for unwrapped strings.
  // Populate immediately before other assertions so teardown is always valid.

  // 1. Empty acat on NULL string is a no-op, followed by real allocation.
  H5RS_str_t* raw_acat = H5RS_create(nullptr);
  ASSERT_NE(raw_acat, nullptr);
  EXPECT_EQ(H5RS_get_count(raw_acat), 1U);
  ASSERT_GE(H5RS_acat(raw_acat, ""), 0);
  ASSERT_GE(H5RS_acat(raw_acat, "alpha"), 0);
  ScopedRefStr acat_rs(raw_acat);
  EXPECT_EQ(H5RS_len(acat_rs.get()), 5U);
  EXPECT_STREQ(H5RS_get_str(acat_rs.get()), "alpha");

  // 2. H5RS_ancat initializes NULL-backed string.
  H5RS_str_t* raw_ancat = H5RS_create(nullptr);
  ASSERT_NE(raw_ancat, nullptr);
  ASSERT_GE(H5RS_ancat(raw_ancat, "beta_extra", 4), 0);
  ScopedRefStr ancat_rs(raw_ancat);
  EXPECT_EQ(H5RS_len(ancat_rs.get()), 4U);
  EXPECT_STREQ(H5RS_get_str(ancat_rs.get()), "beta");

  // 3. H5RS_aputc initializes NULL-backed string.
  H5RS_str_t* raw_aputc = H5RS_create(nullptr);
  ASSERT_NE(raw_aputc, nullptr);
  ASSERT_GE(H5RS_aputc(raw_aputc, 'Z'), 0);
  ScopedRefStr aputc_rs(raw_aputc);
  EXPECT_EQ(H5RS_len(aputc_rs.get()), 1U);
  EXPECT_STREQ(H5RS_get_str(aputc_rs.get()), "Z");

  // 4. H5RS_asprintf_cat initializes NULL-backed string even for empty output.
  H5RS_str_t* raw_printf_empty = H5RS_create(nullptr);
  ASSERT_NE(raw_printf_empty, nullptr);
  ASSERT_GE(H5RS_asprintf_cat(raw_printf_empty, "%s", ""), 0);
  ScopedRefStr printf_empty_rs(raw_printf_empty);
  EXPECT_EQ(H5RS_len(printf_empty_rs.get()), 0U);
  EXPECT_STREQ(H5RS_get_str(printf_empty_rs.get()), "");

  H5RS_str_t* raw_printf = H5RS_create(nullptr);
  ASSERT_NE(raw_printf, nullptr);
  ASSERT_GE(H5RS_asprintf_cat(raw_printf, "%s:%d", "node", 42), 0);
  ScopedRefStr printf_rs(raw_printf);
  EXPECT_STREQ(H5RS_get_str(printf_rs.get()), "node:42");
}

TEST_F(H5RsTest, WrapBorrowsCallerPointerUntilIncrOrModify) {
  // 1. Wrapping borrows the original pointer directly without copying.
  char stack_buf[] = "borrowed_value";
  ScopedRefStr wrapped(H5RS_wrap(stack_buf));
  ASSERT_NE(wrapped, nullptr);
  EXPECT_EQ(H5RS_get_str(wrapped.get()), stack_buf);
  EXPECT_EQ(H5RS_len(wrapped.get()), std::strlen(stack_buf));
  EXPECT_EQ(H5RS_get_count(wrapped.get()), 1U);

  // 2. Empty appends keep a wrapped string borrowed without copying.
  EXPECT_GE(H5RS_acat(wrapped.get(), ""), 0);
  EXPECT_GE(H5RS_ancat(wrapped.get(), "ignored", 0), 0);
  EXPECT_EQ(H5RS_get_str(wrapped.get()), stack_buf);

  // 3. H5RS_incr unwraps into owned storage so caller buffer can mutate.
  ASSERT_GE(H5RS_incr(wrapped.get()), 0);
  ScopedRefStr extra_ref_one(wrapped.get());
  EXPECT_EQ(H5RS_get_count(wrapped.get()), 2U);
  EXPECT_NE(H5RS_get_str(wrapped.get()), stack_buf);
  stack_buf[0] = 'X';
  EXPECT_STREQ(H5RS_get_str(wrapped.get()), "borrowed_value");

  // 4. Subsequent H5RS_incr takes the non-wrapped fast path.
  ASSERT_GE(H5RS_incr(wrapped.get()), 0);
  ScopedRefStr extra_ref_two(wrapped.get());
  EXPECT_EQ(H5RS_get_count(wrapped.get()), 3U);

  // 5. Decrementing drops count step-by-step down to 1 before final cleanup.
  EXPECT_GE(H5RS_decr(extra_ref_two.release()), 0);
  EXPECT_EQ(H5RS_get_count(wrapped.get()), 2U);
  EXPECT_GE(H5RS_decr(extra_ref_one.release()), 0);
  EXPECT_EQ(H5RS_get_count(wrapped.get()), 1U);

  // 6. Wrapped string freed directly skips H5FL_BLK_FREE on borrowed memory.
  constexpr char kLiteral[] = "read_only_literal";
  ScopedRefStr literal_wrapped(H5RS_wrap(kLiteral));
  ASSERT_NE(literal_wrapped, nullptr);
  EXPECT_EQ(H5RS_get_str(literal_wrapped.get()), kLiteral);
}

TEST_F(H5RsTest, MutatingWrappedStringCopiesBeforeAppending) {
  // 1. H5RS_acat on wrapped string copies before appending.
  char buf1[] = "head";
  ScopedRefStr rs1(H5RS_wrap(buf1));
  ASSERT_NE(rs1, nullptr);
  ASSERT_GE(H5RS_acat(rs1.get(), "_tail"), 0);
  EXPECT_NE(H5RS_get_str(rs1.get()), buf1);
  buf1[0] = 'X';
  EXPECT_STREQ(H5RS_get_str(rs1.get()), "head_tail");

  // 2. H5RS_ancat on wrapped string copies before appending.
  char buf2[] = "head";
  ScopedRefStr rs2(H5RS_wrap(buf2));
  ASSERT_NE(rs2, nullptr);
  ASSERT_GE(H5RS_ancat(rs2.get(), "_1234", 3), 0);
  EXPECT_NE(H5RS_get_str(rs2.get()), buf2);
  buf2[0] = 'X';
  EXPECT_STREQ(H5RS_get_str(rs2.get()), "head_12");

  // 3. H5RS_aputc on wrapped string copies before appending.
  char buf3[] = "head";
  ScopedRefStr rs3(H5RS_wrap(buf3));
  ASSERT_NE(rs3, nullptr);
  ASSERT_GE(H5RS_aputc(rs3.get(), '!'), 0);
  EXPECT_NE(H5RS_get_str(rs3.get()), buf3);
  buf3[0] = 'X';
  EXPECT_STREQ(H5RS_get_str(rs3.get()), "head!");

  // 4. H5RS_asprintf_cat on wrapped string copies before formatting.
  char buf4[] = "head";
  ScopedRefStr rs4(H5RS_wrap(buf4));
  ASSERT_NE(rs4, nullptr);
  ASSERT_GE(H5RS_asprintf_cat(rs4.get(), "_%s", "fmt"), 0);
  EXPECT_NE(H5RS_get_str(rs4.get()), buf4);
  buf4[0] = 'X';
  EXPECT_STREQ(H5RS_get_str(rs4.get()), "head_fmt");
}

TEST_F(H5RsTest, DupIncrementsCountHandlesNullAndSharesWrappedState) {
  // 1. Null input returns null safely.
  EXPECT_EQ(H5RS_dup(nullptr), nullptr);

  // 2. Duplicating an owned string returns the same handle with count == 2.
  ScopedRefStr owned(H5RS_create("shared"));
  ASSERT_NE(owned, nullptr);
  ScopedRefStr owned_dup(H5RS_dup(owned.get()));
  EXPECT_EQ(owned_dup.get(), owned.get());
  EXPECT_EQ(H5RS_get_count(owned.get()), 2U);

  // 3. Duplicating a wrapped string preserves borrowed state across both refs,
  //    and mutating unwraps the shared buffer for both references.
  char borrowed_buf[] = "still_borrowed";
  ScopedRefStr wrapped(H5RS_wrap(borrowed_buf));
  ASSERT_NE(wrapped, nullptr);
  ScopedRefStr wrapped_dup(H5RS_dup(wrapped.get()));
  EXPECT_EQ(wrapped_dup.get(), wrapped.get());
  EXPECT_EQ(H5RS_get_count(wrapped.get()), 2U);
  EXPECT_EQ(H5RS_get_str(wrapped.get()), borrowed_buf);

  ASSERT_GE(H5RS_acat(wrapped.get(), "_ok"), 0);
  EXPECT_NE(H5RS_get_str(wrapped_dup.get()), borrowed_buf);
  borrowed_buf[0] = 'X';
  EXPECT_STREQ(H5RS_get_str(wrapped_dup.get()), "still_borrowed_ok");
  EXPECT_EQ(H5RS_get_count(wrapped_dup.get()), 2U);
}

TEST_F(H5RsTest, AcatHandlesEmptyAndGrowsAcrossExactBoundaryAndLargeInputs) {
  // 1. Start just below H5RS_ALLOC_SIZE (256) so 255 fits without resizing.
  const std::string prefix(200, 'p');
  const std::string fit_chunk(55, 'f');
  ScopedRefStr rs(H5RS_create(prefix.c_str()));
  ASSERT_NE(rs, nullptr);

  EXPECT_GE(H5RS_acat(rs.get(), ""), 0);
  EXPECT_EQ(H5RS_len(rs.get()), 200U);

  ASSERT_GE(H5RS_acat(rs.get(), fit_chunk.c_str()), 0);
  EXPECT_EQ(H5RS_len(rs.get()), kInitialAllocSize - 1);

  // 2. Appending 1 more byte hits len + 1 == 256 >= max, triggering resize.
  ASSERT_GE(H5RS_acat(rs.get(), "!"), 0);
  EXPECT_EQ(H5RS_len(rs.get()), kInitialAllocSize);

  // 3. Appending a 700-byte chunk triggers multiple doublings (512 -> 1024).
  const std::string large_chunk(700, 'L');
  ASSERT_GE(H5RS_acat(rs.get(), large_chunk.c_str()), 0);
  const std::string expected = prefix + fit_chunk + "!" + large_chunk;
  EXPECT_EQ(H5RS_len(rs.get()), expected.size());
  EXPECT_STREQ(H5RS_get_str(rs.get()), expected.c_str());
}

TEST_F(H5RsTest, AncatClampsLengthIgnoresEmptyAndTriggersResize) {
  ScopedRefStr rs(H5RS_create("start"));
  ASSERT_NE(rs, nullptr);

  // 1. Zero count or empty string is a no-op.
  EXPECT_GE(H5RS_ancat(rs.get(), "ignored", 0), 0);
  EXPECT_GE(H5RS_ancat(rs.get(), "", 10), 0);
  EXPECT_STREQ(H5RS_get_str(rs.get()), "start");

  // 2. Count smaller than strlen truncates; larger than strlen clamps.
  ASSERT_GE(H5RS_ancat(rs.get(), "_abcdef", 4), 0);
  ASSERT_GE(H5RS_ancat(rs.get(), "_xy", 100), 0);
  EXPECT_STREQ(H5RS_get_str(rs.get()), "start_abc_xy");

  // 3. Resize across 256-byte capacity boundary and multi-doubling resize.
  const std::string pad(250 - H5RS_len(rs.get()), 'q');
  ASSERT_GE(H5RS_ancat(rs.get(), pad.c_str(), pad.size()), 0);
  EXPECT_EQ(H5RS_len(rs.get()), 250U);

  ASSERT_GE(H5RS_ancat(rs.get(), "123456789", 6), 0);
  EXPECT_EQ(H5RS_len(rs.get()), kInitialAllocSize);

  const std::string big(600, 'M');
  ASSERT_GE(H5RS_ancat(rs.get(), big.c_str(), 550), 0);
  EXPECT_EQ(H5RS_len(rs.get()), kInitialAllocSize + 550);
  const std::string expected_tail = "123456" + std::string(550, 'M');
  EXPECT_TRUE(
      std::string_view(H5RS_get_str(rs.get())).ends_with(expected_tail));
}

TEST_F(H5RsTest, AputcAppendsCharactersAcrossCapacityBoundary) {
  const std::string base(250, 'b');
  ScopedRefStr rs(H5RS_create(base.c_str()));
  ASSERT_NE(rs, nullptr);

  // Appending 10 chars crosses 255 -> 256 and triggers H5RS__resize_for_append.
  for (int i = 0; i < 10; ++i) {
    ASSERT_GE(H5RS_aputc(rs.get(), '0' + i), 0);
  }
  const std::string expected = base + "0123456789";
  EXPECT_EQ(H5RS_len(rs.get()), expected.size());
  EXPECT_STREQ(H5RS_get_str(rs.get()), expected.c_str());
}

TEST_F(H5RsTest, AsprintfCatRetriesWithVaCopyOnOverflowAndMultiGrowth) {
  // 1. Start at length 250 so 6 bytes remain in the initial 256-byte buffer;
  // formatting > 5 bytes forces vsnprintf retry using va_copy.
  const std::string base(250, 'r');
  ScopedRefStr rs(H5RS_create(base.c_str()));
  ASSERT_NE(rs, nullptr);

  ASSERT_GE(H5RS_asprintf_cat(rs.get(), ":%s:%d:%zu", "tag", 42, size_t{99}),
            0);
  std::string expected = base + ":tag:42:99";
  EXPECT_EQ(H5RS_len(rs.get()), expected.size());
  EXPECT_STREQ(H5RS_get_str(rs.get()), expected.c_str());

  // 2. Large formatted payload forces multi-doubling resize inside retry loop.
  const std::string big_arg(700, 'v');
  ASSERT_GE(H5RS_asprintf_cat(rs.get(), "|%s|%d", big_arg.c_str(), 7), 0);
  expected += "|" + big_arg + "|7";
  EXPECT_EQ(H5RS_len(rs.get()), expected.size());
  EXPECT_STREQ(H5RS_get_str(rs.get()), expected.c_str());
}

TEST_F(H5RsTest, CmpMatchesStrcmpOrdering) {
  ScopedRefStr empty(H5RS_create(""));
  ScopedRefStr ab(H5RS_create("ab"));
  ScopedRefStr abc_owned(H5RS_create("abc"));
  ScopedRefStr abc_wrapped(H5RS_wrap("abc"));
  ScopedRefStr abd(H5RS_create("abd"));
  ScopedRefStr high_lo(H5RS_create("\x7f"));
  ScopedRefStr high_hi(H5RS_create("\x80"));
  ASSERT_NE(empty, nullptr);
  ASSERT_NE(ab, nullptr);
  ASSERT_NE(abc_owned, nullptr);
  ASSERT_NE(abc_wrapped, nullptr);
  ASSERT_NE(abd, nullptr);
  ASSERT_NE(high_lo, nullptr);
  ASSERT_NE(high_hi, nullptr);

  EXPECT_EQ(H5RS_cmp(empty.get(), empty.get()), 0);
  EXPECT_EQ(H5RS_cmp(abc_owned.get(), abc_owned.get()), 0);
  EXPECT_EQ(H5RS_cmp(abc_owned.get(), abc_wrapped.get()), 0);
  EXPECT_LT(H5RS_cmp(empty.get(), ab.get()), 0);
  EXPECT_GT(H5RS_cmp(ab.get(), empty.get()), 0);
  EXPECT_LT(H5RS_cmp(ab.get(), abc_owned.get()), 0);
  EXPECT_GT(H5RS_cmp(abc_owned.get(), ab.get()), 0);
  EXPECT_LT(H5RS_cmp(abc_owned.get(), abd.get()), 0);
  EXPECT_GT(H5RS_cmp(abd.get(), abc_wrapped.get()), 0);
  // strcmp compares bytes as unsigned char across platforms.
  EXPECT_LT(H5RS_cmp(high_lo.get(), high_hi.get()), 0);
  EXPECT_GT(H5RS_cmp(high_hi.get(), high_lo.get()), 0);
}

TEST_F(H5RsTest, FreeListGarbageCollectionAfterMixedAllocations) {
  constexpr auto kPayloadSizes = std::to_array<size_t>({16, 300, 1200});
  for (const size_t len : kPayloadSizes) {
    const std::string payload(len, 'g');
    ScopedRefStr rs(H5RS_create(payload.c_str()));
    ASSERT_NE(rs, nullptr);
    ASSERT_GE(H5RS_acat(rs.get(), "_done"), 0);
    EXPECT_EQ(H5RS_len(rs.get()), len + 5);
  }
  EXPECT_GE(H5garbage_collect(), 0);
}

#if !defined(H5RS_TEST_HAS_ASAN_OR_LSAN) && !defined(H5RS_TEST_HAS_OTHER_SAN)
TEST_F(H5RsTest, AllocationFailureReturnsErrorAcrossCopyAndResizePaths) {
  // Run in an isolated threadsafe death-test child process with RLIMIT_AS
  // tightened so large H5FL_BLK_MALLOC/REALLOC calls fail cleanly while
  // sanitizers (whose allocators abort on OOM) are inactive.
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  const auto run_oom_checks = []() {
    bool all_failed = false;
    {
      ScopedErrorSilence silence;

      // 1. Pre-allocate payloads and target handles before setting RLIMIT_AS.
      //    kLargeLen is 16 MiB so doubling to 32 MiB exceeds 4 MiB headroom.
      constexpr size_t kLargeLen = 16ULL << 20;
      const std::string large_str(kLargeLen, 'x');
      H5RS_str_t* aputc_rs = nullptr;
      {
        const std::string boundary_str(kLargeLen - 1, 'b');
        aputc_rs = H5RS_create(boundary_str.c_str());
      }

      H5RS_str_t* acat_resize_rs = H5RS_create("base");
      H5RS_str_t* ancat_resize_rs = H5RS_create("base");
      H5RS_str_t* printf_resize_rs = H5RS_create("base");
      H5RS_str_t* incr_wrap_rs = H5RS_wrap(large_str.c_str());
      H5RS_str_t* acat_wrap_rs = H5RS_wrap(large_str.c_str());
      H5RS_str_t* ancat_wrap_rs = H5RS_wrap(large_str.c_str());
      H5RS_str_t* aputc_wrap_rs = H5RS_wrap(large_str.c_str());
      H5RS_str_t* printf_wrap_rs = H5RS_wrap(large_str.c_str());
      if (!aputc_rs || !acat_resize_rs || !ancat_resize_rs ||
          !printf_resize_rs || !incr_wrap_rs || !acat_wrap_rs ||
          !ancat_wrap_rs || !aputc_wrap_rs || !printf_wrap_rs) {
        std::exit(1);
      }

      // 2. Measure current virtual size and cap RLIMIT_AS to vm_bytes + 4 MiB.
      std::ifstream statm("/proc/self/statm");
      unsigned long vm_pages = 0;
      if (!(statm >> vm_pages)) {
        std::exit(2);
      }
      const long page_sz = ::sysconf(_SC_PAGESIZE);
      if (page_sz <= 0) {
        std::exit(3);
      }
      const rlim_t vm_bytes =
          static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

      struct rlimit old_lim{};
      if (::getrlimit(RLIMIT_AS, &old_lim) != 0) {
        std::exit(4);
      }
      struct rlimit tight_lim = old_lim;
      tight_lim.rlim_cur = std::min<rlim_t>(
          old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(4ULL << 20));
      if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) {
        std::exit(5);
      }

      // 3. Drain pre-mapped free spans in the allocator page heap.
      std::vector<void*> drain;
      drain.reserve(256);
      for (int i = 0; i < 256; ++i) {
        void* p = std::malloc(kLargeLen);
        if (p == nullptr) {
          break;
        }
        drain.push_back(p);
      }

      // 4. Exercise OOM paths in xstrdup, prepare_for_append, and resize.
      const H5RS_str_t* create_oom = H5RS_create(large_str.c_str());
      const herr_t rc_incr = H5RS_incr(incr_wrap_rs);
      const herr_t rc_acat_wrap = H5RS_acat(acat_wrap_rs, "x");
      const herr_t rc_ancat_wrap = H5RS_ancat(ancat_wrap_rs, "x", 1);
      const herr_t rc_aputc_wrap = H5RS_aputc(aputc_wrap_rs, 'x');
      const herr_t rc_printf_wrap = H5RS_asprintf_cat(printf_wrap_rs, "x");
      const herr_t rc_acat_resize =
          H5RS_acat(acat_resize_rs, large_str.c_str());
      const herr_t rc_ancat_resize =
          H5RS_ancat(ancat_resize_rs, large_str.c_str(), kLargeLen);
      const herr_t rc_aputc_resize = H5RS_aputc(aputc_rs, '!');
      const herr_t rc_printf_resize =
          H5RS_asprintf_cat(printf_resize_rs, "%s", large_str.c_str());

      // 5. Restore address-space limit and release drained buffers before exit.
      // Note: resize_rs handles have rs->s overwritten to NULL on REALLOC OOM,
      // so we intentionally skip H5RS_decr on them in this exiting child.
      ::setrlimit(RLIMIT_AS, &old_lim);
      for (void* p : drain) {
        std::free(p);
      }
      H5RS_decr(incr_wrap_rs);
      H5RS_decr(acat_wrap_rs);
      H5RS_decr(ancat_wrap_rs);
      H5RS_decr(aputc_wrap_rs);
      H5RS_decr(printf_wrap_rs);

      all_failed =
          (create_oom == nullptr && rc_incr < 0 && rc_acat_wrap < 0 &&
           rc_ancat_wrap < 0 && rc_aputc_wrap < 0 && rc_printf_wrap < 0 &&
           rc_acat_resize < 0 && rc_ancat_resize < 0 && rc_aputc_resize < 0 &&
           rc_printf_resize < 0);
    }
    std::exit(all_failed ? 0 : 6);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
