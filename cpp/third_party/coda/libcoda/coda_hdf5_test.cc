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
// Tests CODA's HDF5 backend initialization, reopening, closing, and HDF5 error
// stack formatting (libcoda/coda-hdf5.c).
//
// google3 build caveats:
//   - HAVE_HDF5 is enabled (1) and config.h defines H5_USE_16_API, so tests
//     synthesize HDF5 files using versioned *2 APIs (H5Gcreate2, H5Dcreate2,
//     H5Acreate2).
//   - coda-hdf5.c:146-148 leaks strings returned by H5Eget_major() /
//     H5Eget_minor() in HDF5 2.1.x; intentional HDF5 error-stack triggers are
//     narrowly scoped with unconditional absl::LeakCheckDisabler (covering
//     both LSan and tcmalloc HeapLeakChecker).
//   - OOM error branches in coda_hdf5_reopen() (lines 53-56 for malloc() and
//     lines 74-78 for strdup()) are unreachable without allocator injection.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-hdf5.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-hdf5.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-hdf5-internal.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/hdf5_test_util.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin.h"
#include "third_party/stcorp_coda/libcoda/coda-hdf5-internal.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Not;

herr_t DummyHdf5AutoError(void* /*client_data*/) { return 0; }

// Creates a minimal empty HDF5 file (root group only, no attributes/datasets)
// so opening leaves product->mem_ptr == nullptr.
void CreateEmptyHdf5File(const std::filesystem::path& path) {
  ScopedH5File file(
      H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(file.valid());
}

// Creates a valid HDF5 file with an optional userblock, root string attribute,
// subgroup integer attribute, and a 1D scaled int32 dataset so that opening
// populates product->mem_ptr, product->object, and product->root_type.
void CreateValidHdf5File(const std::filesystem::path& path,
                         hsize_t userblock_size = 0) {
  ScopedH5Plist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  if (userblock_size > 0) {
    ASSERT_GE(H5Pset_userblock(fcpl.get(), userblock_size), 0);
  }
  ScopedH5File file(
      H5Fcreate(path.c_str(), H5F_ACC_TRUNC, fcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(file.valid());

  // Root group string attribute ("title" = "hdf5_test").
  ScopedH5Space scalar_space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar_space.valid());
  ScopedH5Type str_type(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_type.valid());
  constexpr char kTitle[] = "hdf5_test";
  ASSERT_GE(H5Tset_size(str_type.get(), sizeof(kTitle)), 0);
  ScopedH5Attr title_attr(H5Acreate2(file.get(), "title", str_type.get(),
                                     scalar_space.get(), H5P_DEFAULT,
                                     H5P_DEFAULT));
  ASSERT_TRUE(title_attr.valid());
  ASSERT_GE(H5Awrite(title_attr.get(), str_type.get(), kTitle), 0);

  // Subgroup "/grp" with scalar int32 attribute "count" = 42.
  ScopedH5Group grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedH5Attr count_attr(H5Acreate2(grp.get(), "count", H5T_NATIVE_INT32,
                                     scalar_space.get(), H5P_DEFAULT,
                                     H5P_DEFAULT));
  ASSERT_TRUE(count_attr.valid());
  constexpr int32_t kCount = 42;
  ASSERT_GE(H5Awrite(count_attr.get(), H5T_NATIVE_INT32, &kCount), 0);

  // Dataset "/grp/vals" with 3 int32 values [10, 20, 30] and scale_factor=2.0.
  constexpr std::array<hsize_t, 1> kDims = {3};
  ScopedH5Space dset_space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(dset_space.valid());
  ScopedH5Dataset dset(H5Dcreate2(grp.get(), "vals", H5T_NATIVE_INT32,
                                  dset_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  constexpr std::array<int32_t, 3> kValues = {10, 20, 30};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, kValues.data()),
            0);

  ScopedH5Attr scale_attr(H5Acreate2(dset.get(), "scale_factor",
                                     H5T_NATIVE_DOUBLE, scalar_space.get(),
                                     H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(scale_attr.valid());
  constexpr double kScale = 2.0;
  ASSERT_GE(H5Awrite(scale_attr.get(), H5T_NATIVE_DOUBLE, &kScale), 0);
}

// Creates a valid HDF5 file containing a long-double dataset, which
// coda_hdf5_create_tree() rejects with -1 (unsupported float size).
void CreateUnsupportedFloatHdf5File(const std::filesystem::path& path) {
  ScopedH5File file(
      H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(file.valid());
  constexpr std::array<hsize_t, 1> kDims = {1};
  ScopedH5Space space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedH5Dataset dset(H5Dcreate2(file.get(), "ld_val", H5T_NATIVE_LDOUBLE,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
}

std::vector<uint8_t> ReadFilePrefix(const std::filesystem::path& path,
                                    std::size_t max_bytes) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> buf(max_bytes);
  in.read(reinterpret_cast<char*>(buf.data()),
          static_cast<std::streamsize>(max_bytes));
  buf.resize(static_cast<std::size_t>(in.gcount()));
  return buf;
}

TEST(CodaHdf5Test, InitClearsHdf5AutoErrorHandlerDirectlyAndViaCodaInit) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();

  H5E_auto1_t saved_func = nullptr;
  void* saved_client_data = nullptr;
  ASSERT_GE(H5Eget_auto1(&saved_func, &saved_client_data), 0);
  const absl::Cleanup restore_auto_error = [&] {
    H5Eset_auto1(saved_func, saved_client_data);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Direct coda_hdf5_init() clears any installed HDF5 auto error handler.
  int dummy_cookie = 7;
  ASSERT_GE(H5Eset_auto1(DummyHdf5AutoError, &dummy_cookie), 0);
  EXPECT_EQ(0, coda_hdf5_init());

  H5E_auto1_t current_func = DummyHdf5AutoError;
  void* current_data = &dummy_cookie;
  ASSERT_GE(H5Eget_auto1(&current_func, &current_data), 0);
  EXPECT_EQ(nullptr, current_func);
  EXPECT_EQ(nullptr, current_data);

  // 2. coda_init() invokes coda_hdf5_init() on first initialization.
  ASSERT_GE(H5Eset_auto1(DummyHdf5AutoError, &dummy_cookie), 0);
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  ASSERT_GE(H5Eget_auto1(&current_func, &current_data), 0);
  EXPECT_EQ(nullptr, current_func);
  EXPECT_EQ(nullptr, current_data);
}

TEST(CodaHdf5Test, OpenRecognizeAndCloseValidHdf5Files) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Empty HDF5 file (no attributes -> mem_ptr == nullptr).
  {
    TempFile empty_h5("coda_hdf5_empty.h5");
    ASSERT_NO_FATAL_FAILURE(CreateEmptyHdf5File(empty_h5.path()));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(empty_h5.c_str(), product.OutParam()));
    const auto* hdf5_prod =
        reinterpret_cast<const coda_hdf5_product*>(product.get());
    EXPECT_GE(hdf5_prod->file_id, 0);
    EXPECT_EQ(nullptr, hdf5_prod->mem_ptr);
    EXPECT_EQ(hsize_t{1}, hdf5_prod->num_objects);
    EXPECT_EQ(CODA_SUCCESS, coda_close(product.release()));
  }

  // 2. Populated HDF5 files with 0-byte and 1024-byte userblocks.
  for (const hsize_t userblock : {hsize_t{0}, hsize_t{1024}}) {
    SCOPED_TRACE(userblock);
    TempFile h5_file(userblock == 0 ? "coda_hdf5_valid_0.h5"
                                    : "coda_hdf5_valid_1024.h5");
    ASSERT_NO_FATAL_FAILURE(CreateValidHdf5File(h5_file.path(), userblock));
    const auto expected_size =
        static_cast<int64_t>(std::filesystem::file_size(h5_file.path()));

    // coda_recognize_file detects HDF5 format and leaves class/type unset.
    int64_t file_size = -1;
    coda_format format = coda_format_binary;
    const char* product_class = "sentinel";
    const char* product_type = "sentinel";
    int version = 99;
    ASSERT_EQ(CODA_SUCCESS,
              coda_recognize_file(h5_file.c_str(), &file_size, &format,
                                  &product_class, &product_type, &version));
    EXPECT_EQ(expected_size, file_size);
    EXPECT_EQ(coda_format_hdf5, format);
    EXPECT_EQ(nullptr, product_class);
    EXPECT_EQ(nullptr, product_type);
    EXPECT_EQ(-1, version);

    // coda_open reopens via coda_hdf5_reopen and populates product tree.
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

    const char* open_filename = nullptr;
    int64_t open_size = -1;
    coda_format open_format = coda_format_binary;
    coda_type* root_type = nullptr;
    coda_type_class root_class = coda_special_class;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_filename(product.get(), &open_filename));
    EXPECT_STREQ(h5_file.c_str(), open_filename);
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_file_size(product.get(), &open_size));
    EXPECT_EQ(expected_size, open_size);
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_format(product.get(), &open_format));
    EXPECT_EQ(coda_format_hdf5, open_format);
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_root_type(product.get(), &root_type));
    ASSERT_NE(nullptr, root_type);
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(root_type, &root_class));
    EXPECT_EQ(coda_record_class, root_class);

    // Verify internal coda_hdf5_product state including mem_ptr from attrs.
    const auto* hdf5_prod =
        reinterpret_cast<const coda_hdf5_product*>(product.get());
    EXPECT_GE(hdf5_prod->file_id, 0);
    EXPECT_NE(nullptr, hdf5_prod->mem_ptr);
    EXPECT_NE(nullptr, hdf5_prod->object);
    EXPECT_EQ(hsize_t{3}, hdf5_prod->num_objects);

    // Traverse root attributes, subgroup attributes, and scaled dataset.
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "@title"));
    long str_len = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
    ASSERT_GT(str_len, 0);
    std::vector<char> title(static_cast<std::size_t>(str_len) + 1, '\0');
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&cursor, title.data(), str_len + 1));
    EXPECT_STREQ("hdf5_test", title.data());

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/grp@count"));
    int32_t count = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &count));
    EXPECT_EQ(42, count);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/grp/vals"));
    std::vector<double> vals(3, -999.0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &cursor, vals.data(), coda_array_ordering_c));
    EXPECT_THAT(vals, ElementsAre(20.0, 40.0, 60.0));

    // Close explicitly before session teardown.
    EXPECT_EQ(CODA_SUCCESS, coda_close(product.release()));
  }
}

TEST(CodaHdf5Test, OpenAndRecognizeHandleMissingNonHdf5AndCorruptHdf5Files) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    H5Eclear1();
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Missing file fails with CODA_ERROR_FILE_NOT_FOUND.
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) / "coda_hdf5_missing.h5";
  std::filesystem::remove(missing_path);

  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_recognize_file(missing_path.c_str(), nullptr, nullptr,
                                    nullptr, nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing_path.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 2. Non-HDF5 bytes fall back to binary and fail open without a codadef.
  TempFile non_hdf5("coda_hdf5_non_hdf5.dat");
  ASSERT_TRUE(non_hdf5.WriteText("NOT_AN_HDF5_FILE_PAYLOAD"));
  coda_format fmt = coda_format_hdf5;
  ASSERT_EQ(CODA_SUCCESS, coda_recognize_file(non_hdf5.c_str(), nullptr, &fmt,
                                              nullptr, nullptr, nullptr));
  EXPECT_EQ(coda_format_binary, fmt);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(non_hdf5.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 3. Corrupt/truncated HDF5 magic inputs:
  //    - bare_magic / truncated_64 / userblock_padded_magic trigger
  //      coda_format_hdf5 -> coda_hdf5_reopen() -> H5Fopen() failure with
  //      CODA_ERROR_HDF5 and "[HDF5]" error stack formatting.
  //    - userblock_magic is exactly 520 bytes (512 + 8), so get_format() in
  //      coda-product.c (while (offset + 8 < file_size)) treats it as
  //      coda_format_binary: coda_recognize_file returns 0 (binary) and
  //      coda_open returns -1 with CODA_ERROR_UNSUPPORTED_PRODUCT.
  TempFile seed_valid("coda_hdf5_seed_for_trunc.h5");
  ASSERT_NO_FATAL_FAILURE(CreateValidHdf5File(seed_valid.path()));
  std::vector<uint8_t> truncated_bytes = ReadFilePrefix(seed_valid.path(), 64);
  ASSERT_EQ(64u, truncated_bytes.size());

  constexpr std::array<uint8_t, 8> kHdf5Magic = {0x89, 'H',  'D',  'F',
                                                 '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> userblock_magic(520, 0);
  for (std::size_t i = 0; i < kHdf5Magic.size(); ++i) {
    userblock_magic[512 + i] = kHdf5Magic[i];
  }
  std::vector<uint8_t> userblock_padded_magic = userblock_magic;
  userblock_padded_magic.push_back(0);

  struct CorruptHdf5Case {
    std::string_view filename;
    std::vector<uint8_t> bytes;
    int expected_rec_rc;
    coda_format expected_rec_format;
    int expected_rec_errno;
    int expected_open_errno;
    bool expect_hdf5_msg;
  };
  const CorruptHdf5Case kCorruptCases[] = {
      {.filename = "coda_hdf5_bare_magic.h5",
       .bytes = std::vector<uint8_t>(kHdf5Magic.begin(), kHdf5Magic.end()),
       .expected_rec_rc = -1,
       .expected_rec_format = coda_format_hdf5,
       .expected_rec_errno = CODA_ERROR_HDF5,
       .expected_open_errno = CODA_ERROR_HDF5,
       .expect_hdf5_msg = true},
      {.filename = "coda_hdf5_truncated_64.h5",
       .bytes = std::move(truncated_bytes),
       .expected_rec_rc = -1,
       .expected_rec_format = coda_format_hdf5,
       .expected_rec_errno = CODA_ERROR_HDF5,
       .expected_open_errno = CODA_ERROR_HDF5,
       .expect_hdf5_msg = true},
      {.filename = "coda_hdf5_userblock_magic.h5",
       .bytes = std::move(userblock_magic),
       .expected_rec_rc = 0,
       .expected_rec_format = coda_format_binary,
       .expected_rec_errno = CODA_SUCCESS,
       .expected_open_errno = CODA_ERROR_UNSUPPORTED_PRODUCT,
       .expect_hdf5_msg = false},
      {.filename = "coda_hdf5_userblock_padded_magic.h5",
       .bytes = std::move(userblock_padded_magic),
       .expected_rec_rc = -1,
       .expected_rec_format = coda_format_hdf5,
       .expected_rec_errno = CODA_ERROR_HDF5,
       .expected_open_errno = CODA_ERROR_HDF5,
       .expect_hdf5_msg = true},
  };

  for (const CorruptHdf5Case& tc : kCorruptCases) {
    SCOPED_TRACE(tc.filename);
    TempFile corrupt_file(tc.filename);
    ASSERT_TRUE(corrupt_file.WriteBytes(tc.bytes));

    coda_set_error(CODA_SUCCESS, nullptr);
    coda_format rec_fmt = coda_format_ascii;
    int rec_rc = 0;
    {
      // coda-hdf5.c:146-148 leaks H5Eget_major/H5Eget_minor strings on error.
      const absl::LeakCheckDisabler leak_check_disabler;
      rec_rc = coda_recognize_file(corrupt_file.c_str(), nullptr, &rec_fmt,
                                   nullptr, nullptr, nullptr);
    }
    EXPECT_EQ(tc.expected_rec_rc, rec_rc);
    EXPECT_EQ(tc.expected_rec_errno, coda_errno);
    if (tc.expected_rec_rc == 0) {
      EXPECT_EQ(tc.expected_rec_format, rec_fmt);
    }
    if (tc.expect_hdf5_msg) {
      EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("[HDF5]"));
    }

    coda_set_error(CODA_SUCCESS, nullptr);
    int open_rc = 0;
    {
      const absl::LeakCheckDisabler leak_check_disabler;
      open_rc = coda_open(corrupt_file.c_str(), product.OutParam());
    }
    EXPECT_EQ(-1, open_rc);
    EXPECT_EQ(tc.expected_open_errno, coda_errno);
    if (tc.expect_hdf5_msg) {
      EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("[HDF5]"));
    }
    EXPECT_EQ(nullptr, product.get());
  }
}

TEST(CodaHdf5Test, ReopenDirectlyHandlesValidCorruptAndTreeFailureFiles) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    H5Eclear1();
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Direct coda_hdf5_reopen() on a raw coda_bin_open() handle succeeds.
  {
    TempFile valid_h5("coda_hdf5_direct_reopen_ok.h5");
    ASSERT_NO_FATAL_FAILURE(CreateValidHdf5File(valid_h5.path()));
    const auto file_size =
        static_cast<int64_t>(std::filesystem::file_size(valid_h5.path()));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(valid_h5.c_str(), file_size, product.OutParam()));
    ASSERT_NE(nullptr, product.get());
    EXPECT_EQ(coda_format_binary, product.get()->format);

    coda_product* raw = product.release();
    ASSERT_EQ(0, coda_hdf5_reopen(&raw));
    product.Reset(raw);
    ASSERT_NE(nullptr, product.get());
    EXPECT_EQ(coda_format_hdf5, product.get()->format);
    EXPECT_EQ(file_size, product.get()->file_size);
    EXPECT_STREQ(valid_h5.c_str(), product.get()->filename);

    const auto* hdf5_prod =
        reinterpret_cast<const coda_hdf5_product*>(product.get());
    EXPECT_GE(hdf5_prod->file_id, 0);
    EXPECT_NE(nullptr, hdf5_prod->root_type);
    EXPECT_NE(nullptr, hdf5_prod->mem_ptr);
    EXPECT_NE(nullptr, hdf5_prod->object);
    EXPECT_EQ(CODA_SUCCESS, coda_close(product.release()));
  }

  // 2. Direct coda_hdf5_reopen() on corrupt bytes fails in H5Fopen() and
  //    closes the raw input product without leaking.
  {
    constexpr std::array<uint8_t, 8> kBadMagic = {0x89, 'H',  'D',  'F',
                                                  '\r', '\n', 0x1A, '\n'};
    TempFile bad_h5("coda_hdf5_direct_reopen_bad.h5");
    ASSERT_TRUE(bad_h5.WriteBytes(kBadMagic));

    coda_product* raw = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(bad_h5.c_str(),
                            static_cast<int64_t>(kBadMagic.size()), &raw));
    ASSERT_NE(nullptr, raw);

    coda_set_error(CODA_SUCCESS, nullptr);
    int reopen_rc = 0;
    {
      const absl::LeakCheckDisabler leak_check_disabler;
      reopen_rc = coda_hdf5_reopen(&raw);
    }
    EXPECT_EQ(-1, reopen_rc);
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("[HDF5]"));
  }

  // 3. HDF5 file with an unsupported float dataset (H5T_NATIVE_LDOUBLE) opens
  //    in H5Fopen() but fails in coda_hdf5_create_tree(), exercising
  //    coda-hdf5.c:92-96 cleanup and error return.
  {
    TempFile ldouble_h5("coda_hdf5_unsupported_ldouble.h5");
    ASSERT_NO_FATAL_FAILURE(CreateUnsupportedFloatHdf5File(ldouble_h5.path()));
    const auto file_size =
        static_cast<int64_t>(std::filesystem::file_size(ldouble_h5.path()));

    coda_product* raw = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_bin_open(ldouble_h5.c_str(), file_size, &raw));
    const int reopen_rc = coda_hdf5_reopen(&raw);
    EXPECT_EQ(-1, reopen_rc);
    if (reopen_rc == CODA_SUCCESS) {
      coda_close(raw);
    }

    ScopedProduct product;
    EXPECT_EQ(-1, coda_open(ldouble_h5.c_str(), product.OutParam()));
    EXPECT_EQ(nullptr, product.get());
  }
}

TEST(CodaHdf5Test, AddErrorMessageHandlesEmptyAndMultiFrameStacks) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup cleanup_errors = [] {
    H5Eclear1();
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Empty HDF5 error stack leaves the CODA error message unchanged.
  ASSERT_GE(H5Eclear1(), 0);
  coda_set_error(CODA_ERROR_HDF5, "prefix:");
  coda_hdf5_add_error_message();
  EXPECT_STREQ("prefix:", coda_errno_to_string(CODA_ERROR_HDF5));

  // 2. Multi-frame stack: H5E_WALK_UPWARD visits the outermost frame at
  //    n == 0 and skips deeper frames (n > 0).
  ASSERT_GE(H5Epush1(__FILE__, "outer_func", 10, H5E_FILE, H5E_CANTOPENFILE,
                     "outer hdf5 error"),
            0);
  ASSERT_GE(H5Epush1(__FILE__, "inner_func", 20, H5E_ARGS, H5E_BADTYPE,
                     "inner hdf5 error"),
            0);

  coda_set_error(CODA_ERROR_HDF5, "context: ");
  {
    const absl::LeakCheckDisabler leak_check_disabler;
    coda_hdf5_add_error_message();
  }
  const std::string msg = coda_errno_to_string(CODA_ERROR_HDF5);
  EXPECT_THAT(msg, HasSubstr("context: [HDF5] outer_func(): outer hdf5 error"));
  EXPECT_THAT(msg, HasSubstr("major=\""));
  EXPECT_THAT(msg, HasSubstr("minor=\""));
  EXPECT_THAT(msg, Not(HasSubstr("inner_func")));
}

TEST(CodaHdf5Test, CloseHandlesNullFieldsAndH5FcloseFailure) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup cleanup_errors = [] {
    H5Eclear1();
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Null pointers and negative file_id skip optional cleanup and free
  // struct.
  {
    auto* empty_prod = static_cast<coda_hdf5_product*>(
        std::calloc(1, sizeof(coda_hdf5_product)));
    ASSERT_NE(nullptr, empty_prod);
    empty_prod->format = coda_format_hdf5;
    empty_prod->file_id = -1;
    EXPECT_EQ(0, coda_hdf5_close(reinterpret_cast<coda_product*>(empty_prod)));
  }

  // 2. Non-negative invalid hid_t causes H5Fclose() < 0 -> CODA_ERROR_HDF5.
  //    coda_hdf5_close() returns -1 before freeing product_file, so free it.
  {
    auto* bad_id_prod = static_cast<coda_hdf5_product*>(
        std::calloc(1, sizeof(coda_hdf5_product)));
    ASSERT_NE(nullptr, bad_id_prod);
    bad_id_prod->format = coda_format_hdf5;
    bad_id_prod->file_id = 0;

    coda_set_error(CODA_SUCCESS, nullptr);
    int close_rc = 0;
    {
      const absl::LeakCheckDisabler leak_check_disabler;
      close_rc = coda_hdf5_close(reinterpret_cast<coda_product*>(bad_id_prod));
    }
    std::free(bad_id_prod);
    EXPECT_EQ(-1, close_rc);
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("[HDF5] H5Fclose()"));
  }
}

// TODO(schwehr): 90.8% line coverage in libcoda/coda-hdf5.c (69/76); remaining
// 7 missed lines (53, 55-56, 74, 76-78) are malloc/strdup OOM branches in
// coda_hdf5_reopen().

}  // namespace
}  // namespace stcorp_coda
