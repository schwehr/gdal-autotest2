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
// Tests HDF5 High-Level Lite APIs (third_party/hdf5/hl/src/H5LT.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5LT.c
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/test/test_lite.c

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/base/config.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5_test_util.h"
#include "third_party/hdf5/hl/src/H5LTpublic.h"
#include "third_party/hdf5/hl/src/hdf5_hl.h"
#include "third_party/hdf5/src/H5FDpublic.h"
#include "third_party/hdf5/src/H5Tdevelop.h"
#include "third_party/hdf5/src/H5VLconnector.h"
#include "third_party/hdf5/src/H5VLconnector_passthru.h"
#include "third_party/hdf5/src/hdf5.h"

#if !defined(ABSL_HAVE_ADDRESS_SANITIZER) && !defined(__SANITIZE_ADDRESS__) && \
    !defined(ABSL_HAVE_MEMORY_SANITIZER) && !defined(MEMORY_SANITIZER) &&      \
    !defined(ABSL_HAVE_THREAD_SANITIZER) && !defined(THREAD_SANITIZER)
#define H5LT_TEST_ALLOW_OOM_AND_LEAKY_PATHS 1
#else
#define H5LT_TEST_ALLOW_OOM_AND_LEAKY_PATHS 0
#endif

// Declarations from third_party/hdf5/hl/src/H5LTprivate.h, H5LTanalyze.c, and
// internal HDF5 test/VOL symbols without pulling C-only H5private.h into C++.
extern "C" {
extern size_t indent;
herr_t H5LT_get_attribute_disk(hid_t obj_id, const char* attr_name, void* data);
herr_t H5LT_set_attribute_numerical(hid_t loc_id, const char* obj_name,
                                    const char* attr_name, size_t size,
                                    hid_t type_id, const void* data);
herr_t H5LT_set_attribute_string(hid_t dset_id, const char* name,
                                 const char* buf);
char* H5LT_dtype_to_text(hid_t dtype, char* dt_str, H5LT_lang_t lang,
                         size_t* slen, bool no_user_buf);
int H5LTyylex_destroy(void);
herr_t H5VL__reparse_def_vol_conn_variable_test(void);
}  // extern "C"

namespace {

using ReallocFn = void* (*)(void*, size_t);

// Pointer-tracked fault injection for H5LT.c realloc_and_append(), scoped
// strictly to the active dt_str heap block so gtest/absl/HDF5 reallocs pass
// through untouched.
std::atomic<void*> g_tracked_realloc_ptr{nullptr};
std::atomic<int> g_tracked_realloc_countdown{0};
std::atomic<int> g_tracked_realloc_failures{0};

void* InterceptTrackedRealloc(void* ptr, size_t size, ReallocFn real_fn) {
  if (ptr != nullptr &&
      g_tracked_realloc_ptr.load(std::memory_order_relaxed) == ptr) {
    if (g_tracked_realloc_countdown.fetch_sub(1, std::memory_order_relaxed) ==
        1) {
      g_tracked_realloc_ptr.store(nullptr, std::memory_order_relaxed);
      g_tracked_realloc_failures.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    void* moved = real_fn(ptr, size);
    g_tracked_realloc_ptr.store(moved, std::memory_order_relaxed);
    return moved;
  }
  return real_fn(ptr, size);
}

}  // namespace

extern "C" {

void* __real_realloc(void* ptr, size_t size);
void* __wrap_realloc(void* ptr, size_t size) {
  return InterceptTrackedRealloc(ptr, size, __real_realloc);
}

#ifdef __SANITIZE_ALLOC_TOKEN__
void* __real___alloc_token_0_realloc(void* ptr, size_t size);
void* __wrap___alloc_token_0_realloc(void* ptr, size_t size) {
  return InterceptTrackedRealloc(ptr, size, __real___alloc_token_0_realloc);
}

void* __real___alloc_token_1_realloc(void* ptr, size_t size);
void* __wrap___alloc_token_1_realloc(void* ptr, size_t size) {
  return InterceptTrackedRealloc(ptr, size, __real___alloc_token_1_realloc);
}

void* __real___alloc_token_9_realloc(void* ptr, size_t size);
void* __wrap___alloc_token_9_realloc(void* ptr, size_t size) {
  return InterceptTrackedRealloc(ptr, size, __real___alloc_token_9_realloc);
}
#endif  // __SANITIZE_ALLOC_TOKEN__

}  // extern "C"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::StrEq;

// Serializes an in-memory Core VFD file into a byte vector.
std::vector<uint8_t> ExtractCoreFileImage(hid_t file_id) {
  if (H5Fflush(file_id, H5F_SCOPE_GLOBAL) < 0) {
    return {};
  }
  const ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(size));
  if (H5Fget_file_image(file_id, image.data(), image.size()) != size) {
    return {};
  }
  return image;
}

std::string DtypeToText(hid_t tid) {
  size_t len = 0;
  if (H5LTdtype_to_text(tid, nullptr, H5LT_DDL, &len) < 0 || len == 0) {
    return "";
  }
  std::string out(len, '\0');
  if (H5LTdtype_to_text(tid, out.data(), H5LT_DDL, &len) < 0) {
    return "";
  }
  out.resize(std::strlen(out.c_str()));
  return out;
}

herr_t CollectDatatypeIdCallback(hid_t id, void* op_data) {
  static_cast<std::vector<hid_t>*>(op_data)->push_back(id);
  return 0;
}

std::vector<hid_t> SnapshotOpenDatatypeIds() {
  std::vector<hid_t> ids;
  H5Iiterate(H5I_DATATYPE, CollectDatatypeIdCallback, &ids);
  return ids;
}

void CloseNewDatatypeIds(const std::vector<hid_t>& baseline) {
  for (const hid_t id : SnapshotOpenDatatypeIds()) {
    if (std::ranges::find(baseline, id) == baseline.end()) {
      H5Tclose(id);
    }
  }
}

TEST(H5ltTest, MakeReadFindAndQueryDatasets) {
  ScopedFile file = CreateCoreFile();
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims1d = {4};
  constexpr std::array<hsize_t, 2> kDims2d = {2, 3};

  constexpr std::array<char, 4> kCIn = {'a', 'b', 'c', 'd'};
  constexpr std::array<short, 4> kSIn = {-10, 20, -30,
                                         40};  // NOLINT(runtime/int)
  constexpr std::array<int, 4> kIIn = {1, 2, 3, 4};
  constexpr std::array<long, 4> kLIn = {100L, 200L, 300L,
                                        400L};  // NOLINT(runtime/int)
  constexpr std::array<float, 4> kFIn = {1.5F, 2.5F, 3.5F, 4.5F};
  constexpr std::array<double, 4> kDIn = {10.25, 20.5, 30.75, 40.0};
  constexpr std::array<int, 6> kMIn = {10, 11, 12, 20, 21, 22};

  ASSERT_GE(H5LTmake_dataset(file, "d_generic", 2, kDims2d.data(),
                             H5T_NATIVE_INT, kMIn.data()),
            0);
  ASSERT_GE(H5LTmake_dataset(file, "d_nodata", 1, kDims1d.data(),
                             H5T_NATIVE_INT, nullptr),
            0);
  ASSERT_GE(
      H5LTmake_dataset_char(file, "d_char", 1, kDims1d.data(), kCIn.data()), 0);
  ASSERT_GE(
      H5LTmake_dataset_short(file, "d_short", 1, kDims1d.data(), kSIn.data()),
      0);
  ASSERT_GE(H5LTmake_dataset_int(file, "d_int", 1, kDims1d.data(), kIIn.data()),
            0);
  ASSERT_GE(
      H5LTmake_dataset_long(file, "d_long", 1, kDims1d.data(), kLIn.data()), 0);
  ASSERT_GE(
      H5LTmake_dataset_float(file, "d_float", 1, kDims1d.data(), kFIn.data()),
      0);
  ASSERT_GE(
      H5LTmake_dataset_double(file, "d_double", 1, kDims1d.data(), kDIn.data()),
      0);
  ASSERT_GE(H5LTmake_dataset_string(file, "d_str", "hdf5_lite"), 0);
  ASSERT_GE(H5LTmake_dataset_string(file, "d_empty_str", ""), 0);

  std::array<int, 6> m_out = {};
  std::array<char, 4> c_out = {};
  std::array<short, 4> s_out = {};  // NOLINT(runtime/int)
  std::array<int, 4> i_out = {};
  std::array<long, 4> l_out = {};  // NOLINT(runtime/int)
  std::array<float, 4> f_out = {};
  std::array<double, 4> d_out = {};
  std::array<char, 32> str_out = {};
  std::array<char, 8> empty_str_out = {'x', 'y', '\0'};

  ASSERT_GE(H5LTread_dataset(file, "d_generic", H5T_NATIVE_INT, m_out.data()),
            0);
  EXPECT_EQ(m_out, kMIn);
  ASSERT_GE(H5LTread_dataset_char(file, "d_char", c_out.data()), 0);
  EXPECT_EQ(c_out, kCIn);
  ASSERT_GE(H5LTread_dataset_short(file, "d_short", s_out.data()), 0);
  EXPECT_EQ(s_out, kSIn);
  ASSERT_GE(H5LTread_dataset_int(file, "d_int", i_out.data()), 0);
  EXPECT_EQ(i_out, kIIn);
  ASSERT_GE(H5LTread_dataset_long(file, "d_long", l_out.data()), 0);
  EXPECT_EQ(l_out, kLIn);
  ASSERT_GE(H5LTread_dataset_float(file, "d_float", f_out.data()), 0);
  EXPECT_EQ(f_out, kFIn);
  ASSERT_GE(H5LTread_dataset_double(file, "d_double", d_out.data()), 0);
  EXPECT_EQ(d_out, kDIn);
  ASSERT_GE(H5LTread_dataset_string(file, "d_str", str_out.data()), 0);
  EXPECT_THAT(str_out.data(), StrEq("hdf5_lite"));
  ASSERT_GE(H5LTread_dataset_string(file, "d_empty_str", empty_str_out.data()),
            0);
  EXPECT_THAT(empty_str_out.data(), StrEq(""));

  EXPECT_EQ(H5LTfind_dataset(file, "d_generic"), 1);
  EXPECT_EQ(H5LTfind_dataset(file, "does_not_exist"), 0);

  int rank = -1;
  ASSERT_GE(H5LTget_dataset_ndims(file, "d_generic", &rank), 0);
  EXPECT_EQ(rank, 2);

  std::array<hsize_t, 2> qdims = {};
  H5T_class_t tclass = H5T_NO_CLASS;
  size_t tsize = 0;
  ASSERT_GE(
      H5LTget_dataset_info(file, "d_generic", qdims.data(), &tclass, &tsize),
      0);
  EXPECT_THAT(qdims, ElementsAre(2, 3));
  EXPECT_EQ(tclass, H5T_INTEGER);
  EXPECT_EQ(tsize, sizeof(int));
  ASSERT_GE(H5LTget_dataset_info(file, "d_generic", nullptr, nullptr, nullptr),
            0);

  // Error branches.
  ScopedErrorSilence silence;
  EXPECT_LT(H5LTmake_dataset(file, nullptr, 1, kDims1d.data(), H5T_NATIVE_INT,
                             kIIn.data()),
            0);
  EXPECT_LT(H5LTmake_dataset(file, "bad_rank", -1, kDims1d.data(),
                             H5T_NATIVE_INT, kIIn.data()),
            0);
  EXPECT_LT(H5LTmake_dataset(H5I_INVALID_HID, "bad_loc", 1, kDims1d.data(),
                             H5T_NATIVE_INT, kIIn.data()),
            0);
  EXPECT_GE(
      H5LTmake_dataset_int(file, "null_data_int", 1, kDims1d.data(), nullptr),
      0);
  EXPECT_LT(H5LTmake_dataset_int(H5I_INVALID_HID, "bad_loc_int", 1,
                                 kDims1d.data(), kIIn.data()),
            0);
  EXPECT_LT(H5LTmake_dataset(file, "bad_tid", 1, kDims1d.data(),
                             H5I_INVALID_HID, kIIn.data()),
            0);
  EXPECT_LT(H5LTmake_dataset_string(file, nullptr, "x"), 0);
  EXPECT_LT(H5LTmake_dataset_string(H5I_INVALID_HID, "x", "y"), 0);
  EXPECT_LT(H5LTread_dataset(file, nullptr, H5T_NATIVE_INT, i_out.data()), 0);
  EXPECT_LT(H5LTread_dataset(file, "missing", H5T_NATIVE_INT, i_out.data()), 0);
  EXPECT_LT(H5LTread_dataset_int(file, "d_generic", nullptr), 0);
  EXPECT_LT(H5LTread_dataset_string(file, nullptr, str_out.data()), 0);
  EXPECT_LT(H5LTread_dataset_string(file, "missing", str_out.data()), 0);
  EXPECT_LT(H5LTget_dataset_ndims(file, nullptr, &rank), 0);
  EXPECT_LT(H5LTget_dataset_ndims(file, "missing", &rank), 0);
  EXPECT_LT(H5LTget_dataset_info(file, nullptr, qdims.data(), &tclass, &tsize),
            0);
  EXPECT_LT(
      H5LTget_dataset_info(file, "missing", qdims.data(), &tclass, &tsize), 0);
  EXPECT_LT(H5LTfind_dataset(H5I_INVALID_HID, "d_generic"), 0);
}

TEST(H5ltTest, SetGetFindAndQueryAttributes) {
  ScopedFile file = CreateCoreFile();
  ASSERT_TRUE(file.valid());
  constexpr std::array<hsize_t, 1> kDims = {2};
  constexpr std::array<int, 2> kSeed = {10, 20};
  ASSERT_GE(H5LTmake_dataset_int(file, "target", 1, kDims.data(), kSeed.data()),
            0);

  constexpr std::array<char, 2> kCIn = {'x', 'y'};
  constexpr std::array<unsigned char, 2> kUcIn = {1, 250};
  constexpr std::array<short, 2> kSIn = {-5, 5};  // NOLINT(runtime/int)
  constexpr std::array<unsigned short, 2> kUsIn = {
      10, 60000};  // NOLINT(runtime/int)
  constexpr std::array<int, 2> kIIn = {-100, 100};
  constexpr std::array<unsigned int, 2> kUiIn = {100U, 200U};
  constexpr std::array<long, 2> kLIn = {-1000L, 1000L};  // NOLINT(runtime/int)
  constexpr std::array<long long, 2> kLlIn = {-10000LL,
                                              10000LL};  // NOLINT(runtime/int)
  constexpr std::array<unsigned long, 2> kUlIn = {
      1000UL, 2000UL};  // NOLINT(runtime/int)
  constexpr std::array<unsigned long long, 2> kUllIn = {
      10000ULL, 20000ULL};  // NOLINT(runtime/int)
  constexpr std::array<float, 2> kFIn = {1.25F, 2.5F};
  constexpr std::array<double, 2> kDIn = {3.125, 6.25};

  ASSERT_GE(H5LTset_attribute_char(file, "target", "a_char", kCIn.data(), 2),
            0);
  ASSERT_GE(H5LTset_attribute_uchar(file, "target", "a_uchar", kUcIn.data(), 2),
            0);
  ASSERT_GE(H5LTset_attribute_short(file, "target", "a_short", kSIn.data(), 2),
            0);
  ASSERT_GE(
      H5LTset_attribute_ushort(file, "target", "a_ushort", kUsIn.data(), 2), 0);
  ASSERT_GE(H5LTset_attribute_int(file, "target", "a_int", kIIn.data(), 2), 0);
  // Overwrite existing numerical attribute to cover H5Adelete branch.
  ASSERT_GE(H5LTset_attribute_int(file, "target", "a_int", kIIn.data(), 2), 0);
  ASSERT_GE(H5LTset_attribute_uint(file, "target", "a_uint", kUiIn.data(), 2),
            0);
  ASSERT_GE(H5LTset_attribute_long(file, "target", "a_long", kLIn.data(), 2),
            0);
  ASSERT_GE(
      H5LTset_attribute_long_long(file, "target", "a_llong", kLlIn.data(), 2),
      0);
  ASSERT_GE(H5LTset_attribute_ulong(file, "target", "a_ulong", kUlIn.data(), 2),
            0);
  ASSERT_GE(
      H5LTset_attribute_ullong(file, "target", "a_ullong", kUllIn.data(), 2),
      0);
  ASSERT_GE(H5LTset_attribute_float(file, "target", "a_float", kFIn.data(), 2),
            0);
  ASSERT_GE(
      H5LTset_attribute_double(file, "target", "a_double", kDIn.data(), 2), 0);

  // Set and overwrite string attribute via public and private helpers.
  ASSERT_GE(H5LTset_attribute_string(file, "target", "a_str", "first"), 0);
  ASSERT_GE(H5LTset_attribute_string(file, "target", "a_str", "second"), 0);
  ASSERT_GE(H5LTset_attribute_string(file, "target", "a_empty", ""), 0);

  {
    ScopedDataset dset(H5Dopen2(file, "target", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5LT_set_attribute_string(dset, "p_str", "one"), 0);
    ASSERT_GE(H5LT_set_attribute_string(dset, "p_str", "two"), 0);
    EXPECT_EQ(H5LTfind_attribute(dset, "p_str"), 1);
    EXPECT_EQ(H5LTfind_attribute(dset, "missing_attr"), 0);
    std::array<char, 16> disk_str = {};
    ASSERT_GE(H5LT_get_attribute_disk(dset, "p_str", disk_str.data()), 0);
    EXPECT_THAT(disk_str.data(), StrEq("two"));
  }

  std::array<char, 2> c_out = {};
  std::array<unsigned char, 2> uc_out = {};
  std::array<short, 2> s_out = {};            // NOLINT(runtime/int)
  std::array<unsigned short, 2> us_out = {};  // NOLINT(runtime/int)
  std::array<int, 2> i_out = {};
  std::array<unsigned int, 2> ui_out = {};
  std::array<long, 2> l_out = {};                  // NOLINT(runtime/int)
  std::array<long long, 2> ll_out = {};            // NOLINT(runtime/int)
  std::array<unsigned long, 2> ul_out = {};        // NOLINT(runtime/int)
  std::array<unsigned long long, 2> ull_out = {};  // NOLINT(runtime/int)
  std::array<float, 2> f_out = {};
  std::array<double, 2> d_out = {};
  std::array<int, 2> gen_out = {};
  std::array<char, 16> str_out = {};
  std::array<char, 4> empty_out = {'x', '\0'};

  ASSERT_GE(H5LTget_attribute_char(file, "target", "a_char", c_out.data()), 0);
  EXPECT_EQ(c_out, kCIn);
  ASSERT_GE(H5LTget_attribute_uchar(file, "target", "a_uchar", uc_out.data()),
            0);
  EXPECT_EQ(uc_out, kUcIn);
  ASSERT_GE(H5LTget_attribute_short(file, "target", "a_short", s_out.data()),
            0);
  EXPECT_EQ(s_out, kSIn);
  ASSERT_GE(H5LTget_attribute_ushort(file, "target", "a_ushort", us_out.data()),
            0);
  EXPECT_EQ(us_out, kUsIn);
  ASSERT_GE(H5LTget_attribute_int(file, "target", "a_int", i_out.data()), 0);
  EXPECT_EQ(i_out, kIIn);
  ASSERT_GE(H5LTget_attribute_uint(file, "target", "a_uint", ui_out.data()), 0);
  EXPECT_EQ(ui_out, kUiIn);
  ASSERT_GE(H5LTget_attribute_long(file, "target", "a_long", l_out.data()), 0);
  EXPECT_EQ(l_out, kLIn);
  ASSERT_GE(
      H5LTget_attribute_long_long(file, "target", "a_llong", ll_out.data()), 0);
  EXPECT_EQ(ll_out, kLlIn);
  ASSERT_GE(H5LTget_attribute_ulong(file, "target", "a_ulong", ul_out.data()),
            0);
  EXPECT_EQ(ul_out, kUlIn);
  ASSERT_GE(
      H5LTget_attribute_ullong(file, "target", "a_ullong", ull_out.data()), 0);
  EXPECT_EQ(ull_out, kUllIn);
  ASSERT_GE(H5LTget_attribute_float(file, "target", "a_float", f_out.data()),
            0);
  EXPECT_EQ(f_out, kFIn);
  ASSERT_GE(H5LTget_attribute_double(file, "target", "a_double", d_out.data()),
            0);
  EXPECT_EQ(d_out, kDIn);
  ASSERT_GE(H5LTget_attribute(file, "target", "a_int", H5T_NATIVE_INT,
                              gen_out.data()),
            0);
  EXPECT_EQ(gen_out, kIIn);
  ASSERT_GE(H5LTget_attribute_string(file, "target", "a_str", str_out.data()),
            0);
  EXPECT_THAT(str_out.data(), StrEq("second"));
  ASSERT_GE(
      H5LTget_attribute_string(file, "target", "a_empty", empty_out.data()), 0);
  EXPECT_THAT(empty_out.data(), StrEq(""));

  int rank = -1;
  ASSERT_GE(H5LTget_attribute_ndims(file, "target", "a_int", &rank), 0);
  EXPECT_EQ(rank, 1);

  hsize_t adim = 0;
  H5T_class_t aclass = H5T_NO_CLASS;
  size_t asize = 0;
  ASSERT_GE(
      H5LTget_attribute_info(file, "target", "a_int", &adim, &aclass, &asize),
      0);
  EXPECT_EQ(adim, 2U);
  EXPECT_EQ(aclass, H5T_INTEGER);
  EXPECT_EQ(asize, sizeof(int));
  ASSERT_GE(
      H5LTget_attribute_info(file, "target", "a_int", nullptr, &aclass, &asize),
      0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5LTset_attribute_string(file, nullptr, "a", "v"), 0);
  EXPECT_LT(H5LTset_attribute_string(file, "target", nullptr, "v"), 0);
  EXPECT_LT(H5LTset_attribute_string(file, "target", "a", nullptr), 0);
  EXPECT_LT(H5LTset_attribute_string(file, "missing_obj", "a", "v"), 0);
  EXPECT_LT(H5LT_set_attribute_string(H5I_INVALID_HID, "k", "v"), 0);
  EXPECT_LT(H5LTset_attribute_int(file, nullptr, "a", kIIn.data(), 2), 0);
  EXPECT_LT(H5LTset_attribute_int(file, "target", nullptr, kIIn.data(), 2), 0);
  EXPECT_LT(H5LTset_attribute_int(file, "missing", "a", kIIn.data(), 2), 0);
  EXPECT_LT(H5LT_set_attribute_numerical(file, "target", "bad_tid", 1,
                                         H5I_INVALID_HID, kIIn.data()),
            0);
  EXPECT_LT(H5LTget_attribute_string(file, nullptr, "a_str", str_out.data()),
            0);
  EXPECT_LT(H5LTget_attribute_string(file, "target", nullptr, str_out.data()),
            0);
  EXPECT_LT(H5LTget_attribute_string(file, "missing", "a_str", str_out.data()),
            0);
  EXPECT_LT(H5LTget_attribute_string(file, "target", "missing", str_out.data()),
            0);
  EXPECT_LT(H5LTget_attribute_int(file, nullptr, "a_int", i_out.data()), 0);
  EXPECT_LT(H5LTget_attribute_int(file, "target", nullptr, i_out.data()), 0);
  EXPECT_LT(H5LTget_attribute_int(file, "missing", "a_int", i_out.data()), 0);
  EXPECT_LT(H5LTget_attribute_int(file, "target", "missing", i_out.data()), 0);
  EXPECT_LT(H5LTget_attribute(file, "target", "a_int", H5I_INVALID_HID,
                              gen_out.data()),
            0);
  EXPECT_LT(H5LT_get_attribute_disk(H5I_INVALID_HID, "a_str", str_out.data()),
            0);
  EXPECT_LT(H5LT_get_attribute_disk(file, "missing_root_attr", str_out.data()),
            0);
  EXPECT_LT(H5LTget_attribute_ndims(file, nullptr, "a_int", &rank), 0);
  EXPECT_LT(H5LTget_attribute_ndims(file, "target", nullptr, &rank), 0);
  EXPECT_LT(H5LTget_attribute_ndims(file, "missing", "a_int", &rank), 0);
  EXPECT_LT(H5LTget_attribute_ndims(file, "target", "missing", &rank), 0);
  EXPECT_LT(
      H5LTget_attribute_info(file, nullptr, "a_int", &adim, &aclass, &asize),
      0);
  EXPECT_LT(
      H5LTget_attribute_info(file, "target", nullptr, &adim, &aclass, &asize),
      0);
  EXPECT_LT(
      H5LTget_attribute_info(file, "missing", "a_int", &adim, &aclass, &asize),
      0);
  EXPECT_LT(
      H5LTget_attribute_info(file, "target", "missing", &adim, &aclass, &asize),
      0);
}

TEST(H5ltTest, OpenFileImageFlagsAndCallbacks) {
  ScopedFile src = CreateCoreFile();
  ASSERT_TRUE(src.valid());
  constexpr std::array<hsize_t, 1> kDims = {4};
  constexpr std::array<int, 4> kSeed = {11, 22, 33, 44};
  ASSERT_GE(H5LTmake_dataset_int(src, "seed", 1, kDims.data(), kSeed.data()),
            0);

  std::vector<uint8_t> image = ExtractCoreFileImage(src);
  ASSERT_GT(image.size(), 0U);

  // 1. Default flags (read-only copy).
  {
    ScopedFile ro_copy(H5LTopen_file_image(image.data(), image.size(), 0));
    ASSERT_TRUE(ro_copy.valid());
    std::array<int, 4> read_back = {};
    ASSERT_GE(H5LTread_dataset_int(ro_copy, "seed", read_back.data()), 0);
    EXPECT_EQ(read_back, kSeed);
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5LTmake_dataset_int(ro_copy, "fail", 1, kDims.data(), kSeed.data()),
        0);
  }

  // 2. OPEN_RW copy.
  {
    ScopedFile rw_copy(H5LTopen_file_image(image.data(), image.size(),
                                           H5LT_FILE_IMAGE_OPEN_RW));
    ASSERT_TRUE(rw_copy.valid());
    ASSERT_GE(
        H5LTmake_dataset_int(rw_copy, "added", 1, kDims.data(), kSeed.data()),
        0);
    std::array<int, 4> read_back = {};
    ASSERT_GE(H5LTread_dataset_int(rw_copy, "added", read_back.data()), 0);
    EXPECT_EQ(read_back, kSeed);
  }

  // 3. DONT_COPY | DONT_RELEASE (in-place read-only; caller keeps ownership).
  // Verify reading back data and querying the file's access property list.
  {
    ScopedFile ro_nocopy(H5LTopen_file_image(
        image.data(), image.size(),
        H5LT_FILE_IMAGE_DONT_COPY | H5LT_FILE_IMAGE_DONT_RELEASE));
    ASSERT_TRUE(ro_nocopy.valid());
    {
      ScopedPlist fapl_copy(H5Fget_access_plist(ro_nocopy));
      ASSERT_TRUE(fapl_copy.valid());
    }
    std::array<int, 4> read_back = {};
    ASSERT_GE(H5LTread_dataset_int(ro_nocopy, "seed", read_back.data()), 0);
    EXPECT_EQ(read_back, kSeed);
  }

  // 4. OPEN_RW | DONT_COPY (without DONT_RELEASE): HDF5 owns the malloc buffer
  // and exercises image_realloc on large expansion plus image_free on close.
  {
    void* owned_buf = std::malloc(image.size());
    ASSERT_NE(owned_buf, nullptr);
    std::memcpy(owned_buf, image.data(), image.size());
    ScopedFile rw_nocopy(H5LTopen_file_image(
        owned_buf, image.size(),
        H5LT_FILE_IMAGE_OPEN_RW | H5LT_FILE_IMAGE_DONT_COPY));
    if (!rw_nocopy.valid()) {
      std::free(owned_buf);
    }
    ASSERT_TRUE(rw_nocopy.valid());
    {
      ScopedPlist fapl_copy(H5Fget_access_plist(rw_nocopy));
      ASSERT_TRUE(fapl_copy.valid());
    }
    const std::vector<int> grow(32768, 77);
    const std::array<hsize_t, 1> grow_dims = {grow.size()};
    ASSERT_GE(H5LTmake_dataset_int(rw_nocopy, "big", 1, grow_dims.data(),
                                   grow.data()),
              0);
    ASSERT_GE(H5Fflush(rw_nocopy, H5F_SCOPE_GLOBAL), 0);
    std::vector<int> read_grow(grow.size(), 0);
    ASSERT_GE(H5LTread_dataset_int(rw_nocopy, "big", read_grow.data()), 0);
    EXPECT_EQ(read_grow, grow);
  }

  // 5. Image > 655360 bytes so alloc_incr uses the 10% percentage branch.
  {
    ScopedFile large_src = CreateCoreFile();
    ASSERT_TRUE(large_src.valid());
    const std::vector<int> big_vec(180000, 9);
    const std::array<hsize_t, 1> big_dims = {big_vec.size()};
    ASSERT_GE(H5LTmake_dataset_int(large_src, "payload", 1, big_dims.data(),
                                   big_vec.data()),
              0);
    std::vector<uint8_t> big_image = ExtractCoreFileImage(large_src);
    ASSERT_GT(big_image.size(), 655360U);
    ScopedFile big_ro(
        H5LTopen_file_image(big_image.data(), big_image.size(), 0));
    ASSERT_TRUE(big_ro.valid());
    std::vector<int> check(big_vec.size(), 0);
    ASSERT_GE(H5LTread_dataset_int(big_ro, "payload", check.data()), 0);
    EXPECT_EQ(check[0], 9);
    EXPECT_EQ(check.back(), 9);
  }

  // 6. Invalid argument and corrupt image error paths.
  ScopedErrorSilence silence;
  EXPECT_LT(H5LTopen_file_image(nullptr, image.size(), 0), 0);
  EXPECT_LT(H5LTopen_file_image(image.data(), 0, 0), 0);
  EXPECT_LT(H5LTopen_file_image(image.data(), image.size(), 0x100U), 0);
  std::array<uint8_t, 64> garbage = {1, 2, 3, 4};
  EXPECT_LT(H5LTopen_file_image(garbage.data(), garbage.size(), 0), 0);
}

TEST(H5ltTest, DtypeToTextAndTextToDtypeRoundTrips) {
  auto check_round_trip = [&](const char* ddl, const char* expected_substr) {
    const hid_t parsed = H5LTtext_to_dtype(ddl, H5LT_DDL);
    const absl::Cleanup cleanup_lex = [] { H5LTyylex_destroy(); };
    ASSERT_TRUE(H5Iis_valid(parsed) > 0 && H5Iget_type(parsed) == H5I_DATATYPE)
        << ddl;
    const ScopedDatatype dt(parsed);
    EXPECT_THAT(DtypeToText(dt), HasSubstr(expected_substr)) << ddl;
  };

  // All predefined integer and bitfield types recognized by H5LT_dtype_to_text.
  // On LP64 little-endian hosts, exact-width H5T_STD_* entries match before
  // H5T_NATIVE_* aliases.
  const std::array<std::pair<hid_t, const char*>, 27> kIntTypes = {{
      {H5T_STD_I8BE, "H5T_STD_I8BE"},      {H5T_STD_I8LE, "H5T_STD_I8LE"},
      {H5T_STD_I16BE, "H5T_STD_I16BE"},    {H5T_STD_I16LE, "H5T_STD_I16LE"},
      {H5T_STD_I32BE, "H5T_STD_I32BE"},    {H5T_STD_I32LE, "H5T_STD_I32LE"},
      {H5T_STD_I64BE, "H5T_STD_I64BE"},    {H5T_STD_I64LE, "H5T_STD_I64LE"},
      {H5T_STD_U8BE, "H5T_STD_U8BE"},      {H5T_STD_U8LE, "H5T_STD_U8LE"},
      {H5T_STD_U16BE, "H5T_STD_U16BE"},    {H5T_STD_U16LE, "H5T_STD_U16LE"},
      {H5T_STD_U32BE, "H5T_STD_U32BE"},    {H5T_STD_U32LE, "H5T_STD_U32LE"},
      {H5T_STD_U64BE, "H5T_STD_U64BE"},    {H5T_STD_U64LE, "H5T_STD_U64LE"},
      {H5T_NATIVE_SCHAR, "H5T_STD_I8LE"},  {H5T_NATIVE_UCHAR, "H5T_STD_U8LE"},
      {H5T_NATIVE_SHORT, "H5T_STD_I16LE"}, {H5T_NATIVE_USHORT, "H5T_STD_U16LE"},
      {H5T_NATIVE_INT, "H5T_STD_I32LE"},   {H5T_NATIVE_UINT, "H5T_STD_U32LE"},
      {H5T_NATIVE_LONG, "H5T_STD_I64LE"},  {H5T_NATIVE_ULONG, "H5T_STD_U64LE"},
      {H5T_NATIVE_LLONG, "H5T_STD_I64LE"}, {H5T_NATIVE_ULLONG, "H5T_STD_U64LE"},
      {H5T_STD_B8LE, "undefined integer"},
  }};
  for (const auto& [tid, expected] : kIntTypes) {
    EXPECT_EQ(expected, DtypeToText(tid)) << tid;
  }
  ScopedDatatype custom_int(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(custom_int.valid());
  ASSERT_GE(H5Tset_precision(custom_int, 24), 0);
  EXPECT_EQ("undefined integer", DtypeToText(custom_int));

  // Floats (standard IEEE, ML floats, FLOAT16, LDOUBLE, and custom float).
  const auto kFloatTypes = std::to_array<std::pair<hid_t, const char*>>({
      {H5T_IEEE_F16BE, "H5T_IEEE_F16BE"},
      {H5T_IEEE_F16LE, "H5T_IEEE_F16LE"},
      {H5T_IEEE_F32BE, "H5T_IEEE_F32BE"},
      {H5T_IEEE_F32LE, "H5T_IEEE_F32LE"},
      {H5T_IEEE_F64BE, "H5T_IEEE_F64BE"},
      {H5T_IEEE_F64LE, "H5T_IEEE_F64LE"},
      {H5T_FLOAT_BFLOAT16BE, "H5T_FLOAT_BFLOAT16BE"},
      {H5T_FLOAT_BFLOAT16LE, "H5T_FLOAT_BFLOAT16LE"},
      {H5T_FLOAT_F8E4M3, "H5T_FLOAT_F8E4M3"},
      {H5T_FLOAT_F8E5M2, "H5T_FLOAT_F8E5M2"},
      {H5T_FLOAT_F6E2M3, "H5T_FLOAT_F6E2M3"},
      {H5T_FLOAT_F6E3M2, "H5T_FLOAT_F6E3M2"},
      {H5T_FLOAT_F4E2M1, "H5T_FLOAT_F4E2M1"},
#ifdef H5_HAVE__FLOAT16
      {H5T_NATIVE_FLOAT16, "H5T_IEEE_F16LE"},
#endif
      {H5T_NATIVE_LDOUBLE, "H5T_NATIVE_LDOUBLE"},
      {H5T_NATIVE_FLOAT, "H5T_IEEE_F32LE"},
      {H5T_NATIVE_DOUBLE, "H5T_IEEE_F64LE"},
  });
  for (const auto& [tid, expected] : kFloatTypes) {
    EXPECT_EQ(expected, DtypeToText(tid)) << tid;
  }
  ScopedDatatype custom_float(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(custom_float.valid());
  ASSERT_GE(H5Tset_ebias(custom_float, 100), 0);
  EXPECT_EQ("undefined float", DtypeToText(custom_float));

  // Time, Reference, and Strings.
  EXPECT_EQ("H5T_TIME: not yet implemented", DtypeToText(H5T_UNIX_D32LE));
  EXPECT_THAT(DtypeToText(H5T_STD_REF_DSETREG),
              HasSubstr("H5T_STD_REF_DSETREG"));
  EXPECT_THAT(DtypeToText(H5T_STD_REF_OBJ), HasSubstr("H5T_STD_REF_OBJECT"));

  ScopedDatatype vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);
  ASSERT_GE(H5Tset_cset(vl_str, H5T_CSET_UTF8), 0);
  ASSERT_GE(H5Tset_strpad(vl_str, H5T_STR_NULLPAD), 0);
  EXPECT_THAT(DtypeToText(vl_str), HasSubstr("STRSIZE H5T_VARIABLE"));

  ScopedDatatype spacepad_str(H5Tcopy(H5T_FORTRAN_S1));
  ASSERT_TRUE(spacepad_str.valid());
  ASSERT_GE(H5Tset_size(spacepad_str, 12), 0);
  ASSERT_GE(H5Tset_strpad(spacepad_str, H5T_STR_SPACEPAD), 0);
  EXPECT_THAT(DtypeToText(spacepad_str), HasSubstr("H5T_STR_SPACEPAD"));

  ScopedDatatype ordered_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(ordered_str.valid());
  ASSERT_GE(H5Tset_size(ordered_str, 8), 0);
  ASSERT_GE(H5Tset_order(ordered_str, H5T_ORDER_BE), 0);
  EXPECT_THAT(DtypeToText(ordered_str), HasSubstr("CTYPE H5T_C_S1"));

  // Opaque (with tag and without tag).
  ScopedDatatype opq_tagged(H5Tcreate(H5T_OPAQUE, 16));
  ASSERT_TRUE(opq_tagged.valid());
  ASSERT_GE(H5Tset_tag(opq_tagged, "my_tag"), 0);
  EXPECT_THAT(DtypeToText(opq_tagged), HasSubstr("OPQ_TAG \"my_tag\""));
  ScopedDatatype opq_untagged(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opq_untagged.valid());
  EXPECT_THAT(DtypeToText(opq_untagged), HasSubstr("OPQ_TAG \"\""));

  // Enum (signed + unsigned, plus empty enum error branch).
  ScopedDatatype signed_enum(H5Tenum_create(H5T_STD_I32LE));
  ASSERT_TRUE(signed_enum.valid());
  int32_t sv = -1;
  ASSERT_GE(H5Tenum_insert(signed_enum, "NEG", &sv), 0);
  EXPECT_THAT(DtypeToText(signed_enum), HasSubstr("-1;"));

  ScopedDatatype unsigned_enum(H5Tenum_create(H5T_STD_U32LE));
  ASSERT_TRUE(unsigned_enum.valid());
  uint32_t uv = 42;
  ASSERT_GE(H5Tenum_insert(unsigned_enum, "FORTY_TWO", &uv), 0);
  EXPECT_THAT(DtypeToText(unsigned_enum), HasSubstr("42;"));

  // Array, VLEN (2-deep nesting; 22-deep is O(2^22) in H5LTdtype_to_text),
  // Complex, Nested Compound, and Wide Compound (>512 bytes triggers realloc).
  constexpr std::array<hsize_t, 2> kAdims = {2, 3};
  ScopedDatatype arr_tid(H5Tarray_create2(H5T_STD_I32LE, 2, kAdims.data()));
  ASSERT_TRUE(arr_tid.valid());
  EXPECT_THAT(DtypeToText(arr_tid), HasSubstr("[2][3]"));

  ScopedDatatype inner_vlen(H5Tvlen_create(H5T_STD_I16BE));
  ASSERT_TRUE(inner_vlen.valid());
  ScopedDatatype outer_vlen(H5Tvlen_create(inner_vlen));
  ASSERT_TRUE(outer_vlen.valid());
  EXPECT_THAT(DtypeToText(outer_vlen), HasSubstr("H5T_STD_I16BE"));

  ScopedDatatype wide_comp(H5Tcreate(H5T_COMPOUND, 24 * sizeof(int32_t)));
  ASSERT_TRUE(wide_comp.valid());
  for (int idx = 0; idx < 24; ++idx) {
    const std::string field = "member_long_name_padding_" + std::to_string(idx);
    ASSERT_GE(
        H5Tinsert(wide_comp, field.c_str(),
                  static_cast<size_t>(idx) * sizeof(int32_t), H5T_STD_I32LE),
        0);
  }
  const std::string wide_ddl = DtypeToText(wide_comp);
  EXPECT_GT(wide_ddl.size(), 1024U);

  ScopedDatatype cplx(H5Tcomplex_create(H5T_IEEE_F32LE));
  ASSERT_TRUE(cplx.valid());
  EXPECT_THAT(DtypeToText(cplx), HasSubstr("H5T_COMPLEX"));

  ScopedDatatype inner_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(inner_comp.valid());
  ASSERT_GE(H5Tinsert(inner_comp, "x", 0, H5T_STD_I32LE), 0);
  ASSERT_GE(H5Tinsert(inner_comp, "y", 4, H5T_IEEE_F32LE), 0);
  ScopedDatatype outer_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(outer_comp.valid());
  ASSERT_GE(H5Tinsert(outer_comp, "nested", 0, inner_comp), 0);
  EXPECT_THAT(DtypeToText(outer_comp), HasSubstr("\"nested\" : 0;"));

  // Undersized buffer truncation path.
  std::array<char, 8> tiny = {};
  size_t tiny_len = tiny.size();
  ASSERT_GE(H5LTdtype_to_text(outer_comp, tiny.data(), H5LT_DDL, &tiny_len), 0);
  EXPECT_THAT(tiny.data(), StrEq("H5T_COM"));

  // Round-trips through H5LTtext_to_dtype.
  check_round_trip("H5T_STD_I32LE", "H5T_STD_I32LE");
  check_round_trip(
      "H5T_STRING { STRSIZE 10; STRPAD H5T_STR_NULLTERM; CSET H5T_CSET_ASCII; "
      "CTYPE H5T_C_S1; }",
      "STRSIZE 10");
  check_round_trip(
      "H5T_COMPOUND { H5T_STD_I32LE \"id\" : 0; H5T_IEEE_F64LE \"v\" : 8; }",
      "\"v\" : 8;");
  check_round_trip("H5T_ENUM { H5T_STD_I32LE; \"ZERO\" 0; \"ONE\" 1; }",
                   "\"ONE\"");
  check_round_trip("H5T_ARRAY { [2][3] H5T_IEEE_F32LE }", "[2][3]");
  check_round_trip("H5T_VLEN { H5T_STD_U16LE }", "H5T_VLEN");
  check_round_trip("H5T_OPAQUE { OPQ_SIZE 8; OPQ_TAG \"tag\"; }",
                   "OPQ_TAG \"tag\"");

  // Invalid inputs & unsupported cases.
  // Note: When str == nullptr on an error path inside H5LT_dtype_to_text
  // (e.g. empty enum or H5LT_C), upstream H5LT.c:2153 overwrites text_str
  // with NULL and leaks the calloc'd buffer; pass a caller buffer here to
  // cover those error paths leak-free under LSan.
  ScopedErrorSilence silence;
  ScopedDatatype empty_enum(H5Tenum_create(H5T_STD_I32LE));
  ASSERT_TRUE(empty_enum.valid());
  std::array<char, 64> bad_buf = {};
  size_t elen = bad_buf.size();
  EXPECT_LT(H5LTdtype_to_text(empty_enum, bad_buf.data(), H5LT_DDL, &elen), 0);

  size_t bad_len = bad_buf.size();
  EXPECT_LT(H5LTdtype_to_text(H5T_STD_I32LE, nullptr, H5LT_LANG_ERR, &bad_len),
            0);
  EXPECT_LT(H5LTdtype_to_text(H5T_STD_I32LE, bad_buf.data(), H5LT_C, &bad_len),
            0);
  EXPECT_LT(H5LTdtype_to_text(H5T_STD_I32LE, nullptr, H5LT_NO_LANG, &bad_len),
            0);
  EXPECT_LT(
      H5LTdtype_to_text(H5I_INVALID_HID, bad_buf.data(), H5LT_DDL, &bad_len),
      0);
  EXPECT_EQ(H5LT_dtype_to_text(H5T_STD_I32LE, bad_buf.data(), H5LT_C, &bad_len,
                               false),
            nullptr);
  EXPECT_EQ(
      H5LT_dtype_to_text(H5T_STD_I32LE, nullptr, H5LT_DDL, &bad_len, true),
      nullptr);

  EXPECT_LT(H5LTtext_to_dtype(nullptr, H5LT_DDL), 0);
  EXPECT_LT(H5LTtext_to_dtype("H5T_STD_I32LE", H5LT_LANG_ERR), 0);
  EXPECT_LT(H5LTtext_to_dtype("H5T_STD_I32LE", H5LT_C), 0);
  {
    const hid_t bad_cplx =
        H5LTtext_to_dtype("H5T_COMPLEX { H5T_STD_I32LE }", H5LT_DDL);
    H5LTyylex_destroy();
    EXPECT_LT(bad_cplx, 0);
  }
}

TEST(H5ltTest, PathValidTraversalAndLinkStates) {
  ScopedFile file = CreateCoreFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup g1(
      H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ScopedGroup g2(H5Gcreate2(g1, "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g2.valid());

  constexpr std::array<hsize_t, 1> kDims = {1};
  constexpr int kVal = 42;
  ASSERT_GE(H5LTmake_dataset_int(file, "/grp/sub/ds", 1, kDims.data(), &kVal),
            0);
  ASSERT_GE(H5Lcreate_hard(file, "/grp/sub/ds", file, "hard_ds", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(
      H5Lcreate_soft("/grp/sub/ds", file, "soft_ok", H5P_DEFAULT, H5P_DEFAULT),
      0);
  ASSERT_GE(H5Lcreate_soft("/grp/missing_leaf", file, "soft_dangle",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  // Creating a dangling external link lets `H5LTpath_valid(..., "ext", true)`
  // probe external-link resolution (which issues a single failing read-only
  // open() syscall for "nonexistent_ext_file.h5" without creating any files).
  ASSERT_GE(H5Lcreate_external("nonexistent_ext_file.h5", "/ds", file, "ext",
                               H5P_DEFAULT, H5P_DEFAULT),
            0);

  EXPECT_GT(H5LTpath_valid(file, ".", false), 0);
  EXPECT_GT(H5LTpath_valid(file, ".", true), 0);
  EXPECT_GT(H5LTpath_valid(file, "/", false), 0);
  EXPECT_GT(H5LTpath_valid(file, "/", true), 0);
  EXPECT_GT(H5LTpath_valid(file, "/grp/sub/ds", false), 0);
  EXPECT_GT(H5LTpath_valid(file, "/grp/sub/ds", true), 0);
  EXPECT_GT(H5LTpath_valid(file, "./grp/sub/ds", true), 0);
  EXPECT_GT(H5LTpath_valid(file, "hard_ds", true), 0);
  EXPECT_GT(H5LTpath_valid(file, "soft_ok", true), 0);

  EXPECT_EQ(H5LTpath_valid(file, "/grp/missing", false), 0);
  EXPECT_EQ(H5LTpath_valid(file, "/no_grp/sub/ds", false), 0);
  EXPECT_GT(H5LTpath_valid(file, "soft_dangle", false), 0);
  EXPECT_EQ(H5LTpath_valid(file, "soft_dangle", true), 0);
  EXPECT_EQ(H5LTpath_valid(file, "soft_dangle/child", true), 0);
  EXPECT_GT(H5LTpath_valid(file, "ext", false), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5LTpath_valid(file, "/grp/sub/ds/beyond", true), 0);
  // Attempts a read-only open() on "nonexistent_ext_file.h5" and returns 0.
  EXPECT_EQ(H5LTpath_valid(file, "ext", true), 0);
  EXPECT_LT(H5LTpath_valid(file, nullptr, true), 0);
  EXPECT_LT(H5LTpath_valid(H5I_INVALID_HID, "/grp", true), 0);
}

TEST(H5ltTest, TypedAttributeWrapperErrorsAndUnlimitedSpace) {
  ScopedFile file = CreateCoreFile();
  ASSERT_TRUE(file.valid());
  ScopedErrorSilence silence;

  constexpr char kCIn = 'x';
  constexpr unsigned char kUcIn = 1;
  constexpr short kSIn = 1;            // NOLINT(runtime/int)
  constexpr unsigned short kUsIn = 1;  // NOLINT(runtime/int)
  constexpr int kIIn = 1;
  constexpr unsigned int kUiIn = 1U;
  constexpr long kLIn = 1L;                    // NOLINT(runtime/int)
  constexpr long long kLlIn = 1LL;             // NOLINT(runtime/int)
  constexpr unsigned long kUlIn = 1UL;         // NOLINT(runtime/int)
  constexpr unsigned long long kUllIn = 1ULL;  // NOLINT(runtime/int)
  constexpr float kFIn = 1.0F;
  constexpr double kDIn = 1.0;

  EXPECT_LT(H5LTset_attribute_char(file, "missing", "a", &kCIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_uchar(file, "missing", "a", &kUcIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_short(file, "missing", "a", &kSIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_ushort(file, "missing", "a", &kUsIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_int(file, "missing", "a", &kIIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_uint(file, "missing", "a", &kUiIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_long(file, "missing", "a", &kLIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_long_long(file, "missing", "a", &kLlIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_ulong(file, "missing", "a", &kUlIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_ullong(file, "missing", "a", &kUllIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_float(file, "missing", "a", &kFIn, 1), 0);
  EXPECT_LT(H5LTset_attribute_double(file, "missing", "a", &kDIn, 1), 0);

  char c_out = 0;
  unsigned char uc_out = 0;
  short s_out = 0;            // NOLINT(runtime/int)
  unsigned short us_out = 0;  // NOLINT(runtime/int)
  int i_out = 0;
  unsigned int ui_out = 0;
  long l_out = 0;                  // NOLINT(runtime/int)
  long long ll_out = 0;            // NOLINT(runtime/int)
  unsigned long ul_out = 0;        // NOLINT(runtime/int)
  unsigned long long ull_out = 0;  // NOLINT(runtime/int)
  float f_out = 0.0F;
  double d_out = 0.0;

  EXPECT_LT(H5LTget_attribute_char(file, "/", "missing", &c_out), 0);
  EXPECT_LT(H5LTget_attribute_uchar(file, "/", "missing", &uc_out), 0);
  EXPECT_LT(H5LTget_attribute_short(file, "/", "missing", &s_out), 0);
  EXPECT_LT(H5LTget_attribute_ushort(file, "/", "missing", &us_out), 0);
  EXPECT_LT(H5LTget_attribute_int(file, "/", "missing", &i_out), 0);
  EXPECT_LT(H5LTget_attribute_uint(file, "/", "missing", &ui_out), 0);
  EXPECT_LT(H5LTget_attribute_long(file, "/", "missing", &l_out), 0);
  EXPECT_LT(H5LTget_attribute_long_long(file, "/", "missing", &ll_out), 0);
  EXPECT_LT(H5LTget_attribute_ulong(file, "/", "missing", &ul_out), 0);
  EXPECT_LT(H5LTget_attribute_ullong(file, "/", "missing", &ull_out), 0);
  EXPECT_LT(H5LTget_attribute_float(file, "/", "missing", &f_out), 0);
  EXPECT_LT(H5LTget_attribute_double(file, "/", "missing", &d_out), 0);
  EXPECT_LT(H5LTget_attribute(file, "/", "missing", H5T_NATIVE_INT, &i_out), 0);

  // H5S_UNLIMITED triggers H5Screate_simple failure inside
  // H5LT_set_attribute_numerical.
  EXPECT_LT(H5LT_set_attribute_numerical(file, "/", "unlim",
                                         static_cast<size_t>(H5S_UNLIMITED),
                                         H5T_NATIVE_INT, &kIIn),
            0);
}

int g_i24_conv_calls = 0;
int g_i24_fail_on_call = 0;

herr_t FailI24Conv(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                   size_t /*nelmts*/, size_t /*buf_stride*/,
                   size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                   hid_t /*dset_xfer_plist*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
    case H5T_CONV_FREE:
      return 0;
    case H5T_CONV_CONV:
      ++g_i24_conv_calls;
      if (g_i24_fail_on_call == 0 || g_i24_conv_calls == g_i24_fail_on_call) {
        return -1;
      }
      return 0;
    default:
      return -1;
  }
}

TEST(H5ltTest, DtypeToTextEdgeCasesAndEnumConvFailure) {
  // String endianness/padding branches: match after order adjustment or fall
  // through to unknown_one_character_type when pad differs from H5T_C_S1 and
  // H5T_FORTRAN_S1.
  {
    ScopedDatatype c_le(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(c_le.valid());
    ASSERT_GE(H5Tset_size(c_le, 6), 0);
    ASSERT_GE(H5Tset_order(c_le, H5T_ORDER_LE), 0);
    EXPECT_THAT(DtypeToText(c_le), HasSubstr("CTYPE H5T_C_S1"));

    ScopedDatatype f_le(H5Tcopy(H5T_FORTRAN_S1));
    ASSERT_TRUE(f_le.valid());
    ASSERT_GE(H5Tset_size(f_le, 6), 0);
    ASSERT_GE(H5Tset_order(f_le, H5T_ORDER_LE), 0);
    EXPECT_THAT(DtypeToText(f_le), HasSubstr("STRSIZE 6"));

    ScopedDatatype f_be(H5Tcopy(H5T_FORTRAN_S1));
    ASSERT_TRUE(f_be.valid());
    ASSERT_GE(H5Tset_size(f_be, 6), 0);
    ASSERT_GE(H5Tset_order(f_be, H5T_ORDER_BE), 0);
    EXPECT_THAT(DtypeToText(f_be), HasSubstr("STRSIZE 6"));

    ScopedErrorSilence silence;
    for (const H5T_order_t order : {H5T_ORDER_LE, H5T_ORDER_BE}) {
      ScopedDatatype c_pad(H5Tcopy(H5T_C_S1));
      ASSERT_TRUE(c_pad.valid());
      ASSERT_GE(H5Tset_size(c_pad, 6), 0);
      ASSERT_GE(H5Tset_pad(c_pad, H5T_PAD_ONE, H5T_PAD_ONE), 0);
      ASSERT_GE(H5Tset_order(c_pad, order), 0);
      EXPECT_THAT(DtypeToText(c_pad),
                  HasSubstr("CTYPE unknown_one_character_type"));
      indent = 0;
    }
  }

  // Indentation >= 80 prints column limit message.
  {
    ScopedDatatype opq(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq.valid());
    const absl::Cleanup reset_indent = [] { indent = 0; };
    indent = 80;
    std::array<char, 256> buf = {};
    size_t len = buf.size();
    EXPECT_GE(H5LTdtype_to_text(opq, buf.data(), H5LT_DDL, &len), 0);
    EXPECT_THAT(buf.data(), HasSubstr("exceeds the number of cols"));
  }

  // realloc_and_append with no_user_buf = true and str_to_add = nullptr when
  // remaining capacity < LIMIT (512).
  {
    size_t slen = 16;
    char* raw = static_cast<char*>(std::calloc(slen, 1));
    ASSERT_NE(raw, nullptr);
    char* formatted =
        H5LT_dtype_to_text(H5T_STD_I32LE, raw, H5LT_DDL, &slen, true);
    ASSERT_NE(formatted, nullptr);
    EXPECT_THAT(formatted, StrEq("H5T_STD_I32LE"));
    std::free(formatted);
  }

  // Enum conversion failure in print_enum via bespoke hard converter on 3-byte
  // integer base type (unregistered cleanly before type closes).
  ScopedErrorSilence silence;
  ScopedDatatype i24(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(i24.valid());
  ASSERT_GE(H5Tset_size(i24, 3), 0);
  ASSERT_GE(H5Tset_precision(i24, 24), 0);
  g_i24_conv_calls = 0;
  g_i24_fail_on_call = 0;
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "lt_fail_i24", i24, H5T_NATIVE_INT,
                        FailI24Conv),
            0);
  const absl::Cleanup unregister_conv = [&] {
    g_i24_conv_calls = 0;
    g_i24_fail_on_call = 0;
    H5Tunregister(H5T_PERS_HARD, "lt_fail_i24", i24, H5T_NATIVE_INT,
                  FailI24Conv);
  };
  ScopedDatatype bad_enum(H5Tenum_create(i24));
  ASSERT_TRUE(bad_enum.valid());
  constexpr std::array<uint8_t, 3> kEnumVal = {1, 0, 0};
  ASSERT_GE(H5Tenum_insert(bad_enum, "A", kEnumVal.data()), 0);
  {
    std::array<char, 128> cbuf = {};
    size_t clen = cbuf.size();
    const absl::Cleanup reset_indent = [] { indent = 0; };
    EXPECT_LT(H5LTdtype_to_text(bad_enum, cbuf.data(), H5LT_DDL, &clen), 0);
  }

  // Exercise empty-enum and nested VLEN/ARRAY error paths with a caller buffer
  // (str != nullptr) and g_i24_fail_on_call = 2 so neither text_str
  // nor stmp heap buffers leak under HeapLeakChecker/LSan; sweep any `super`
  // datatype IDs leaked when print_vlen/print_array fail early.
  {
    const absl::Cleanup reset_indent = [] { indent = 0; };
    std::array<char, 128> cbuf = {};
    size_t clen = cbuf.size();

    ScopedDatatype empty_enum(H5Tenum_create(H5T_STD_I32LE));
    ASSERT_TRUE(empty_enum.valid());
    EXPECT_LT(H5LTdtype_to_text(empty_enum, cbuf.data(), H5LT_DDL, &clen), 0);
    indent = 0;

    ScopedDatatype vlen_bad(H5Tvlen_create(bad_enum));
    ASSERT_TRUE(vlen_bad.valid());
    constexpr std::array<hsize_t, 1> kAdims = {2};
    ScopedDatatype arr_bad(H5Tarray_create2(bad_enum, 1, kAdims.data()));
    ASSERT_TRUE(arr_bad.valid());

    const std::vector<hid_t> baseline_types = SnapshotOpenDatatypeIds();
    const absl::Cleanup close_super_leaks = [&] {
      CloseNewDatatypeIds(baseline_types);
    };
    for (hid_t composite : {vlen_bad.get(), arr_bad.get()}) {
      // Succeed on first call (size query), fail on second call (fill stmp).
      g_i24_conv_calls = 0;
      g_i24_fail_on_call = 2;
      clen = cbuf.size();
      EXPECT_LT(H5LTdtype_to_text(composite, cbuf.data(), H5LT_DDL, &clen), 0);
      indent = 0;
      CloseNewDatatypeIds(baseline_types);
    }
  }
}

// Mirrors H5LT_file_image_ud_t layout in third_party/hdf5/hl/src/H5LT.c.
struct FileImageUdataMirror {
  void* app_image_ptr;
  size_t app_image_size;
  void* fapl_image_ptr;
  size_t fapl_image_size;
  int fapl_ref_count;
  void* vfd_image_ptr;
  size_t vfd_image_size;
  int vfd_ref_count;
  unsigned flags;
  int ref_count;
};

struct LtVolFaultState {
  bool capture_image_fapl = false;
  bool has_captured_cb = false;
  H5FD_file_image_callbacks_t captured_cb = {};

  bool fail_dset_write = false;
  bool fail_dset_read = false;
  bool fail_dset_get_type = false;
  bool invalid_dset_get_type = false;
  bool immutable_dset_get_type = false;
  bool fail_dset_get_space = false;
  bool invalid_dset_get_space = false;
  bool fail_dset_close = false;

  bool fail_group_close = false;

  bool fail_attr_create = false;
  bool fail_attr_write = false;
  bool fail_attr_read = false;
  bool fail_attr_get_type = false;
  bool immutable_attr_get_type = false;
  bool fail_attr_get_space = false;
  bool invalid_attr_get_space = false;
  bool fail_attr_exists = false;
  bool fail_attr_delete = false;
  bool fail_attr_close = false;

  int link_exists_calls = 0;
  int fail_link_exists_call = 0;
  bool inject_null_link_iter_name = false;

  int object_exists_calls = 0;
  int fail_object_exists_call = 0;

  void ResetFaults() {
    fail_dset_write = false;
    fail_dset_read = false;
    fail_dset_get_type = false;
    invalid_dset_get_type = false;
    immutable_dset_get_type = false;
    fail_dset_get_space = false;
    invalid_dset_get_space = false;
    fail_dset_close = false;
    fail_group_close = false;
    fail_attr_create = false;
    fail_attr_write = false;
    fail_attr_read = false;
    fail_attr_get_type = false;
    immutable_attr_get_type = false;
    fail_attr_get_space = false;
    invalid_attr_get_space = false;
    fail_attr_exists = false;
    fail_attr_delete = false;
    fail_attr_close = false;
    link_exists_calls = 0;
    fail_link_exists_call = 0;
    inject_null_link_iter_name = false;
    object_exists_calls = 0;
    fail_object_exists_call = 0;
  }

  void ResetAll() {
    capture_image_fapl = false;
    has_captured_cb = false;
    captured_cb = {};
    ResetFaults();
  }
};

LtVolFaultState g_lt_vol_state;

void* LtVolAttrCreate(void* obj, const H5VL_loc_params_t* loc_params,
                      const char* attr_name, hid_t type_id, hid_t space_id,
                      hid_t acpl_id, hid_t aapl_id, hid_t dxpl_id, void** req) {
  if (g_lt_vol_state.fail_attr_create) {
    return nullptr;
  }
  return H5VLattr_create(obj, loc_params, H5VL_NATIVE, attr_name, type_id,
                         space_id, acpl_id, aapl_id, dxpl_id, req);
}

void* LtVolAttrOpen(void* obj, const H5VL_loc_params_t* loc_params,
                    const char* attr_name, hid_t aapl_id, hid_t dxpl_id,
                    void** req) {
  return H5VLattr_open(obj, loc_params, H5VL_NATIVE, attr_name, aapl_id,
                       dxpl_id, req);
}

herr_t LtVolAttrRead(void* attr, hid_t mem_type_id, void* buf, hid_t dxpl_id,
                     void** req) {
  if (g_lt_vol_state.fail_attr_read) {
    return -1;
  }
  return H5VLattr_read(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

herr_t LtVolAttrWrite(void* attr, hid_t mem_type_id, const void* buf,
                      hid_t dxpl_id, void** req) {
  if (g_lt_vol_state.fail_attr_write) {
    return -1;
  }
  return H5VLattr_write(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

herr_t LtVolAttrGet(void* attr, H5VL_attr_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  if (args->op_type == H5VL_ATTR_GET_TYPE) {
    if (g_lt_vol_state.fail_attr_get_type) {
      return -1;
    }
    if (g_lt_vol_state.immutable_attr_get_type) {
      args->args.get_type.type_id = H5T_NATIVE_INT;
      return 0;
    }
  } else if (args->op_type == H5VL_ATTR_GET_SPACE) {
    if (g_lt_vol_state.fail_attr_get_space) {
      return -1;
    }
    if (g_lt_vol_state.invalid_attr_get_space) {
      // 0 (H5P_DEFAULT) is never a valid object ID.
      args->args.get_space.space_id = H5P_DEFAULT;
      return 0;
    }
  }
  return H5VLattr_get(attr, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolAttrSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_attr_specific_args_t* args, hid_t dxpl_id,
                         void** req) {
  if (args->op_type == H5VL_ATTR_EXISTS && g_lt_vol_state.fail_attr_exists) {
    return -1;
  }
  if (args->op_type == H5VL_ATTR_DELETE && g_lt_vol_state.fail_attr_delete) {
    return -1;
  }
  return H5VLattr_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolAttrOptional(void* attr, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLattr_optional(attr, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolAttrClose(void* attr, hid_t dxpl_id, void** req) {
  if (g_lt_vol_state.fail_attr_close) {
    return -1;
  }
  return H5VLattr_close(attr, H5VL_NATIVE, dxpl_id, req);
}

void* LtVolDatasetCreate(void* obj, const H5VL_loc_params_t* loc_params,
                         const char* name, hid_t lcpl_id, hid_t type_id,
                         hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                         hid_t dxpl_id, void** req) {
  return H5VLdataset_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id,
                            type_id, space_id, dcpl_id, dapl_id, dxpl_id, req);
}

void* LtVolDatasetOpen(void* obj, const H5VL_loc_params_t* loc_params,
                       const char* name, hid_t dapl_id, hid_t dxpl_id,
                       void** req) {
  return H5VLdataset_open(obj, loc_params, H5VL_NATIVE, name, dapl_id, dxpl_id,
                          req);
}

herr_t LtVolDatasetRead(size_t count, void* dset[], hid_t mem_type_id[],
                        hid_t mem_space_id[], hid_t file_space_id[],
                        hid_t dxpl_id, void* buf[], void** req) {
  if (g_lt_vol_state.fail_dset_read) {
    return -1;
  }
  return H5VLdataset_read(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                          file_space_id, dxpl_id, buf, req);
}

herr_t LtVolDatasetWrite(size_t count, void* dset[], hid_t mem_type_id[],
                         hid_t mem_space_id[], hid_t file_space_id[],
                         hid_t dxpl_id, const void* buf[], void** req) {
  if (g_lt_vol_state.fail_dset_write) {
    return -1;
  }
  return H5VLdataset_write(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                           file_space_id, dxpl_id, buf, req);
}

herr_t LtVolDatasetGet(void* dset, H5VL_dataset_get_args_t* args, hid_t dxpl_id,
                       void** req) {
  if (args->op_type == H5VL_DATASET_GET_TYPE) {
    if (g_lt_vol_state.fail_dset_get_type) {
      return -1;
    }
    if (g_lt_vol_state.invalid_dset_get_type) {
      // 0 (H5P_DEFAULT) is never a valid object ID.
      args->args.get_type.type_id = H5P_DEFAULT;
      return 0;
    }
    if (g_lt_vol_state.immutable_dset_get_type) {
      args->args.get_type.type_id = H5T_NATIVE_INT;
      return 0;
    }
  } else if (args->op_type == H5VL_DATASET_GET_SPACE) {
    if (g_lt_vol_state.fail_dset_get_space) {
      return -1;
    }
    if (g_lt_vol_state.invalid_dset_get_space) {
      // 0 (H5P_DEFAULT) is never a valid object ID.
      args->args.get_space.space_id = H5P_DEFAULT;
      return 0;
    }
  }
  return H5VLdataset_get(dset, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolDatasetSpecific(void* obj, H5VL_dataset_specific_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolDatasetOptional(void* obj, H5VL_optional_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolDatasetClose(void* dset, hid_t dxpl_id, void** req) {
  const herr_t status = H5VLdataset_close(dset, H5VL_NATIVE, dxpl_id, req);
  if (g_lt_vol_state.fail_dset_close) {
    return -1;
  }
  return status;
}

void* LtVolDatatypeCommit(void* obj, const H5VL_loc_params_t* loc_params,
                          const char* name, hid_t type_id, hid_t lcpl_id,
                          hid_t tcpl_id, hid_t tapl_id, hid_t dxpl_id,
                          void** req) {
  return H5VLdatatype_commit(obj, loc_params, H5VL_NATIVE, name, type_id,
                             lcpl_id, tcpl_id, tapl_id, dxpl_id, req);
}

void* LtVolDatatypeOpen(void* obj, const H5VL_loc_params_t* loc_params,
                        const char* name, hid_t tapl_id, hid_t dxpl_id,
                        void** req) {
  return H5VLdatatype_open(obj, loc_params, H5VL_NATIVE, name, tapl_id, dxpl_id,
                           req);
}

herr_t LtVolDatatypeGet(void* dt, H5VL_datatype_get_args_t* args, hid_t dxpl_id,
                        void** req) {
  return H5VLdatatype_get(dt, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolDatatypeSpecific(void* obj, H5VL_datatype_specific_args_t* args,
                             hid_t dxpl_id, void** req) {
  return H5VLdatatype_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolDatatypeOptional(void* obj, H5VL_optional_args_t* args,
                             hid_t dxpl_id, void** req) {
  return H5VLdatatype_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolDatatypeClose(void* dt, hid_t dxpl_id, void** req) {
  return H5VLdatatype_close(dt, H5VL_NATIVE, dxpl_id, req);
}

void* LtVolFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                      hid_t fapl_id, hid_t dxpl_id, void** req) {
  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  if (!under_fapl.valid() || H5Pset_vol(under_fapl, H5VL_NATIVE, nullptr) < 0) {
    return nullptr;
  }
  return H5VLfile_create(name, flags, fcpl_id, under_fapl, dxpl_id, req);
}

void* LtVolFileOpen(const char* name, unsigned flags, hid_t fapl_id,
                    hid_t dxpl_id, void** req) {
  if (g_lt_vol_state.capture_image_fapl) {
    H5FD_file_image_callbacks_t cb = {};
    if (H5Pget_file_image_callbacks(fapl_id, &cb) >= 0) {
      if (cb.udata != nullptr && cb.udata_free != nullptr) {
        cb.udata_free(cb.udata);
        cb.udata = nullptr;
      }
      g_lt_vol_state.captured_cb = cb;
      g_lt_vol_state.has_captured_cb = true;
    }
  }
  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  if (!under_fapl.valid() || H5Pset_vol(under_fapl, H5VL_NATIVE, nullptr) < 0) {
    return nullptr;
  }
  return H5VLfile_open(name, flags, under_fapl, dxpl_id, req);
}

herr_t LtVolFileGet(void* file, H5VL_file_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  return H5VLfile_get(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolFileSpecific(void* obj, H5VL_file_specific_args_t* args,
                         hid_t dxpl_id, void** req) {
  return H5VLfile_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolFileOptional(void* file, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLfile_optional(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolFileClose(void* file, hid_t dxpl_id, void** req) {
  return H5VLfile_close(file, H5VL_NATIVE, dxpl_id, req);
}

void* LtVolGroupCreate(void* obj, const H5VL_loc_params_t* loc_params,
                       const char* name, hid_t lcpl_id, hid_t gcpl_id,
                       hid_t gapl_id, hid_t dxpl_id, void** req) {
  return H5VLgroup_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id, gcpl_id,
                          gapl_id, dxpl_id, req);
}

void* LtVolGroupOpen(void* obj, const H5VL_loc_params_t* loc_params,
                     const char* name, hid_t gapl_id, hid_t dxpl_id,
                     void** req) {
  return H5VLgroup_open(obj, loc_params, H5VL_NATIVE, name, gapl_id, dxpl_id,
                        req);
}

herr_t LtVolGroupGet(void* obj, H5VL_group_get_args_t* args, hid_t dxpl_id,
                     void** req) {
  return H5VLgroup_get(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolGroupSpecific(void* obj, H5VL_group_specific_args_t* args,
                          hid_t dxpl_id, void** req) {
  return H5VLgroup_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolGroupOptional(void* obj, H5VL_optional_args_t* args, hid_t dxpl_id,
                          void** req) {
  return H5VLgroup_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolGroupClose(void* grp, hid_t dxpl_id, void** req) {
  if (g_lt_vol_state.fail_group_close) {
    return -1;
  }
  return H5VLgroup_close(grp, H5VL_NATIVE, dxpl_id, req);
}

herr_t LtVolLinkCreate(H5VL_link_create_args_t* args, void* obj,
                       const H5VL_loc_params_t* loc_params, hid_t lcpl_id,
                       hid_t lapl_id, hid_t dxpl_id, void** req) {
  return H5VLlink_create(args, obj, loc_params, H5VL_NATIVE, lcpl_id, lapl_id,
                         dxpl_id, req);
}

herr_t LtVolLinkCopy(void* src_obj, const H5VL_loc_params_t* loc_params1,
                     void* dst_obj, const H5VL_loc_params_t* loc_params2,
                     hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id, void** req) {
  return H5VLlink_copy(src_obj, loc_params1, dst_obj, loc_params2, H5VL_NATIVE,
                       lcpl_id, lapl_id, dxpl_id, req);
}

herr_t LtVolLinkMove(void* src_obj, const H5VL_loc_params_t* loc_params1,
                     void* dst_obj, const H5VL_loc_params_t* loc_params2,
                     hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id, void** req) {
  return H5VLlink_move(src_obj, loc_params1, dst_obj, loc_params2, H5VL_NATIVE,
                       lcpl_id, lapl_id, dxpl_id, req);
}

herr_t LtVolLinkGet(void* obj, const H5VL_loc_params_t* loc_params,
                    H5VL_link_get_args_t* args, hid_t dxpl_id, void** req) {
  return H5VLlink_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolLinkSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_link_specific_args_t* args, hid_t dxpl_id,
                         void** req) {
  if (args->op_type == H5VL_LINK_EXISTS) {
    ++g_lt_vol_state.link_exists_calls;
    if (g_lt_vol_state.fail_link_exists_call ==
        g_lt_vol_state.link_exists_calls) {
      return -1;
    }
  } else if (args->op_type == H5VL_LINK_ITER &&
             g_lt_vol_state.inject_null_link_iter_name &&
             args->args.iterate.op != nullptr) {
    H5L_info2_t dummy_info = {};
    args->args.iterate.op(H5I_INVALID_HID, nullptr, &dummy_info,
                          args->args.iterate.op_data);
  }
  return H5VLlink_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolLinkOptional(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLlink_optional(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

void* LtVolObjectOpen(void* obj, const H5VL_loc_params_t* loc_params,
                      H5I_type_t* opened_type, hid_t dxpl_id, void** req) {
  return H5VLobject_open(obj, loc_params, H5VL_NATIVE, opened_type, dxpl_id,
                         req);
}

herr_t LtVolObjectCopy(void* src_obj, const H5VL_loc_params_t* loc_params1,
                       const char* src_name, void* dst_obj,
                       const H5VL_loc_params_t* loc_params2,
                       const char* dst_name, hid_t ocpypl_id, hid_t lcpl_id,
                       hid_t dxpl_id, void** req) {
  return H5VLobject_copy(src_obj, loc_params1, src_name, dst_obj, loc_params2,
                         dst_name, H5VL_NATIVE, ocpypl_id, lcpl_id, dxpl_id,
                         req);
}

herr_t LtVolObjectGet(void* obj, const H5VL_loc_params_t* loc_params,
                      H5VL_object_get_args_t* args, hid_t dxpl_id, void** req) {
  return H5VLobject_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolObjectSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                           H5VL_object_specific_args_t* args, hid_t dxpl_id,
                           void** req) {
  if (args->op_type == H5VL_OBJECT_EXISTS) {
    ++g_lt_vol_state.object_exists_calls;
    if (g_lt_vol_state.fail_object_exists_call ==
        g_lt_vol_state.object_exists_calls) {
      return -1;
    }
  }
  return H5VLobject_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolObjectOptional(void* obj, const H5VL_loc_params_t* loc_params,
                           H5VL_optional_args_t* args, hid_t dxpl_id,
                           void** req) {
  return H5VLobject_optional(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t LtVolIntrospectGetConnCls(void* obj, H5VL_get_conn_lvl_t lvl,
                                 const H5VL_class_t** conn_cls) {
  return H5VLintrospect_get_conn_cls(obj, H5VL_NATIVE, lvl, conn_cls);
}

herr_t LtVolIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  return H5VLintrospect_get_cap_flags(nullptr, H5VL_NATIVE, cap_flags);
}

herr_t LtVolIntrospectOptQuery(void* obj, H5VL_subclass_t cls, int opt_type,
                               uint64_t* flags) {
  return H5VLintrospect_opt_query(obj, H5VL_NATIVE, cls, opt_type, flags);
}

herr_t LtVolBlobPut(void* obj, const void* buf, size_t size, void* blob_id,
                    void* ctx) {
  return H5VLblob_put(obj, H5VL_NATIVE, buf, size, blob_id, ctx);
}

herr_t LtVolBlobGet(void* obj, const void* blob_id, void* buf, size_t size,
                    void* ctx) {
  return H5VLblob_get(obj, H5VL_NATIVE, blob_id, buf, size, ctx);
}

herr_t LtVolBlobSpecific(void* obj, void* blob_id,
                         H5VL_blob_specific_args_t* args) {
  return H5VLblob_specific(obj, H5VL_NATIVE, blob_id, args);
}

herr_t LtVolTokenCmp(void* obj, const H5O_token_t* token1,
                     const H5O_token_t* token2, int* cmp_value) {
  return H5VLtoken_cmp(obj, H5VL_NATIVE, token1, token2, cmp_value);
}

herr_t LtVolTokenToStr(void* obj, H5I_type_t obj_type, const H5O_token_t* token,
                       char** token_str) {
  return H5VLtoken_to_str(obj, obj_type, H5VL_NATIVE, token, token_str);
}

herr_t LtVolTokenFromStr(void* obj, H5I_type_t obj_type, const char* token_str,
                         H5O_token_t* token) {
  return H5VLtoken_from_str(obj, obj_type, H5VL_NATIVE, token_str, token);
}

const H5VL_class_t kLtFaultVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(51007),
    .name = "lt_fault_vol",
    .conn_version = 1,
    .cap_flags = 0,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls = {},
    .attr_cls =
        {
            .create = LtVolAttrCreate,
            .open = LtVolAttrOpen,
            .read = LtVolAttrRead,
            .write = LtVolAttrWrite,
            .get = LtVolAttrGet,
            .specific = LtVolAttrSpecific,
            .optional = LtVolAttrOptional,
            .close = LtVolAttrClose,
        },
    .dataset_cls =
        {
            .create = LtVolDatasetCreate,
            .open = LtVolDatasetOpen,
            .read = LtVolDatasetRead,
            .write = LtVolDatasetWrite,
            .get = LtVolDatasetGet,
            .specific = LtVolDatasetSpecific,
            .optional = LtVolDatasetOptional,
            .close = LtVolDatasetClose,
        },
    .datatype_cls =
        {
            .commit = LtVolDatatypeCommit,
            .open = LtVolDatatypeOpen,
            .get = LtVolDatatypeGet,
            .specific = LtVolDatatypeSpecific,
            .optional = LtVolDatatypeOptional,
            .close = LtVolDatatypeClose,
        },
    .file_cls =
        {
            .create = LtVolFileCreate,
            .open = LtVolFileOpen,
            .get = LtVolFileGet,
            .specific = LtVolFileSpecific,
            .optional = LtVolFileOptional,
            .close = LtVolFileClose,
        },
    .group_cls =
        {
            .create = LtVolGroupCreate,
            .open = LtVolGroupOpen,
            .get = LtVolGroupGet,
            .specific = LtVolGroupSpecific,
            .optional = LtVolGroupOptional,
            .close = LtVolGroupClose,
        },
    .link_cls =
        {
            .create = LtVolLinkCreate,
            .copy = LtVolLinkCopy,
            .move = LtVolLinkMove,
            .get = LtVolLinkGet,
            .specific = LtVolLinkSpecific,
            .optional = LtVolLinkOptional,
        },
    .object_cls =
        {
            .open = LtVolObjectOpen,
            .copy = LtVolObjectCopy,
            .get = LtVolObjectGet,
            .specific = LtVolObjectSpecific,
            .optional = LtVolObjectOptional,
        },
    .introspect_cls =
        {
            .get_conn_cls = LtVolIntrospectGetConnCls,
            .get_cap_flags = LtVolIntrospectGetCapFlags,
            .opt_query = LtVolIntrospectOptQuery,
        },
    .request_cls = {},
    .blob_cls =
        {
            .put = LtVolBlobPut,
            .get = LtVolBlobGet,
            .specific = LtVolBlobSpecific,
            .optional = nullptr,
        },
    .token_cls =
        {
            .cmp = LtVolTokenCmp,
            .to_str = LtVolTokenToStr,
            .from_str = LtVolTokenFromStr,
        },
    .optional = nullptr,
};

void CleanupLtVolConnector(hid_t vol_id) {
  g_lt_vol_state.ResetAll();
  SweepOpenH5Ids();
  H5VLunregister_connector(vol_id);
}

TEST(H5ltTest, FileImageCallbacksAndVolFaultPropagation) {
  g_lt_vol_state.ResetAll();
  const hid_t vol_id = H5VLregister_connector(&kLtFaultVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  const absl::Cleanup cleanup_vol = [vol_id] { CleanupLtVolConnector(vol_id); };

  // 1. Capture the private H5LT file image callbacks by temporarily making
  // lt_fault_vol the default VOL during H5LTopen_file_image.
  std::vector<uint8_t> base_img;
  {
    ScopedFile src = CreateCoreFile();
    ASSERT_TRUE(src.valid());
    constexpr std::array<hsize_t, 1> kDims = {2};
    constexpr std::array<int, 2> kVals = {7, 8};
    ASSERT_GE(H5LTmake_dataset_int(src, "v", 1, kDims.data(), kVals.data()), 0);
    base_img = ExtractCoreFileImage(src);
    ASSERT_GT(base_img.size(), 0U);
  }

  {
    const char* prev_env = std::getenv("HDF5_VOL_CONNECTOR");
    const std::optional<std::string> prev_vol =
        prev_env != nullptr ? std::optional<std::string>(prev_env)
                            : std::nullopt;
    const absl::Cleanup restore_default_vol = [&prev_vol] {
      if (prev_vol.has_value()) {
        setenv("HDF5_VOL_CONNECTOR", prev_vol->c_str(), 1);
      } else {
        unsetenv("HDF5_VOL_CONNECTOR");
      }
      H5VL__reparse_def_vol_conn_variable_test();
    };
    ASSERT_EQ(setenv("HDF5_VOL_CONNECTOR", "lt_fault_vol", 1), 0);
    ASSERT_GE(H5VL__reparse_def_vol_conn_variable_test(), 0);
    g_lt_vol_state.capture_image_fapl = true;
    ScopedFile opened(H5LTopen_file_image(
        base_img.data(), base_img.size(),
        H5LT_FILE_IMAGE_DONT_COPY | H5LT_FILE_IMAGE_DONT_RELEASE));
    ASSERT_TRUE(opened.valid());
    g_lt_vol_state.capture_image_fapl = false;
  }
  ASSERT_TRUE(g_lt_vol_state.has_captured_cb);
  const H5FD_file_image_callbacks_t& cb = g_lt_vol_state.captured_cb;

  // 2. Drive every guard and switch branch in the captured file-image
  // callbacks using FileImageUdataMirror.
  {
    std::array<uint8_t, 64> app_buf = {};
    std::array<uint8_t, 64> dummy_buf = {};
    FileImageUdataMirror ud = {};

    // DONT_COPY unset guards across all 6 callbacks.
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);
    EXPECT_EQ(cb.image_memcpy(dummy_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);
    EXPECT_EQ(cb.image_realloc(dummy_buf.data(), 64,
                               H5FD_FILE_IMAGE_OP_FILE_RESIZE, &ud),
              nullptr);
    EXPECT_LT(cb.image_free(dummy_buf.data(),
                            H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE, &ud),
              0);
    EXPECT_EQ(cb.udata_copy(&ud), nullptr);
    EXPECT_LT(cb.udata_free(&ud), 0);

    // udata_copy / udata_free ref_count == 0 and normal refcounting.
    ud.flags = H5LT_FILE_IMAGE_DONT_COPY | H5LT_FILE_IMAGE_DONT_RELEASE;
    EXPECT_EQ(cb.udata_copy(&ud), nullptr);
    EXPECT_LT(cb.udata_free(&ud), 0);
    ud.ref_count = 2;
    EXPECT_EQ(cb.udata_copy(&ud), &ud);
    EXPECT_EQ(ud.ref_count, 3);
    EXPECT_GE(cb.udata_free(&ud), 0);
    EXPECT_GE(cb.udata_free(&ud), 0);
    EXPECT_EQ(ud.ref_count, 1);

    // image_malloc guards (PROPERTY_LIST_SET, COPY, GET, FILE_OPEN, default).
    ud = {};
    ud.flags = H5LT_FILE_IMAGE_DONT_COPY | H5LT_FILE_IMAGE_DONT_RELEASE;
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);  // app_image_ptr == nullptr
    ud.app_image_ptr = app_buf.data();
    ud.app_image_size = 64;
    EXPECT_EQ(cb.image_malloc(32, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);  // size mismatch
    ud.fapl_image_ptr = dummy_buf.data();
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);
    ud.fapl_image_ptr = nullptr;
    ud.fapl_image_size = 64;
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);
    ud.fapl_image_size = 0;
    ud.fapl_ref_count = 1;
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);
    ud.fapl_ref_count = 0;

    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY, &ud),
              nullptr);  // fapl_image_ptr == nullptr
    ud.fapl_image_ptr = app_buf.data();
    EXPECT_EQ(cb.image_malloc(32, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY, &ud),
              nullptr);  // fapl_image_size mismatch
    ud.fapl_image_size = 64;
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY, &ud),
              nullptr);  // fapl_ref_count == 0
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET, &ud),
              nullptr);
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_NO_OP, &ud), nullptr);

    ud.fapl_ref_count = 1;
    ud.vfd_image_ptr = dummy_buf.data();
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud), nullptr);
    ud.vfd_image_ptr = nullptr;
    ud.vfd_image_size = 64;
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud), nullptr);
    ud.vfd_image_size = 0;
    ud.vfd_ref_count = 1;
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud), nullptr);
    ud.vfd_ref_count = 0;
    ud.fapl_image_ptr = nullptr;
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud), nullptr);
    ud.fapl_image_ptr = app_buf.data();
    EXPECT_EQ(cb.image_malloc(32, H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud), nullptr);
    ud.fapl_ref_count = 0;
    EXPECT_EQ(cb.image_malloc(64, H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud), nullptr);

    // image_memcpy guards across all ops.
    ud = {};
    ud.flags = H5LT_FILE_IMAGE_DONT_COPY | H5LT_FILE_IMAGE_DONT_RELEASE;
    ud.app_image_ptr = app_buf.data();
    ud.app_image_size = 64;
    ud.fapl_image_ptr = app_buf.data();
    ud.fapl_image_size = 64;
    ud.fapl_ref_count = 1;
    EXPECT_EQ(cb.image_memcpy(dummy_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), dummy_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 32,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);
    ud.app_image_size = 32;
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);
    ud.app_image_size = 64;
    ud.fapl_ref_count = 0;
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET, &ud),
              nullptr);

    ud.fapl_ref_count = 1;
    EXPECT_EQ(cb.image_memcpy(dummy_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY, &ud),
              nullptr);
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), dummy_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY, &ud),
              nullptr);
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 32,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY, &ud),
              nullptr);
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY, &ud),
              nullptr);  // fapl_ref_count < 2

    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET, &ud),
              nullptr);
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_NO_OP, &ud),
              nullptr);

    ud.vfd_image_ptr = app_buf.data();
    ud.vfd_image_size = 64;
    ud.vfd_ref_count = 1;
    EXPECT_EQ(cb.image_memcpy(dummy_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud),
              nullptr);
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), dummy_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud),
              nullptr);
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 32,
                              H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud),
              nullptr);
    ud.fapl_image_size = 32;
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud),
              nullptr);
    ud.fapl_image_size = 64;
    ud.fapl_ref_count = 0;
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud),
              nullptr);
    ud.fapl_ref_count = 1;
    ud.vfd_ref_count = 2;
    EXPECT_EQ(cb.image_memcpy(app_buf.data(), app_buf.data(), 64,
                              H5FD_FILE_IMAGE_OP_FILE_OPEN, &ud),
              nullptr);

    // image_realloc guards + OOM branch.
    ud = {};
    ud.flags = H5LT_FILE_IMAGE_DONT_COPY | H5LT_FILE_IMAGE_DONT_RELEASE;
    EXPECT_EQ(cb.image_realloc(app_buf.data(), 128,
                               H5FD_FILE_IMAGE_OP_FILE_RESIZE, &ud),
              nullptr);  // DONT_RELEASE set
    ud.flags = H5LT_FILE_IMAGE_DONT_COPY;
    EXPECT_EQ(cb.image_realloc(app_buf.data(), 128,
                               H5FD_FILE_IMAGE_OP_FILE_RESIZE, &ud),
              nullptr);  // read-only
    ud.flags = H5LT_FILE_IMAGE_DONT_COPY | H5LT_FILE_IMAGE_OPEN_RW;
    ud.app_image_ptr = app_buf.data();
    ud.fapl_image_ptr = app_buf.data();
    ud.vfd_image_ptr = app_buf.data();
    ud.vfd_ref_count = 0;
    EXPECT_EQ(cb.image_realloc(dummy_buf.data(), 128,
                               H5FD_FILE_IMAGE_OP_FILE_RESIZE, &ud),
              nullptr);  // ptr mismatch
    EXPECT_EQ(cb.image_realloc(app_buf.data(), 128,
                               H5FD_FILE_IMAGE_OP_FILE_RESIZE, &ud),
              nullptr);  // vfd_ref_count != 1
    EXPECT_EQ(
        cb.image_realloc(app_buf.data(), 128, H5FD_FILE_IMAGE_OP_NO_OP, &ud),
        nullptr);  // wrong op

#if H5LT_TEST_ALLOW_OOM_AND_LEAKY_PATHS
    void* realloc_fail_buf = std::malloc(64);
    ASSERT_NE(realloc_fail_buf, nullptr);
    ud.app_image_ptr = realloc_fail_buf;
    ud.fapl_image_ptr = realloc_fail_buf;
    ud.vfd_image_ptr = realloc_fail_buf;
    ud.vfd_ref_count = 1;
    EXPECT_EQ(
        cb.image_realloc(realloc_fail_buf, static_cast<size_t>(1ULL << 62),
                         H5FD_FILE_IMAGE_OP_FILE_RESIZE, &ud),
        nullptr);
    EXPECT_EQ(ud.vfd_image_ptr, nullptr);
#endif  // H5LT_TEST_ALLOW_OOM_AND_LEAKY_PATHS

    // image_free guards + PROPERTY_LIST_CLOSE buffer release without leak.
    void* owned = std::malloc(64);
    ASSERT_NE(owned, nullptr);
    ud = {};
    ud.flags = H5LT_FILE_IMAGE_DONT_COPY;
    ud.app_image_ptr = owned;
    ud.fapl_image_ptr = owned;
    ud.fapl_ref_count = 1;
    EXPECT_LT(cb.image_free(dummy_buf.data(),
                            H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE, &ud),
              0);
    ud.fapl_ref_count = 0;
    EXPECT_LT(cb.image_free(owned, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE, &ud),
              0);
    ud.fapl_ref_count = 1;
    EXPECT_GE(cb.image_free(owned, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE, &ud),
              0);
    EXPECT_EQ(ud.fapl_image_ptr, nullptr);

    ud = {};
    ud.flags = H5LT_FILE_IMAGE_DONT_COPY | H5LT_FILE_IMAGE_DONT_RELEASE;
    ud.vfd_image_ptr = app_buf.data();
    ud.vfd_ref_count = 1;
    EXPECT_LT(
        cb.image_free(dummy_buf.data(), H5FD_FILE_IMAGE_OP_FILE_CLOSE, &ud), 0);
    ud.vfd_ref_count = 2;
    EXPECT_LT(cb.image_free(app_buf.data(), H5FD_FILE_IMAGE_OP_FILE_CLOSE, &ud),
              0);
    ud.vfd_ref_count = 1;
    ud.ref_count = 0;
    EXPECT_LT(cb.image_free(app_buf.data(), H5FD_FILE_IMAGE_OP_FILE_CLOSE, &ud),
              0);  // udata_free failure when ref_count == 0
    EXPECT_LT(cb.image_free(app_buf.data(), H5FD_FILE_IMAGE_OP_NO_OP, &ud), 0);
  }

  // 3. Inject VOL faults across dataset, attribute, and path operations.
  ScopedFile file = CreateCoreVolFile(vol_id, "lt_fault_core.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {2};
  constexpr std::array<int, 2> kIIn = {10, 20};
  std::array<int, 2> i_out = {};
  std::array<char, 16> str_out = {};
  ASSERT_GE(H5LTmake_dataset_int(file, "ds", 1, kDims.data(), kIIn.data()), 0);
  ASSERT_GE(H5LTmake_dataset_string(file, "sds", "hello"), 0);
  ASSERT_GE(H5LTset_attribute_int(file, "ds", "a_int", kIIn.data(), 2), 0);
  ASSERT_GE(H5LTset_attribute_string(file, "ds", "a_str", "world"), 0);
  ScopedGroup grp(
      H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ASSERT_GE(H5LTmake_dataset_int(grp, "sub", 1, kDims.data(), kIIn.data()), 0);
  ASSERT_GE(H5LTset_attribute_int(file, "grp", "a_int", kIIn.data(), 2), 0);
  ASSERT_GE(H5LTset_attribute_string(file, "grp", "a_str", "world"), 0);

  ScopedErrorSilence silence;
  auto clear_faults = [&] {
    g_lt_vol_state.ResetFaults();
    for (const auto [obj_type, keep_id] :
         std::array<std::pair<unsigned, hid_t>, 2>{
             {{H5F_OBJ_ATTR | H5F_OBJ_LOCAL, H5I_INVALID_HID},
              {H5F_OBJ_GROUP | H5F_OBJ_LOCAL, grp.get()}}}) {
      const ssize_t count = H5Fget_obj_count(file, obj_type);
      if (count <= 0) {
        continue;
      }
      std::vector<hid_t> ids(static_cast<size_t>(count), H5I_INVALID_HID);
      const ssize_t filled =
          H5Fget_obj_ids(file, obj_type, ids.size(), ids.data());
      for (ssize_t i = 0; i < filled; ++i) {
        if (ids[i] != keep_id) {
          if ((obj_type & H5F_OBJ_ATTR) != 0) {
            H5Aclose(ids[i]);
          } else {
            H5Gclose(ids[i]);
          }
        }
      }
    }
  };

  // H5LT_make_dataset_numerical / H5LTmake_dataset_string faults.
  g_lt_vol_state.fail_dset_write = true;
  EXPECT_LT(H5LTmake_dataset_int(file, "fail_w", 1, kDims.data(), kIIn.data()),
            0);
  clear_faults();

  g_lt_vol_state.fail_dset_close = true;
  EXPECT_LT(H5LTmake_dataset_int(file, "fail_c", 1, kDims.data(), kIIn.data()),
            0);
  clear_faults();

  g_lt_vol_state.fail_dset_write = true;
  EXPECT_LT(H5LTmake_dataset_string(file, "fail_sw", "abc"), 0);
  clear_faults();

  g_lt_vol_state.fail_dset_close = true;
  EXPECT_LT(H5LTmake_dataset_string(file, "fail_sc", "abc"), 0);
  clear_faults();

  // H5LT_read_dataset_numerical / H5LTread_dataset_string faults.
  g_lt_vol_state.fail_dset_close = true;
  EXPECT_LT(H5LTread_dataset_int(file, "ds", i_out.data()), 0);
  clear_faults();

  g_lt_vol_state.fail_dset_get_type = true;
  EXPECT_LT(H5LTread_dataset_string(file, "sds", str_out.data()), 0);
  clear_faults();

  g_lt_vol_state.invalid_dset_get_type = true;
  EXPECT_LT(H5LTread_dataset_string(file, "sds", str_out.data()), 0);
  clear_faults();

  g_lt_vol_state.fail_dset_read = true;
  EXPECT_LT(H5LTread_dataset_string(file, "sds", str_out.data()), 0);
  clear_faults();

  g_lt_vol_state.fail_dset_close = true;
  EXPECT_LT(H5LTread_dataset_string(file, "sds", str_out.data()), 0);
  clear_faults();

  // H5LTget_dataset_ndims / H5LTget_dataset_info / find_dataset faults.
  int ndims = -1;
  g_lt_vol_state.fail_dset_get_space = true;
  EXPECT_LT(H5LTget_dataset_ndims(file, "ds", &ndims), 0);
  clear_faults();

  g_lt_vol_state.invalid_dset_get_space = true;
  EXPECT_LT(H5LTget_dataset_ndims(file, "ds", &ndims), 0);
  clear_faults();

  g_lt_vol_state.fail_dset_close = true;
  EXPECT_LT(H5LTget_dataset_ndims(file, "ds", &ndims), 0);
  clear_faults();

  hsize_t qdim = 0;
  H5T_class_t qcls = H5T_NO_CLASS;
  size_t qsz = 0;
  g_lt_vol_state.fail_dset_get_space = true;
  EXPECT_LT(H5LTget_dataset_info(file, "ds", &qdim, &qcls, &qsz), 0);
  clear_faults();

  g_lt_vol_state.invalid_dset_get_space = true;
  EXPECT_LT(H5LTget_dataset_info(file, "ds", &qdim, &qcls, &qsz), 0);
  clear_faults();

  g_lt_vol_state.immutable_dset_get_type = true;
  EXPECT_LT(H5LTget_dataset_info(file, "ds", &qdim, &qcls, &qsz), 0);
  clear_faults();

  g_lt_vol_state.fail_dset_close = true;
  EXPECT_LT(H5LTget_dataset_info(file, "ds", &qdim, &qcls, &qsz), 0);
  clear_faults();

  g_lt_vol_state.inject_null_link_iter_name = true;
  EXPECT_EQ(1, H5LTfind_dataset(file, "ds"));
  clear_faults();

  // Attribute set/get faults.
  g_lt_vol_state.fail_attr_exists = true;
  EXPECT_LT(H5LTset_attribute_string(file, "ds", "a_str", "x"), 0);
  EXPECT_LT(H5LTset_attribute_int(file, "ds", "a_int", kIIn.data(), 2), 0);
  clear_faults();

  g_lt_vol_state.fail_attr_delete = true;
  EXPECT_LT(H5LTset_attribute_string(file, "ds", "a_str", "x"), 0);
  EXPECT_LT(H5LTset_attribute_int(file, "ds", "a_int", kIIn.data(), 2), 0);
  clear_faults();

  g_lt_vol_state.fail_attr_create = true;
  EXPECT_LT(H5LTset_attribute_string(file, "ds", "new_s", "x"), 0);
  EXPECT_LT(H5LTset_attribute_int(file, "ds", "new_i", kIIn.data(), 2), 0);
  clear_faults();

  g_lt_vol_state.fail_attr_close = true;
  EXPECT_LT(H5LTset_attribute_string(file, "ds", "a_str", "x"), 0);
  EXPECT_LT(H5LTset_attribute_int(file, "ds", "a_int", kIIn.data(), 2), 0);
  clear_faults();

  g_lt_vol_state.fail_group_close = true;
  EXPECT_LT(H5LTset_attribute_string(file, "grp", "a_str", "x"), 0);
  clear_faults();
  g_lt_vol_state.fail_group_close = true;
  EXPECT_LT(H5LTset_attribute_int(file, "grp", "a_int", kIIn.data(), 2), 0);
  clear_faults();
  g_lt_vol_state.fail_group_close = true;
  EXPECT_LT(H5LTget_attribute_string(file, "grp", "a_str", str_out.data()), 0);
  clear_faults();
  g_lt_vol_state.fail_group_close = true;
  EXPECT_LT(H5LTget_attribute_int(file, "grp", "a_int", i_out.data()), 0);
  clear_faults();
  g_lt_vol_state.fail_group_close = true;
  EXPECT_LT(H5LTget_attribute_ndims(file, "grp", "a_int", &ndims), 0);
  clear_faults();
  g_lt_vol_state.fail_group_close = true;
  EXPECT_LT(H5LTget_attribute_info(file, "grp", "a_int", &qdim, &qcls, &qsz),
            0);
  clear_faults();

#if H5LT_TEST_ALLOW_OOM_AND_LEAKY_PATHS
  // H5LTset_attribute_string/numerical jump to out: without closing attr_id,
  // space_id, or type_id (unlike H5LT_set_attribute_string), leaking under
  // LSan.
  g_lt_vol_state.fail_attr_write = true;
  EXPECT_LT(H5LTset_attribute_string(file, "ds", "a_str", "x"), 0);
  EXPECT_LT(H5LTset_attribute_int(file, "ds", "a_int", kIIn.data(), 2), 0);
  clear_faults();
#endif  // H5LT_TEST_ALLOW_OOM_AND_LEAKY_PATHS

  g_lt_vol_state.fail_attr_get_space = true;
  EXPECT_LT(H5LTget_attribute_ndims(file, "ds", "a_int", &ndims), 0);
  EXPECT_LT(H5LTget_attribute_info(file, "ds", "a_int", &qdim, &qcls, &qsz), 0);
  clear_faults();

  g_lt_vol_state.invalid_attr_get_space = true;
  EXPECT_LT(H5LTget_attribute_ndims(file, "ds", "a_int", &ndims), 0);
  EXPECT_LT(H5LTget_attribute_info(file, "ds", "a_int", &qdim, &qcls, &qsz), 0);
  clear_faults();

  g_lt_vol_state.fail_attr_close = true;
  EXPECT_LT(H5LTget_attribute_ndims(file, "ds", "a_int", &ndims), 0);
  EXPECT_LT(H5LTget_attribute_info(file, "ds", "a_int", &qdim, &qcls, &qsz), 0);
  EXPECT_LT(H5LTget_attribute_int(file, "ds", "a_int", i_out.data()), 0);
  clear_faults();

  g_lt_vol_state.immutable_attr_get_type = true;
  EXPECT_LT(H5LTget_attribute_info(file, "ds", "a_int", &qdim, &qcls, &qsz), 0);
  clear_faults();

  g_lt_vol_state.fail_attr_read = true;
  EXPECT_LT(H5LTget_attribute_int(file, "ds", "a_int", i_out.data()), 0);
  clear_faults();

  {
    ScopedDataset ds_id(H5Dopen2(file, "ds", H5P_DEFAULT));
    ASSERT_TRUE(ds_id.valid());

    g_lt_vol_state.fail_attr_get_type = true;
    EXPECT_LT(H5LT_get_attribute_disk(ds_id, "a_str", str_out.data()), 0);
    clear_faults();

    g_lt_vol_state.fail_attr_read = true;
    EXPECT_LT(H5LT_get_attribute_disk(ds_id, "a_str", str_out.data()), 0);
    clear_faults();

    g_lt_vol_state.immutable_attr_get_type = true;
    EXPECT_LT(H5LT_get_attribute_disk(ds_id, "a_str", str_out.data()), 0);
    clear_faults();

    g_lt_vol_state.fail_attr_close = true;
    EXPECT_LT(H5LT_get_attribute_disk(ds_id, "a_str", str_out.data()), 0);
    clear_faults();

    g_lt_vol_state.fail_attr_delete = true;
    EXPECT_LT(H5LT_set_attribute_string(ds_id, "a_str", "z"), 0);
    clear_faults();

    g_lt_vol_state.fail_attr_create = true;
    EXPECT_LT(H5LT_set_attribute_string(ds_id, "new_ds_s", "z"), 0);
    clear_faults();

    g_lt_vol_state.fail_attr_write = true;
    EXPECT_LT(H5LT_set_attribute_string(ds_id, "a_str", "z"), 0);
    clear_faults();

    g_lt_vol_state.fail_attr_close = true;
    EXPECT_LT(H5LT_set_attribute_string(ds_id, "a_str", "z"), 0);
    clear_faults();
  }

  // H5LTpath_valid link_exists and obj_exists failures in loop & leaf.
  g_lt_vol_state.fail_link_exists_call = 1;
  EXPECT_LT(H5LTpath_valid(file, "grp/sub", false), 0);
  clear_faults();

  g_lt_vol_state.fail_link_exists_call = 2;
  EXPECT_LT(H5LTpath_valid(file, "grp/sub", false), 0);
  clear_faults();

  g_lt_vol_state.fail_object_exists_call = 1;
  EXPECT_LT(H5LTpath_valid(file, "grp/sub", false), 0);
  clear_faults();

  g_lt_vol_state.fail_object_exists_call = 2;
  EXPECT_LT(H5LTpath_valid(file, "grp/sub", true), 0);
  clear_faults();
}

TEST(H5ltTest, ReallocAndAppendFailurePathsAcrossDtypeClasses) {
  ScopedErrorSilence silence;

  // Null caller buffer with no_user_buf = true fails immediately (H5LT.c:1924).
  size_t null_len = 1024;
  EXPECT_EQ(
      H5LT_dtype_to_text(H5T_STD_I32LE, nullptr, H5LT_DDL, &null_len, true),
      nullptr);

  ScopedDatatype c_le(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(c_le.valid());
  ASSERT_GE(H5Tset_size(c_le, 6), 0);
  ASSERT_GE(H5Tset_order(c_le, H5T_ORDER_LE), 0);

  ScopedDatatype c_be(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(c_be.valid());
  ASSERT_GE(H5Tset_size(c_be, 6), 0);
  ASSERT_GE(H5Tset_order(c_be, H5T_ORDER_BE), 0);

  ScopedDatatype c_pad_le(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(c_pad_le.valid());
  ASSERT_GE(H5Tset_size(c_pad_le, 6), 0);
  ASSERT_GE(H5Tset_pad(c_pad_le, H5T_PAD_ONE, H5T_PAD_ONE), 0);
  ASSERT_GE(H5Tset_order(c_pad_le, H5T_ORDER_LE), 0);

  ScopedDatatype opq(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opq.valid());
  ASSERT_GE(H5Tset_tag(opq, "t"), 0);

  ScopedDatatype enum1(H5Tenum_create(H5T_STD_I32LE));
  ASSERT_TRUE(enum1.valid());
  constexpr int32_t kE0 = 1;
  ASSERT_GE(H5Tenum_insert(enum1, "E0", &kE0), 0);

  ScopedDatatype vlen1(H5Tvlen_create(H5T_STD_I32LE));
  ASSERT_TRUE(vlen1.valid());

  constexpr std::array<hsize_t, 1> kAdims = {2};
  ScopedDatatype arr1(H5Tarray_create2(H5T_STD_I32LE, 1, kAdims.data()));
  ASSERT_TRUE(arr1.valid());

  // Use a 0-member compound type so realloc failure never triggers H5LT.c:2724
  // where mname would leak on mid-loop goto out.
  ScopedDatatype empty_comp(H5Tcreate(H5T_COMPOUND, 4));
  ASSERT_TRUE(empty_comp.valid());

  const std::vector<hid_t> baseline_types = SnapshotOpenDatatypeIds();
  const absl::Cleanup cleanup_state = [&] {
    g_tracked_realloc_ptr.store(nullptr, std::memory_order_relaxed);
    g_tracked_realloc_countdown.store(0, std::memory_order_relaxed);
    g_tracked_realloc_failures.store(0, std::memory_order_relaxed);
    indent = 0;
    CloseNewDatatypeIds(baseline_types);
  };

  const std::array<hid_t, 10> test_types = {
      H5T_C_S1,         c_le.get(),
      c_be.get(),       c_pad_le.get(),
      opq.get(),        enum1.get(),
      vlen1.get(),      arr1.get(),
      empty_comp.get(), H5T_COMPLEX_IEEE_F32LE,
  };

  for (const hid_t tid : test_types) {
    bool succeeded = false;
    int failures_for_type = 0;
    for (int fail_on = 1; fail_on <= 32; ++fail_on) {
      size_t slen = 1;
      char* raw = static_cast<char*>(std::calloc(1, 1));
      ASSERT_NE(raw, nullptr);
      indent = 2;
      g_tracked_realloc_failures.store(0, std::memory_order_relaxed);
      g_tracked_realloc_ptr.store(raw, std::memory_order_relaxed);
      g_tracked_realloc_countdown.store(fail_on, std::memory_order_relaxed);
      char* formatted = H5LT_dtype_to_text(tid, raw, H5LT_DDL, &slen, true);
      g_tracked_realloc_ptr.store(nullptr, std::memory_order_relaxed);
      g_tracked_realloc_countdown.store(0, std::memory_order_relaxed);
      const int fired =
          g_tracked_realloc_failures.exchange(0, std::memory_order_relaxed);
      failures_for_type += fired;
      indent = 0;
      CloseNewDatatypeIds(baseline_types);
      if (fail_on == 1) {
        EXPECT_EQ(nullptr, formatted);
        EXPECT_EQ(1, fired);
      }
      if (formatted != nullptr) {
        std::free(formatted);
        succeeded = true;
        break;
      }
    }
    EXPECT_TRUE(succeeded);
    EXPECT_GT(failures_for_type, 0);
  }
}

// TODO(schwehr): Remaining uncovered lines in H5LT.c are mainly defensive
// post-create/close/type error guards on internal HDF5 calls not exposed via
// public VOL callbacks and mid-format calloc/realloc OOM branches.

}  // namespace
}  // namespace hdf5
