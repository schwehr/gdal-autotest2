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
// Tests object name (comment) message encoding, decoding, copying, sizing,
// resetting, debugging, and comment persistence in
// third_party/hdf5/src/H5Oname.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oname.c

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5ONAME_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5ONAME_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5ONAME_TEST_HAS_ASAN_OR_LSAN) &&                      \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5ONAME_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5ONAME_TEST_HAS_OTHER_SAN) &&                    \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5ONAME_TEST_HAS_OTHER_SAN 1
#endif

// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs pulled in via H5Opkg.h. Define NDEBUG around internal headers
// so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5ONAME_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5ONAME_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ONAME_TEST_RESTORE_UNDEF_NDEBUG
#endif

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
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

std::string ReadTmpFile(FILE* fp) {
  std::fflush(fp);
  std::rewind(fp);
  std::string out;
  char chunk[256];
  while (true) {
    const size_t n = std::fread(chunk, 1, sizeof(chunk), fp);
    if (n == 0) {
      break;
    }
    out.append(chunk, n);
  }
  return out;
}

TEST(H5OnameTest, DescriptorMatchesClassSpec) {
  ScopedFile file = CreateInMemoryFile("name_desc.h5");
  ASSERT_TRUE(file.valid());

  const H5O_msg_class_t& msg = H5O_MSG_NAME[0];
  EXPECT_EQ(msg.id, static_cast<unsigned>(H5O_NAME_ID));
  EXPECT_STREQ(msg.name, "name");
  EXPECT_EQ(msg.native_size, sizeof(H5O_name_t));
  EXPECT_EQ(msg.share_flags, 0U);
  EXPECT_EQ(H5O_msg_class_g[H5O_NAME_ID], H5O_MSG_NAME);
  EXPECT_NE(msg.decode, nullptr);
  EXPECT_NE(msg.encode, nullptr);
  EXPECT_NE(msg.copy, nullptr);
  EXPECT_NE(msg.raw_size, nullptr);
  EXPECT_NE(msg.reset, nullptr);
  EXPECT_EQ(msg.free, nullptr);
  EXPECT_NE(msg.debug, nullptr);
}

TEST(H5OnameTest, DecodeEncodeSizeCopyResetAndDebug) {
  ScopedFile file = CreateInMemoryFile("name_codec.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  const char kText[] = "sensor calibration v2";
  H5O_name_t src{};
  src.s = const_cast<char*>(kText);

  const size_t raw_size = H5O_msg_raw_size(f, H5O_NAME_ID, false, &src);
  EXPECT_EQ(raw_size, std::strlen(kText) + 1);

  std::vector<uint8_t> encoded(raw_size, 0xff);
  ASSERT_GE(H5O_msg_encode(f, H5O_NAME_ID, false, encoded.data(), &src), 0);
  EXPECT_EQ(encoded.back(), 0U);
  EXPECT_STREQ(reinterpret_cast<const char*>(encoded.data()), kText);

  // Decode normal NUL-terminated buffer.
  H5O_name_t* decoded = static_cast<H5O_name_t*>(
      H5O_msg_decode(f, nullptr, H5O_NAME_ID, encoded.size(), encoded.data()));
  ASSERT_NE(decoded, nullptr);
  EXPECT_STREQ(decoded->s, kText);
  H5O_msg_free(H5O_NAME_ID, decoded);

  // Decode empty string (single NUL byte).
  const uint8_t kEmptyBuf[1] = {0};
  H5O_name_t* decoded_empty = static_cast<H5O_name_t*>(
      H5O_msg_decode(f, nullptr, H5O_NAME_ID, sizeof(kEmptyBuf), kEmptyBuf));
  ASSERT_NE(decoded_empty, nullptr);
  EXPECT_STREQ(decoded_empty->s, "");
  EXPECT_EQ(H5O_msg_raw_size(f, H5O_NAME_ID, false, decoded_empty), 1U);
  H5O_msg_free(H5O_NAME_ID, decoded_empty);

  // Decode non-NUL-terminated buffer truncated cleanly at p_size - 1 bytes.
  const uint8_t kUntermBuf[5] = {'a', 'b', 'c', 'd', 'e'};
  H5O_name_t* decoded_trunc = static_cast<H5O_name_t*>(
      H5O_msg_decode(f, nullptr, H5O_NAME_ID, sizeof(kUntermBuf), kUntermBuf));
  ASSERT_NE(decoded_trunc, nullptr);
  EXPECT_STREQ(decoded_trunc->s, "abcd");
  H5O_msg_free(H5O_NAME_ID, decoded_trunc);

  // Copy allocating destination and copying into existing destination.
  H5O_name_t* copy_alloc =
      static_cast<H5O_name_t*>(H5O_msg_copy(H5O_NAME_ID, &src, nullptr));
  ASSERT_NE(copy_alloc, nullptr);
  EXPECT_NE(copy_alloc->s, src.s);
  EXPECT_STREQ(copy_alloc->s, kText);
  H5O_msg_free(H5O_NAME_ID, copy_alloc);

  H5O_name_t copy_stack{};
  void* copy_ret = H5O_msg_copy(H5O_NAME_ID, &src, &copy_stack);
  EXPECT_EQ(copy_ret, &copy_stack);
  EXPECT_STREQ(copy_stack.s, kText);
  EXPECT_GE(H5O_msg_reset(H5O_NAME_ID, &copy_stack), 0);
  EXPECT_EQ(copy_stack.s, nullptr);
  // Resetting again when s is already null is safe.
  EXPECT_GE(H5O_msg_reset(H5O_NAME_ID, &copy_stack), 0);

  // Null string size and copy failure cleanup paths (dest == nullptr vs !=).
  H5O_name_t empty_ptr{};
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5O_msg_raw_size(f, H5O_NAME_ID, false, &empty_ptr), 0U);
    EXPECT_EQ(H5O_msg_copy(H5O_NAME_ID, &empty_ptr, nullptr), nullptr);
    H5O_name_t preexisting{};
    EXPECT_EQ(H5O_msg_copy(H5O_NAME_ID, &empty_ptr, &preexisting), nullptr);
    EXPECT_EQ(preexisting.s, nullptr);
  }

  // Debug output.
  FILE* fp = std::tmpfile();
  ASSERT_NE(fp, nullptr);
  absl::Cleanup close_fp = [fp] { std::fclose(fp); };
  ASSERT_GE(H5O_debug_id(H5O_NAME_ID, f, &src, fp, 2, 12), 0);
  const std::string dbg = ReadTmpFile(fp);
  EXPECT_THAT(dbg, HasSubstr("Name:"));
  EXPECT_THAT(dbg, HasSubstr(kText));
}

TEST(H5OnameTest, CommentPersistenceAcrossReopenAndDebugHeader) {
  TempFile tmp("h5oname_persist.h5");
  {
    ScopedFile file(
        H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(
        H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ASSERT_GE(H5Oset_comment(grp.get(), "persistent comment"), 0);
  }

  {
    ScopedFile file(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    char buf[64] = {};
    const ssize_t len = H5Oget_comment_by_name(file.get(), "/grp", buf,
                                               sizeof(buf), H5P_DEFAULT);
    ASSERT_EQ(len, static_cast<ssize_t>(std::strlen("persistent comment")));
    EXPECT_STREQ(buf, "persistent comment");

    ScopedGroup grp(H5Gopen2(file.get(), "/grp", H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    H5O_loc_t* oloc = H5O_get_loc(grp.get());
    ASSERT_NE(oloc, nullptr);

    ScopedApiContext ctx;
    FILE* fp = std::tmpfile();
    ASSERT_NE(fp, nullptr);
    absl::Cleanup close_fp = [fp] { std::fclose(fp); };
    ASSERT_GE(H5O_debug(oloc->file, oloc->addr, fp, 0, 20), 0);
    const std::string oh_dbg = ReadTmpFile(fp);
    EXPECT_THAT(oh_dbg, HasSubstr("persistent comment"));
  }
}

#if !defined(H5ONAME_TEST_HAS_ASAN_OR_LSAN) && \
    !defined(H5ONAME_TEST_HAS_OTHER_SAN)
TEST(H5OnameTest, DecodeHandlesStrndupOomCleanup) {
  // Tighten RLIMIT_AS inside an isolated threadsafe child process so
  // H5MM_strndup fails in H5O__name_decode and executes cleanup safely.
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  const auto run_oom_check = []() {
    void* decoded_oom = nullptr;
    {
      ScopedErrorSilence silence;
      ScopedFile file = CreateInMemoryFile("name_oom.h5");
      if (!file.valid()) {
        std::exit(1);
      }
      H5F_t* f = UnwrapFile(file.get());
      if (f == nullptr) {
        std::exit(2);
      }

      // Warm up decode/free and error-push allocations before tightening
      // limits.
      const uint8_t warm_buf[2] = {'w', '\0'};
      void* warm =
          H5O_msg_decode(f, nullptr, H5O_NAME_ID, sizeof(warm_buf), warm_buf);
      if (warm != nullptr) {
        H5O_msg_free(H5O_NAME_ID, warm);
      }
      H5O_name_t empty_ptr{};
      H5O_msg_copy(H5O_NAME_ID, &empty_ptr, nullptr);

      constexpr size_t kLargeLen = 16ULL << 20;
      std::string large_str(kLargeLen, 'x');
      std::vector<void*> drain;
      drain.reserve(256);

      constexpr std::array<size_t, 9> kWarmSizes = {8,   16,  32,   64,  128,
                                                    256, 512, 1024, 2048};
      for (size_t sz : kWarmSizes) {
        void* p = std::malloc(sz);
        std::free(p);
      }

      std::ifstream statm("/proc/self/statm");
      unsigned long vm_pages = 0;
      if (!(statm >> vm_pages)) {
        std::exit(3);
      }
      const long page_sz = ::sysconf(_SC_PAGESIZE);
      if (page_sz <= 0) {
        std::exit(4);
      }
      const rlim_t vm_bytes =
          static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

      struct rlimit old_lim{};
      if (::getrlimit(RLIMIT_AS, &old_lim) != 0) {
        std::exit(5);
      }
      struct rlimit tight_lim = old_lim;
      tight_lim.rlim_cur = std::min<rlim_t>(
          old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(4ULL << 20));
      if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) {
        std::exit(6);
      }

      for (int i = 0; i < 256; ++i) {
        void* p = std::malloc(kLargeLen + 1);
        if (p == nullptr) {
          break;
        }
        drain.push_back(p);
      }

      decoded_oom =
          H5O_msg_decode(f, nullptr, H5O_NAME_ID, large_str.size() + 1,
                         reinterpret_cast<const uint8_t*>(large_str.data()));

      ::setrlimit(RLIMIT_AS, &old_lim);
      for (void* p : drain) {
        std::free(p);
      }
    }
    if (decoded_oom != nullptr) {
      std::exit(7);
    }
    std::exit(0);
  };

  EXPECT_EXIT(run_oom_check(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
