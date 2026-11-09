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
// Tests the Core (in-memory) Virtual File Driver: FAPL configuration, write
// tracking and dirty-region coalescing, file image callbacks, backing-store
// open/read/write/flush/truncate/delete/lock/unlock, comparison, handle
// retrieval, environment-variable defaults, and registration lifecycle
// (third_party/hdf5/src/H5FDcore.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDcore.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG; define NDEBUG around the internal prelude
// and package headers so internal struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5FDCORE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
}  // extern "C"
#ifdef H5FDCORE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDCORE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Each;
using ::testing::ElementsAreArray;

constexpr size_t kMegabyte = 1024ULL * 1024ULL;
constexpr size_t kDefaultWriteTrackingPageSize = 524288ULL;
constexpr size_t kDefaultIncrement = 8192ULL;

struct H5FDFileCloser {
  void operator()(H5FD_t* file) const {
    if (file != nullptr) {
      H5FDclose(file);
    }
  }
};
using ScopedH5FDFile = std::unique_ptr<H5FD_t, H5FDFileCloser>;

class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    const char* prev = std::getenv(name_);
    if (prev != nullptr) {
      had_prev_ = true;
      prev_value_ = prev;
    }
  }

  ~ScopedEnvVar() {
    if (had_prev_) {
      setenv(name_, prev_value_.c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  const char* name_;
  bool had_prev_ = false;
  std::string prev_value_;
};

// Reads raw bytes from an OS file path into a vector.
std::vector<uint8_t> ReadRawFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    return {};
  }
  const std::streamsize size = in.tellg();
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> buf(static_cast<size_t>(size));
  in.seekg(0, std::ios::beg);
  in.read(reinterpret_cast<char*>(buf.data()), size);
  return buf;
}

// Serializes a small valid HDF5 file image into memory using H5Fget_file_image.
std::vector<uint8_t> MakeSampleHdf5Image(int32_t value = 42) {
  ScopedFile file = CreateInMemoryFile("sample_image.h5");
  if (!file.valid()) {
    return {};
  }
  ScopedSpace space(H5Screate(H5S_SCALAR));
  if (!space.valid()) {
    return {};
  }
  ScopedDataset dset(H5Dcreate2(file.get(), "val", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  if (!dset.valid()) {
    return {};
  }
  if (H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
               &value) < 0) {
    return {};
  }
  dset.reset();
  space.reset();
  if (H5Fflush(file.get(), H5F_SCOPE_GLOBAL) < 0) {
    return {};
  }
  const ssize_t size = H5Fget_file_image(file.get(), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(size));
  if (H5Fget_file_image(file.get(), image.data(), image.size()) != size) {
    return {};
  }
  return image;
}

struct ImageCallbackState {
  int malloc_plist_set = 0;
  int malloc_file_open = 0;
  int memcpy_plist_set = 0;
  int memcpy_file_open = 0;
  int realloc_resize = 0;
  int free_plist_close = 0;
  int free_file_close = 0;
  int udata_copies = 0;
  int udata_frees = 0;
  bool fail_malloc_on_open = false;
  bool fail_memcpy_on_open = false;
  bool fail_realloc_on_resize = false;
};

struct UdataWrapper {
  ImageCallbackState* state;
};

void* TestImageMalloc(size_t size, H5FD_file_image_op_t op, void* udata) {
  auto* wrapper = static_cast<UdataWrapper*>(udata);
  if (wrapper && wrapper->state) {
    if (op == H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET) {
      ++wrapper->state->malloc_plist_set;
    } else if (op == H5FD_FILE_IMAGE_OP_FILE_OPEN) {
      ++wrapper->state->malloc_file_open;
      if (wrapper->state->fail_malloc_on_open) {
        return nullptr;
      }
    }
  }
  return std::malloc(size);
}

void* TestImageMemcpy(void* dest, const void* src, size_t size,
                      H5FD_file_image_op_t op, void* udata) {
  auto* wrapper = static_cast<UdataWrapper*>(udata);
  if (wrapper && wrapper->state) {
    if (op == H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET) {
      ++wrapper->state->memcpy_plist_set;
    } else if (op == H5FD_FILE_IMAGE_OP_FILE_OPEN) {
      ++wrapper->state->memcpy_file_open;
      if (wrapper->state->fail_memcpy_on_open) {
        return nullptr;
      }
    }
  }
  return std::memcpy(dest, src, size);
}

void* TestImageRealloc(void* ptr, size_t size, H5FD_file_image_op_t op,
                       void* udata) {
  auto* wrapper = static_cast<UdataWrapper*>(udata);
  if (wrapper && wrapper->state) {
    if (op == H5FD_FILE_IMAGE_OP_FILE_RESIZE) {
      ++wrapper->state->realloc_resize;
      if (wrapper->state->fail_realloc_on_resize) {
        return nullptr;
      }
    }
  }
  return std::realloc(ptr, size);
}

herr_t TestImageFree(void* ptr, H5FD_file_image_op_t op, void* udata) {
  auto* wrapper = static_cast<UdataWrapper*>(udata);
  if (wrapper && wrapper->state) {
    if (op == H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE) {
      ++wrapper->state->free_plist_close;
    } else if (op == H5FD_FILE_IMAGE_OP_FILE_CLOSE) {
      ++wrapper->state->free_file_close;
    }
  }
  std::free(ptr);
  return 0;
}

void* TestUdataCopy(void* udata) {
  auto* src = static_cast<UdataWrapper*>(udata);
  if (src && src->state) {
    ++src->state->udata_copies;
  }
  auto* copy = static_cast<UdataWrapper*>(std::malloc(sizeof(UdataWrapper)));
  if (copy != nullptr) {
    copy->state = src->state;
  }
  return copy;
}

herr_t TestUdataFree(void* udata) {
  auto* wrapper = static_cast<UdataWrapper*>(udata);
  if (wrapper && wrapper->state) {
    ++wrapper->state->udata_frees;
  }
  std::free(wrapper);
  return 0;
}

H5FD_file_image_callbacks_t MakeImageCallbacks(UdataWrapper* wrapper) {
  return H5FD_file_image_callbacks_t{
      .image_malloc = TestImageMalloc,
      .image_memcpy = TestImageMemcpy,
      .image_realloc = TestImageRealloc,
      .image_free = TestImageFree,
      .udata_copy = TestUdataCopy,
      .udata_free = TestUdataFree,
      .udata = wrapper,
  };
}

TEST(H5FDcoreTest, DriverRegistrationAndLifecycle) {
  ASSERT_GE(H5open(), 0);
  EXPECT_EQ(H5FD_CORE_VALUE, H5_VFD_CORE);
  EXPECT_GT(H5FDis_driver_registered_by_name("core"), 0);
  EXPECT_GT(H5FDis_driver_registered_by_value(H5_VFD_CORE), 0);

  // Calling H5FD__core_register() while already registered takes the no-op
  // branch (H5FD_CORE_id_g already has type H5I_VFL).
  EXPECT_GE(H5FD__core_register(), 0);

  // Closing the library invokes H5FD__core_unregister(); reopening calls
  // H5FD__core_register() to obtain a fresh VFL ID.
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5FD_CORE_id_g, H5I_INVALID_HID);
  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5FD_CORE_id_g, H5I_INVALID_HID);
}

TEST(H5FDcoreTest, FaplSetAndGetCorePropertiesAndValidation) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  ASSERT_GE(H5Pset_fapl_core(fapl.get(), 16384, /*backing_store=*/true), 0);
  size_t inc = 0;
  bool bstore = false;
  ASSERT_GE(H5Pget_fapl_core(fapl.get(), &inc, &bstore), 0);
  EXPECT_EQ(inc, 16384U);
  EXPECT_TRUE(bstore);

  // Default write tracking matches header defaults.
  bool wt_enabled = true;
  size_t page_size = 0;
  ASSERT_GE(H5Pget_core_write_tracking(fapl.get(), &wt_enabled, &page_size), 0);
  EXPECT_FALSE(wt_enabled);
  EXPECT_EQ(page_size, kDefaultWriteTrackingPageSize);

  // Updating write tracking preserves increment and backing_store.
  ASSERT_GE(H5Pset_core_write_tracking(fapl.get(), /*is_enabled=*/true, 4096),
            0);
  ASSERT_GE(H5Pget_core_write_tracking(fapl.get(), &wt_enabled, &page_size), 0);
  EXPECT_TRUE(wt_enabled);
  EXPECT_EQ(page_size, 4096U);
  ASSERT_GE(H5Pget_fapl_core(fapl.get(), &inc, &bstore), 0);
  EXPECT_EQ(inc, 16384U);
  EXPECT_TRUE(bstore);

  // Passing nullptr output pointers is allowed.
  EXPECT_GE(H5Pget_fapl_core(fapl.get(), nullptr, nullptr), 0);
  EXPECT_GE(H5Pget_core_write_tracking(fapl.get(), nullptr, nullptr), 0);

  ScopedErrorSilence silence;
  // page_size == 0 is rejected.
  EXPECT_LT(H5Pset_core_write_tracking(fapl.get(), true, 0), 0);

  // Non-FAPL / invalid plist arguments fail.
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_fapl_core(dcpl.get(), 4096, false), 0);
  EXPECT_LT(H5Pget_fapl_core(dcpl.get(), &inc, &bstore), 0);
  EXPECT_LT(H5Pset_core_write_tracking(dcpl.get(), true, 4096), 0);
  EXPECT_LT(H5Pget_core_write_tracking(dcpl.get(), &wt_enabled, &page_size), 0);
  EXPECT_LT(H5Pset_fapl_core(H5I_INVALID_HID, 4096, false), 0);
  EXPECT_LT(H5Pget_fapl_core(H5I_INVALID_HID, &inc, &bstore), 0);

  // Wrong VFL driver on FAPL fails.
  ScopedPlist stdio_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(stdio_fapl.valid());
  ASSERT_GE(H5Pset_fapl_stdio(stdio_fapl.get()), 0);
  EXPECT_LT(H5Pget_fapl_core(stdio_fapl.get(), &inc, &bstore), 0);
  EXPECT_LT(H5Pset_core_write_tracking(stdio_fapl.get(), true, 4096), 0);
  EXPECT_LT(
      H5Pget_core_write_tracking(stdio_fapl.get(), &wt_enabled, &page_size), 0);

  // Setting H5FD_CORE with nullptr driver info leaves H5P_peek_driver_info
  // null; getters report bad VFL driver info.
  ScopedPlist raw_core_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(raw_core_fapl.valid());
  ASSERT_GE(H5Pset_driver(raw_core_fapl.get(), H5FD_CORE, nullptr), 0);
  EXPECT_LT(H5Pget_fapl_core(raw_core_fapl.get(), &inc, &bstore), 0);
  EXPECT_LT(
      H5Pget_core_write_tracking(raw_core_fapl.get(), &wt_enabled, &page_size),
      0);
}

TEST(H5FDcoreTest, DefaultConfigFromEnvironmentVariable) {
  ScopedEnvVar restore_driver(HDF5_DRIVER);

  // 1. Unset HDF5_DRIVER -> default config (backing_store=true,
  //    write_tracking=false, increment=1MB, page_size=512KB).
  unsetenv(HDF5_DRIVER);
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_driver(fapl.get(), H5FD_CORE, nullptr), 0);
  ASSERT_GE(H5Pset_core_write_tracking(fapl.get(), true, 8192), 0);

  size_t inc = 0;
  bool bstore = false;
  ASSERT_GE(H5Pget_fapl_core(fapl.get(), &inc, &bstore), 0);
  EXPECT_EQ(inc, kMegabyte);
  EXPECT_TRUE(bstore);

  // 2. Non-core HDF5_DRIVER -> falls back to H5FD_core_default_config_g.
  ASSERT_EQ(setenv(HDF5_DRIVER, "sec2", 1), 0);
  ASSERT_GE(H5Pset_driver(fapl.get(), H5FD_CORE, nullptr), 0);
  ASSERT_GE(H5Pset_core_write_tracking(fapl.get(), false, 16384), 0);
  ASSERT_GE(H5Pget_fapl_core(fapl.get(), &inc, &bstore), 0);
  EXPECT_EQ(inc, kMegabyte);
  EXPECT_TRUE(bstore);

  // 3. "core" and "core_paged" select their respective default configs; also
  //    exercise H5FDopen and H5FDdelete when driver info is null.
  ASSERT_EQ(setenv(HDF5_DRIVER, "core", 1), 0);
  ASSERT_GE(H5Pset_driver(fapl.get(), H5FD_CORE, nullptr), 0);
  TempFile core_file("default_core.h5");
  ScopedH5FDFile f_core(H5FDopen(core_file.c_str(),
                                 H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                                 fapl.get(), HADDR_UNDEF));
  ASSERT_NE(f_core, nullptr);
  f_core.reset();
  EXPECT_GE(H5FDdelete(core_file.c_str(), fapl.get()), 0);

  ASSERT_EQ(setenv(HDF5_DRIVER, "core_paged", 1), 0);
  ASSERT_GE(H5Pset_driver(fapl.get(), H5FD_CORE, nullptr), 0);
  TempFile paged_file("default_core_paged.h5");
  ScopedH5FDFile f_paged(H5FDopen(paged_file.c_str(),
                                  H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                                  fapl.get(), HADDR_UNDEF));
  ASSERT_NE(f_paged, nullptr);
  f_paged.reset();
  EXPECT_GE(H5FDdelete(paged_file.c_str(), fapl.get()), 0);
}

TEST(H5FDcoreTest, FaplGetPreservesRuntimeConfigViaH5FgetAccessPlist) {
  TempFile tmp("fapl_get_bstore.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(fapl.get(), 12288, /*backing_store=*/true), 0);
  ASSERT_GE(H5Pset_core_write_tracking(fapl.get(), /*is_enabled=*/true, 2048),
            0);

  ScopedFile file(
      H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist retrieved(H5Fget_access_plist(file.get()));
  ASSERT_TRUE(retrieved.valid());

  size_t inc = 0;
  bool bstore = false;
  ASSERT_GE(H5Pget_fapl_core(retrieved.get(), &inc, &bstore), 0);
  EXPECT_EQ(inc, 12288U);
  EXPECT_TRUE(bstore);

  bool wt = false;
  size_t page_sz = 0;
  ASSERT_GE(H5Pget_core_write_tracking(retrieved.get(), &wt, &page_sz), 0);
  EXPECT_TRUE(wt);
  EXPECT_EQ(page_sz, 2048U);

  file.reset();

  // Zero increment in H5Pset_fapl_core defaults to H5FD_CORE_INCREMENT (8192).
  ScopedPlist zero_inc_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(zero_inc_fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(zero_inc_fapl.get(), 0, /*backing_store=*/false),
            0);
  ScopedFile mem_file(H5Fcreate("zero_inc.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                zero_inc_fapl.get()));
  ASSERT_TRUE(mem_file.valid());
  ScopedPlist retrieved_mem(H5Fget_access_plist(mem_file.get()));
  ASSERT_TRUE(retrieved_mem.valid());
  ASSERT_GE(H5Pget_fapl_core(retrieved_mem.get(), &inc, &bstore), 0);
  EXPECT_EQ(inc, kDefaultIncrement);
  EXPECT_FALSE(bstore);
}

TEST(H5FDcoreTest, OpenValidationAndErrorPaths) {
  ScopedPlist fapl = MakeCoreFapl(4096);
  ASSERT_TRUE(fapl.valid());

  const H5FD_class_t* cls = H5FD_get_class(H5FD_CORE);
  ASSERT_NE(cls, nullptr);
  ASSERT_NE(cls->open, nullptr);

  ScopedErrorSilence silence;

  // Direct class open argument checks.
  EXPECT_EQ(cls->open(nullptr, H5F_ACC_RDWR | H5F_ACC_CREAT, fapl.get(), 1024),
            nullptr);
  EXPECT_EQ(cls->open("", H5F_ACC_RDWR | H5F_ACC_CREAT, fapl.get(), 1024),
            nullptr);
  EXPECT_EQ(
      cls->open("zero_max.h5", H5F_ACC_RDWR | H5F_ACC_CREAT, fapl.get(), 0),
      nullptr);
  EXPECT_EQ(cls->open("undef_max.h5", H5F_ACC_RDWR | H5F_ACC_CREAT, fapl.get(),
                      HADDR_UNDEF),
            nullptr);
  EXPECT_EQ(cls->open("bad_fapl.h5", H5F_ACC_RDWR | H5F_ACC_CREAT,
                      H5I_INVALID_HID, 1024),
            nullptr);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  EXPECT_EQ(cls->open("wrong_plist.h5", H5F_ACC_RDWR | H5F_ACC_CREAT,
                      dcpl.get(), 1024),
            nullptr);

  // Opening a non-existent file RDONLY without H5F_ACC_CREAT fails.
  TempFile missing("does_not_exist.h5");
  EXPECT_EQ(H5FDopen(missing.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
            nullptr);

  // Opening an existing file with H5F_ACC_EXCL | H5F_ACC_CREAT fails.
  TempFile existing("already_exists.h5");
  {
    std::ofstream out(existing.path(), std::ios::binary);
    out << "existing";
  }
  ScopedPlist bstore_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(bstore_fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(bstore_fapl.get(), 4096, /*backing_store=*/true),
            0);
  EXPECT_EQ(
      H5FDopen(existing.c_str(), H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_EXCL,
               bstore_fapl.get(), HADDR_UNDEF),
      nullptr);

  // Opening a directory RDONLY triggers HDpread error if st_size > 0.
  TempDir dir("open_dir_as_file");
  ScopedH5FDFile dir_fd(
      H5FDopen(dir.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF));
  if (dir_fd != nullptr) {
    // If filesystem reports st_size == 0 for directories, close cleanly.
    dir_fd.reset();
  }
}

TEST(H5FDcoreTest, OpenEmptyFileAndReopenBackingStoreFile) {
  TempFile empty_file("empty_on_disk.h5");
  {
    std::ofstream out(empty_file.path(), std::ios::binary);
  }
  ASSERT_TRUE(std::filesystem::exists(empty_file.path()));

  ScopedPlist bstore_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(bstore_fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(bstore_fapl.get(), 512, /*backing_store=*/true),
            0);
  ASSERT_GE(H5Pset_core_write_tracking(bstore_fapl.get(), true, 256), 0);

  // 1. Re-opening a 0-byte file skips buffer allocation (size == 0).
  ScopedH5FDFile f_empty(H5FDopen(empty_file.c_str(), H5F_ACC_RDWR,
                                  bstore_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(f_empty, nullptr);
  EXPECT_EQ(H5FDget_eof(f_empty.get(), H5FD_MEM_DEFAULT), 0U);
  f_empty.reset();

  // 2. Write data to a backing store file, close, and reopen RDONLY + RDWR.
  TempFile persisted("persisted_raw.bin");
  const auto payload = std::to_array<uint8_t>({10, 20, 30, 40, 50, 60, 70, 80});
  {
    ScopedH5FDFile f_create(H5FDopen(
        persisted.c_str(), H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
        bstore_fapl.get(), HADDR_UNDEF));
    ASSERT_NE(f_create, nullptr);
    ASSERT_GE(H5FDset_eoa(f_create.get(), H5FD_MEM_DEFAULT, 64), 0);
    ASSERT_GE(H5FDwrite(f_create.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 0,
                        payload.size(), payload.data()),
              0);
  }

  // Opening RDONLY disables dirty_list even when write_tracking is true.
  ScopedH5FDFile f_ro(H5FDopen(persisted.c_str(), H5F_ACC_RDONLY,
                               bstore_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(f_ro, nullptr);
  EXPECT_GE(H5FDget_eof(f_ro.get(), H5FD_MEM_DEFAULT), payload.size());
  ASSERT_GE(H5FDset_eoa(f_ro.get(), H5FD_MEM_DEFAULT, payload.size()), 0);
  std::vector<uint8_t> read_back(payload.size(), 0xff);
  ASSERT_GE(H5FDread(f_ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 0,
                     read_back.size(), read_back.data()),
            0);
  EXPECT_THAT(read_back, ElementsAreArray(payload));
}

TEST(H5FDcoreTest, FileImageLoadCallbacksAndFailures) {
  std::vector<uint8_t> image = MakeSampleHdf5Image(12345);
  ASSERT_FALSE(image.empty());

  // 1. Opening a file image without callbacks (in-memory only).
  {
    ScopedPlist fapl = MakeCoreFapl(4096);
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);

    TempFile non_existent("image_in_mem_only.h5");
    ScopedFile file(H5Fopen(non_existent.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedDataset dset(H5Dopen2(file.get(), "val", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    int32_t read_val = 0;
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, &read_val),
              0);
    EXPECT_EQ(read_val, 12345);
    EXPECT_FALSE(std::filesystem::exists(non_existent.path()));
  }

  // 2. Full callback lifecycle with backing_store=true on a new TempFile.
  {
    ImageCallbackState state;
    UdataWrapper wrapper{&state};
    H5FD_file_image_callbacks_t cbs = MakeImageCallbacks(&wrapper);

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_core(fapl.get(), 1024, /*backing_store=*/true), 0);
    ASSERT_GE(H5Pset_file_image_callbacks(fapl.get(), &cbs), 0);
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);

    TempFile bstore_img("image_with_bstore.h5");
    {
      ScopedH5FDFile f(
          H5FDopen(bstore_img.c_str(), H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF));
      ASSERT_NE(f, nullptr);

      // Grow past image.size() to exercise image_realloc during write &
      // truncate.
      const haddr_t new_eoa = image.size() + 2048;
      ASSERT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, new_eoa), 0);
      const std::vector<uint8_t> tail(64, 0xAB);
      ASSERT_GE(H5FDwrite(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT,
                          new_eoa - tail.size(), tail.size(), tail.data()),
                0);
      ASSERT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, image.size()), 0);
      ASSERT_GE(H5FDtruncate(f.get(), H5P_DEFAULT, /*closing=*/false), 0);
    }
    fapl.reset();

    EXPECT_TRUE(std::filesystem::exists(bstore_img.path()));
    EXPECT_GE(state.udata_copies, 1);
    EXPECT_EQ(state.udata_frees, state.udata_copies);
    EXPECT_EQ(state.malloc_plist_set, 1);
    EXPECT_EQ(state.malloc_file_open, 1);
    EXPECT_EQ(state.memcpy_plist_set, 1);
    EXPECT_EQ(state.memcpy_file_open, 1);
    EXPECT_GE(state.realloc_resize, 1);
    EXPECT_EQ(state.free_file_close, 1);
    EXPECT_EQ(state.free_plist_close, 1);
  }

  // 3. Error: opening file image without H5F_ACC_CREAT when file exists on
  // disk.
  {
    TempFile existing("image_collision.h5");
    {
      std::ofstream out(existing.path(), std::ios::binary);
      out << "occupied";
    }
    ScopedPlist fapl = MakeCoreFapl(4096);
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5FDopen(existing.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
        nullptr);
  }

  // 4. Error: file image + backing_store=true inside missing parent dir.
  {
    TempDir dir("img_missing_parent");
    const std::filesystem::path bad_path =
        dir.child("nonexistent_subdir/file.h5");
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_core(fapl.get(), 4096, /*backing_store=*/true), 0);
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FDopen(bad_path.c_str(), H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF),
              nullptr);
  }

  // 5. Error: image_malloc failing on FILE_OPEN.
  {
    ImageCallbackState state;
    state.fail_malloc_on_open = true;
    UdataWrapper wrapper{&state};
    H5FD_file_image_callbacks_t cbs = MakeImageCallbacks(&wrapper);

    ScopedPlist fapl = MakeCoreFapl(4096);
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image_callbacks(fapl.get(), &cbs), 0);
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5FDopen("malloc_fail.h5", H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
        nullptr);
  }

  // 6. Error: image_memcpy failing on FILE_OPEN.
  {
    ImageCallbackState state;
    state.fail_memcpy_on_open = true;
    UdataWrapper wrapper{&state};
    H5FD_file_image_callbacks_t cbs = MakeImageCallbacks(&wrapper);

    ScopedPlist fapl = MakeCoreFapl(4096);
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image_callbacks(fapl.get(), &cbs), 0);
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5FDopen("memcpy_fail.h5", H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
        nullptr);
  }

  // 7. Error: image_realloc failing during H5FDwrite and H5FDtruncate.
  {
    ImageCallbackState state;
    UdataWrapper wrapper{&state};
    H5FD_file_image_callbacks_t cbs = MakeImageCallbacks(&wrapper);

    ScopedPlist fapl = MakeCoreFapl(1024);
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image_callbacks(fapl.get(), &cbs), 0);
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedH5FDFile f(
        H5FDopen("realloc_fail.h5", H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF));
    ASSERT_NE(f, nullptr);

    state.fail_realloc_on_resize = true;
    ScopedErrorSilence silence;
    const haddr_t big_eoa = image.size() + 4096;
    ASSERT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, big_eoa), 0);
    const std::vector<uint8_t> data(32, 0x7f);
    EXPECT_LT(H5FDwrite(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, image.size(),
                        data.size(), data.data()),
              0);
    EXPECT_LT(H5FDtruncate(f.get(), H5P_DEFAULT, /*closing=*/false), 0);
    // Re-allow realloc and leave clean state before H5FDclose frees file->mem.
    state.fail_realloc_on_resize = false;
  }
}

TEST(H5FDcoreTest, ReadWriteEoaEofAndOverflowChecks) {
  ScopedPlist fapl = MakeCoreFapl(256);
  ASSERT_TRUE(fapl.valid());
  ScopedH5FDFile f(H5FDopen("rw_ops.h5", H5F_ACC_RDWR | H5F_ACC_CREAT,
                            fapl.get(), HADDR_UNDEF));
  ASSERT_NE(f, nullptr);

  // Set EOA and verify initial EOF is 0 before writes.
  EXPECT_EQ(H5FDget_eoa(f.get(), H5FD_MEM_DEFAULT), 0U);
  EXPECT_EQ(H5FDget_eof(f.get(), H5FD_MEM_DEFAULT), 0U);
  ASSERT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, 1024), 0);
  EXPECT_EQ(H5FDget_eoa(f.get(), H5FD_MEM_DEFAULT), 1024U);

  // Write exact multiple of increment (256): new_eof == 256, no extra bump.
  std::vector<uint8_t> block1(128, 0x3C);
  ASSERT_GE(H5FDwrite(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 128,
                      block1.size(), block1.data()),
            0);
  EXPECT_EQ(H5FDget_eof(f.get(), H5FD_MEM_DEFAULT), 256U);

  // Overwrite within existing eof (no reallocation).
  const uint8_t patch[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  ASSERT_GE(H5FDwrite(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 0, sizeof(patch),
                      patch),
            0);
  EXPECT_EQ(H5FDget_eof(f.get(), H5FD_MEM_DEFAULT), 256U);

  // Write crossing into non-multiple (256..266): rounds up to 512.
  const std::vector<uint8_t> block2(10, 0x5E);
  ASSERT_GE(H5FDwrite(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 256,
                      block2.size(), block2.data()),
            0);
  EXPECT_EQ(H5FDget_eof(f.get(), H5FD_MEM_DEFAULT), 512U);

  // Read partially across EOF: bytes before eof come from memory; after EOF are
  // zero-filled (here read from 500..600 with eoa=1024, eof=512).
  std::vector<uint8_t> partial(100, 0xFF);
  ASSERT_GE(H5FDread(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 500,
                     partial.size(), partial.data()),
            0);
  EXPECT_THAT(partial, Each(0));

  // Write near end of eof then read straddling eof.
  const std::array<uint8_t, 8> tail_marker = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5FDwrite(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 504,
                      tail_marker.size(), tail_marker.data()),
            0);
  std::vector<uint8_t> straddle(16, 0xEE);
  ASSERT_GE(H5FDread(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 504,
                     straddle.size(), straddle.data()),
            0);
  for (size_t i = 0; i < tail_marker.size(); ++i) {
    EXPECT_EQ(straddle[i], tail_marker[i]);
  }
  for (size_t i = tail_marker.size(); i < straddle.size(); ++i) {
    EXPECT_EQ(straddle[i], 0);
  }

  // Overflow checks directly via H5FD_class_t callbacks.
  const H5FD_class_t* cls = f->cls;
  ASSERT_NE(cls, nullptr);
  uint8_t dummy[16] = {};
  ScopedErrorSilence silence;
  EXPECT_LT(cls->set_eoa(f.get(), H5FD_MEM_DEFAULT, HADDR_UNDEF), 0);
  EXPECT_LT(
      cls->read(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, HADDR_UNDEF, 8, dummy),
      0);
  const haddr_t wrap_addr = static_cast<haddr_t>(~size_t{0}) - 4;
  EXPECT_LT(
      cls->read(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, wrap_addr, 16, dummy),
      0);
  EXPECT_LT(
      cls->write(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, wrap_addr, 16, dummy),
      0);
}

TEST(H5FDcoreTest, WriteTrackingDirtyRegionsCoalesceFlushAndTruncate) {
  TempFile tmp("write_tracking_coalesce.bin");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(fapl.get(), 512, /*backing_store=*/true), 0);
  ASSERT_GE(H5Pset_core_write_tracking(fapl.get(), true, 512), 0);

  {
    ScopedH5FDFile f(H5FDopen(tmp.c_str(),
                              H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                              fapl.get(), HADDR_UNDEF));
    ASSERT_NE(f, nullptr);
    ASSERT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, 8192), 0);

    // Flush when clean is a safe no-op.
    ASSERT_GE(H5FDflush(f.get(), H5P_DEFAULT, /*closing=*/false), 0);

    // Truncate with closing=false before any write (eoa=8192, eof=0 -> 8192).
    ASSERT_GE(H5FDtruncate(f.get(), H5P_DEFAULT, /*closing=*/false), 0);
    EXPECT_EQ(H5FDget_eof(f.get(), H5FD_MEM_DEFAULT), 8192U);

    const auto write_byte_fill = [&](haddr_t addr, size_t len, uint8_t val) {
      std::vector<uint8_t> buf(len, val);
      ASSERT_GE(H5FDwrite(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, addr,
                          buf.size(), buf.data()),
                0);
    };

    // Build coalescing skip-list scenarios (page_size = 512):
    // Step 1: Unaligned start & end -> [0, 511].
    write_byte_fill(10, 100, 0x11);
    // Step 2: Rewrite strictly inside existing b_item -> [0, 511] unchanged.
    write_byte_fill(20, 10, 0x12);
    // Step 3: Disjoint aligned region [2048, 2559] (exact boundary start/end).
    write_byte_fill(2048, 512, 0x22);
    // Step 4: Two-page region [4096, 5119] (spans >1 page for Step 7 test).
    write_byte_fill(4096, 1024, 0x33);
    // Step 5: Adjacent to [0, 511] -> extends b_item to [0, 1023].
    write_byte_fill(512, 200, 0x13);
    // Step 6: Coalesces [2048, 2559] while preceded by [0, 1023] -> tests
    //         `if (less) a_item = less;` inside while loop.
    write_byte_fill(1536, 1200, 0x23);
    // Step 7: Precedes [4096, 5119] and ends inside its first page
    //         [3584, 4200) -> extends `end` from a_item->end (5119) and deletes
    //         a_item.
    write_byte_fill(3584, 700, 0x34);

    // Truncate back to eoa=6000 (hits aligned shrink branch, closing=false).
    ASSERT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, 6000), 0);
    ASSERT_GE(H5FDtruncate(f.get(), H5P_DEFAULT, /*closing=*/false), 0);
    EXPECT_EQ(H5FDget_eof(f.get(), H5FD_MEM_DEFAULT), 6144U);

    // Flush dirty list to backing store.
    ASSERT_GE(H5FDflush(f.get(), H5P_DEFAULT, /*closing=*/false), 0);

    // Now test dirty_list interaction with truncate(closing=true) before flush:
    // add dirty regions at [0, 511] and [2048, 2559], then shrink eof to 300
    // so [0, 511] gets clipped to [0, 299] and [2048, 2559] is skipped.
    write_byte_fill(0, 10, 0x77);
    write_byte_fill(2048, 100, 0x88);
    ASSERT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, 300), 0);
    ASSERT_GE(H5FDtruncate(f.get(), H5P_DEFAULT, /*closing=*/true), 0);
    EXPECT_EQ(H5FDget_eof(f.get(), H5FD_MEM_DEFAULT), 300U);
    ASSERT_GE(H5FDflush(f.get(), H5P_DEFAULT, /*closing=*/false), 0);
  }

  const std::vector<uint8_t> disk_bytes = ReadRawFile(tmp.path());
  ASSERT_EQ(disk_bytes.size(), 300U);
  EXPECT_EQ(disk_bytes[0], 0x77);
  EXPECT_EQ(disk_bytes[9], 0x77);
  EXPECT_EQ(disk_bytes[10], 0x11);
  EXPECT_EQ(disk_bytes[20], 0x12);
}

TEST(H5FDcoreTest, UnalignedPageEndClampedToEofAndNoWriteTrackingFlush) {
  // 1. increment (300) not a multiple of bstore_page_size (512): triggers
  //    `if (end > file->eof) end = file->eof - 1;` in add_dirty_region.
  TempFile odd_inc("odd_increment.bin");
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_core(fapl.get(), 300, /*backing_store=*/true), 0);
    ASSERT_GE(H5Pset_core_write_tracking(fapl.get(), true, 512), 0);

    ScopedH5FDFile f(H5FDopen(odd_inc.c_str(),
                              H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                              fapl.get(), HADDR_UNDEF));
    ASSERT_NE(f, nullptr);
    ASSERT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, 300), 0);
    const std::vector<uint8_t> data(50, 0x42);
    ASSERT_GE(H5FDwrite(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 0, data.size(),
                        data.data()),
              0);
    EXPECT_EQ(H5FDget_eof(f.get(), H5FD_MEM_DEFAULT), 300U);
  }
  EXPECT_EQ(std::filesystem::file_size(odd_inc.path()), 300U);

  // 2. backing_store=true without write tracking flushes entire file at once.
  TempFile untracked("untracked_bstore.bin");
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_core(fapl.get(), 256, /*backing_store=*/true), 0);

    ScopedH5FDFile f(H5FDopen(untracked.c_str(),
                              H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                              fapl.get(), HADDR_UNDEF));
    ASSERT_NE(f, nullptr);
    ASSERT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, 128), 0);
    const std::array<uint8_t, 8> vals = {9, 8, 7, 6, 5, 4, 3, 2};
    ASSERT_GE(H5FDwrite(f.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 16, vals.size(),
                        vals.data()),
              0);
  }
  const std::vector<uint8_t> disk_bytes = ReadRawFile(untracked.path());
  // Without H5FDtruncate(..., closing=true), closing flushes the full
  // increment-allocated EOF (256 bytes).
  ASSERT_EQ(disk_bytes.size(), 256U);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(disk_bytes[16 + i], 9 - i);
  }
}

TEST(H5FDcoreTest, TruncateClosingAndReadOnlyFdError) {
  // 1. closing=true with backing_store=false is a no-op.
  ScopedPlist mem_fapl = MakeCoreFapl(512);
  ASSERT_TRUE(mem_fapl.valid());
  ScopedH5FDFile mem_f(H5FDopen("trunc_mem.h5", H5F_ACC_RDWR | H5F_ACC_CREAT,
                                mem_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(mem_f, nullptr);
  ASSERT_GE(H5FDset_eoa(mem_f.get(), H5FD_MEM_DEFAULT, 64), 0);
  ASSERT_GE(H5FDtruncate(mem_f.get(), H5P_DEFAULT, /*closing=*/true), 0);
  EXPECT_EQ(H5FDget_eof(mem_f.get(), H5FD_MEM_DEFAULT), 0U);

  // 2. backing_store=true opened RDONLY cleanly triggers HDftruncate failure
  //    without dirtying memory, so closing releases all buffers leak-free.
  TempFile ro_file("truncate_ro_error.bin");
  {
    std::ofstream out(ro_file.path(), std::ios::binary);
    const std::string data(64, 'A');
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
  ScopedPlist bstore_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(bstore_fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(bstore_fapl.get(), 64, /*backing_store=*/true), 0);
  ScopedH5FDFile ro_fd(H5FDopen(ro_file.c_str(), H5F_ACC_RDONLY,
                                bstore_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(ro_fd, nullptr);
  ASSERT_GE(H5FDset_eoa(ro_fd.get(), H5FD_MEM_DEFAULT, 128), 0);
  ScopedErrorSilence silence;
  EXPECT_LT(H5FDtruncate(ro_fd.get(), H5P_DEFAULT, /*closing=*/true), 0);
}

TEST(H5FDcoreTest, ComparisonQueryAndVfdHandleRetrieval) {
  ScopedPlist mem_fapl = MakeCoreFapl(4096);
  ASSERT_TRUE(mem_fapl.valid());
  ScopedPlist bstore_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(bstore_fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(bstore_fapl.get(), 4096, /*backing_store=*/true),
            0);

  // 1. Driver-level feature query before opening a file.
  unsigned long driver_flags = 0;
  ASSERT_GE(H5FDdriver_query(H5FD_CORE, &driver_flags), 0);
  EXPECT_NE(driver_flags & H5FD_FEAT_AGGREGATE_METADATA, 0UL);
  EXPECT_NE(driver_flags & H5FD_FEAT_ALLOW_FILE_IMAGE, 0UL);
  EXPECT_NE(driver_flags & H5FD_FEAT_CAN_USE_FILE_IMAGE_CALLBACKS, 0UL);
  EXPECT_EQ(driver_flags & H5FD_FEAT_POSIX_COMPAT_HANDLE, 0UL);
  EXPECT_EQ(driver_flags & H5FD_FEAT_DEFAULT_VFD_COMPATIBLE, 0UL);

  // 2. File-level feature query: in-memory vs backing-store.
  ScopedH5FDFile mem_a(H5FDopen("alpha.h5", H5F_ACC_RDWR | H5F_ACC_CREAT,
                                mem_fapl.get(), HADDR_UNDEF));
  ScopedH5FDFile mem_a2(H5FDopen("alpha.h5", H5F_ACC_RDWR | H5F_ACC_CREAT,
                                 mem_fapl.get(), HADDR_UNDEF));
  ScopedH5FDFile mem_b(H5FDopen("beta.h5", H5F_ACC_RDWR | H5F_ACC_CREAT,
                                mem_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(mem_a, nullptr);
  ASSERT_NE(mem_a2, nullptr);
  ASSERT_NE(mem_b, nullptr);

  unsigned long mem_flags = 0;
  ASSERT_GE(H5FDquery(mem_a.get(), &mem_flags), 0);
  EXPECT_EQ(mem_flags & H5FD_FEAT_POSIX_COMPAT_HANDLE, 0UL);

  // In-memory comparisons use file names.
  EXPECT_EQ(H5FDcmp(mem_a.get(), mem_a2.get()), 0);
  EXPECT_LT(H5FDcmp(mem_a.get(), mem_b.get()), 0);
  EXPECT_GT(H5FDcmp(mem_b.get(), mem_a.get()), 0);

  // Backing-store comparisons use (device, inode) and include POSIX flags.
  TempFile tmp1("cmp_one.bin");
  TempFile tmp2("cmp_two.bin");
  ScopedH5FDFile bs1(H5FDopen(tmp1.c_str(),
                              H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                              bstore_fapl.get(), HADDR_UNDEF));
  ScopedH5FDFile bs1_again(
      H5FDopen(tmp1.c_str(), H5F_ACC_RDWR, bstore_fapl.get(), HADDR_UNDEF));
  ScopedH5FDFile bs2(H5FDopen(tmp2.c_str(),
                              H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                              bstore_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(bs1, nullptr);
  ASSERT_NE(bs1_again, nullptr);
  ASSERT_NE(bs2, nullptr);

  unsigned long bs_flags = 0;
  ASSERT_GE(H5FDquery(bs1.get(), &bs_flags), 0);
  EXPECT_NE(bs_flags & H5FD_FEAT_POSIX_COMPAT_HANDLE, 0UL);
  EXPECT_NE(bs_flags & H5FD_FEAT_DEFAULT_VFD_COMPATIBLE, 0UL);

  EXPECT_EQ(H5FDcmp(bs1.get(), bs1_again.get()), 0);
  const int cmp_1_2 = H5FDcmp(bs1.get(), bs2.get());
  EXPECT_NE(cmp_1_2, 0);
  EXPECT_EQ(H5FDcmp(bs2.get(), bs1.get()), -cmp_1_2);

  // Compare against /dev/null if on a different filesystem st_dev.
  ScopedH5FDFile devnull(
      H5FDopen("/dev/null", H5F_ACC_RDONLY, bstore_fapl.get(), HADDR_UNDEF));
  if (devnull != nullptr) {
    const int cmp_dev = H5FDcmp(bs1.get(), devnull.get());
    EXPECT_NE(cmp_dev, 0);
    EXPECT_EQ(H5FDcmp(devnull.get(), bs1.get()), -cmp_dev);
  }

  // 3. Handle retrieval: H5P_DEFAULT, non-default FAPL without want_posix_fd,
  //    non-default FAPL with want_posix_fd=true, non-FAPL plist, and errors.
  TempFile h5_bstore_path("vfd_handle.h5");
  ScopedFile h5_file(H5Fcreate(h5_bstore_path.c_str(), H5F_ACC_TRUNC,
                               H5P_DEFAULT, bstore_fapl.get()));
  ASSERT_TRUE(h5_file.valid());
  ASSERT_GE(H5Fflush(h5_file.get(), H5F_SCOPE_GLOBAL), 0);

  void* handle = nullptr;
  ASSERT_GE(H5Fget_vfd_handle(h5_file.get(), H5P_DEFAULT, &handle), 0);
  ASSERT_NE(handle, nullptr);
  const uint8_t* raw_mem = *static_cast<const uint8_t* const*>(handle);
  ASSERT_NE(raw_mem, nullptr);
  EXPECT_EQ(raw_mem[0], 0x89);
  EXPECT_EQ(raw_mem[1], 'H');
  EXPECT_EQ(raw_mem[2], 'D');
  EXPECT_EQ(raw_mem[3], 'F');

  ScopedPlist plain_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(plain_fapl.valid());
  void* mem_ptr = nullptr;
  ASSERT_GE(H5FDget_vfd_handle(bs1.get(), plain_fapl.get(), &mem_ptr), 0);
  EXPECT_NE(mem_ptr, nullptr);

  ScopedPlist posix_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(posix_fapl.valid());
  H5P_genplist_t* plist_obj =
      static_cast<H5P_genplist_t*>(H5I_object(posix_fapl.get()));
  ASSERT_NE(plist_obj, nullptr);
  bool want_fd = true;
  ASSERT_GE(H5P_set(plist_obj, H5F_ACS_WANT_POSIX_FD_NAME, &want_fd), 0);
  void* fd_ptr = nullptr;
  ASSERT_GE(H5FDget_vfd_handle(bs1.get(), posix_fapl.get(), &fd_ptr), 0);
  ASSERT_NE(fd_ptr, nullptr);
  EXPECT_GE(*static_cast<const int*>(fd_ptr), 0);

  // Direct class get_handle checks: non-FAPL plist (property missing),
  // invalid plist ID, and null output pointer.
  ASSERT_NE(bs1->cls, nullptr);
  ASSERT_NE(bs1->cls->get_handle, nullptr);
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  void* dcpl_handle = nullptr;
  EXPECT_GE(bs1->cls->get_handle(bs1.get(), dcpl.get(), &dcpl_handle), 0);
  EXPECT_EQ(dcpl_handle, mem_ptr);

  ScopedErrorSilence silence;
  void* unused_handle = nullptr;
  EXPECT_LT(bs1->cls->get_handle(bs1.get(), H5I_INVALID_HID, &unused_handle),
            0);
  EXPECT_LT(bs1->cls->get_handle(bs1.get(), H5P_DEFAULT, nullptr), 0);
}

TEST(H5FDcoreTest, LockUnlockFileLockingEnvAndDelete) {
  // 1. In-memory lock/unlock are no-ops.
  ScopedPlist mem_fapl = MakeCoreFapl(4096);
  ASSERT_TRUE(mem_fapl.valid());
  ScopedH5FDFile mem_f(H5FDopen("lock_mem.h5", H5F_ACC_RDWR | H5F_ACC_CREAT,
                                mem_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(mem_f, nullptr);
  EXPECT_GE(H5FDlock(mem_f.get(), false), 0);
  EXPECT_GE(H5FDlock(mem_f.get(), true), 0);
  EXPECT_GE(H5FDunlock(mem_f.get()), 0);
  mem_f.reset();

  // 2. Backing store locking between two independent open descriptors plus
  //    H5FD_ignore_disabled_file_locks_p override during open.
  TempFile lock_tmp("core_lock_target.bin");
  ScopedPlist bstore_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(bstore_fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(bstore_fapl.get(), 4096, /*backing_store=*/true),
            0);

  ScopedH5FDFile f1(H5FDopen(lock_tmp.c_str(),
                             H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                             bstore_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(f1, nullptr);

  // Temporarily test H5FD_ignore_disabled_file_locks_p != FAIL branch in open.
  const htri_t prev_ignore = H5FD_ignore_disabled_file_locks_p;
  H5FD_ignore_disabled_file_locks_p = 1;
  ScopedH5FDFile f2 = [&] {
    absl::Cleanup restore_ignore = [&] {
      H5FD_ignore_disabled_file_locks_p = prev_ignore;
    };
    return ScopedH5FDFile(H5FDopen(lock_tmp.c_str(), H5F_ACC_RDWR,
                                   bstore_fapl.get(), HADDR_UNDEF));
  }();
  ASSERT_NE(f2, nullptr);

  // Two shared locks succeed together.
  ASSERT_GE(H5FDlock(f1.get(), /*rw=*/false), 0);
  ASSERT_GE(H5FDlock(f2.get(), /*rw=*/false), 0);
  ASSERT_GE(H5FDunlock(f1.get()), 0);
  ASSERT_GE(H5FDunlock(f2.get()), 0);

  // Exclusive lock on f1 blocks lock attempt on f2.
  ASSERT_GE(H5FDlock(f1.get(), /*rw=*/true), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDlock(f2.get(), /*rw=*/true), 0);
  }
  ASSERT_GE(H5FDunlock(f1.get()), 0);

  f2.reset();
  f1.reset();

  // 3. Delete behavior with backing_store false/true and errors.
  TempFile missing_mem("not_on_disk.h5");
  EXPECT_GE(H5FDdelete(missing_mem.c_str(), mem_fapl.get()), 0);

  ASSERT_TRUE(std::filesystem::exists(lock_tmp.path()));
  EXPECT_GE(H5FDdelete(lock_tmp.c_str(), bstore_fapl.get()), 0);
  EXPECT_FALSE(std::filesystem::exists(lock_tmp.path()));

  // Missing file with backing_store=true fails.
  ScopedErrorSilence silence;
  EXPECT_LT(H5FDdelete(lock_tmp.c_str(), bstore_fapl.get()), 0);

  // Direct class del with invalid FAPL ID fails.
  const H5FD_class_t* cls = H5FD_get_class(H5FD_CORE);
  ASSERT_NE(cls, nullptr);
  ASSERT_NE(cls->del, nullptr);
  EXPECT_LT(cls->del(lock_tmp.c_str(), H5I_INVALID_HID), 0);
}

TEST(H5FDcoreTest, BackingStoreDefaultVfdCompatAndCorruptHeaderChecks) {
  TempFile h5_tmp("core_roundtrip_compat.h5");
  const auto expected = std::to_array<int32_t>({10, -20, 30, 404, 5555});

  // 1. Create an HDF5 file via Core VFD with backing_store=true and write data.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_core(fapl.get(), 4096, /*backing_store=*/true), 0);
    ASSERT_GE(H5Pset_core_write_tracking(fapl.get(), true, 4096), 0);

    ScopedFile file(
        H5Fcreate(h5_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    const hsize_t dim = expected.size();
    ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "numbers", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, expected.data()),
              0);
  }

  // 2. Re-open flushed backing-store file with the default VFD (H5P_DEFAULT).
  {
    ScopedFile default_file(
        H5Fopen(h5_tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(default_file.valid());
    ScopedDataset dset(H5Dopen2(default_file.get(), "numbers", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::array<int32_t, expected.size()> actual{};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, actual.data()),
              0);
    EXPECT_THAT(actual, ElementsAreArray(expected));
  }

  // 3. Opening a corrupt disk header or corrupt in-memory file image fails.
  ScopedErrorSilence silence;
  TempFile corrupt_disk("corrupt_core_disk.h5");
  {
    std::ofstream out(corrupt_disk.path(), std::ios::binary);
    out << "NOT_AN_HDF5_FILE_HEADER_PADDING_0123456789";
  }
  ScopedPlist bstore_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(bstore_fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(bstore_fapl.get(), 4096, /*backing_store=*/true),
            0);
  ScopedFile bad_disk(
      H5Fopen(corrupt_disk.c_str(), H5F_ACC_RDONLY, bstore_fapl.get()));
  EXPECT_FALSE(bad_disk.valid());

  std::array<uint8_t, 64> corrupt_img{};
  corrupt_img.fill(0x5a);
  ScopedPlist mem_fapl = MakeCoreFapl(4096);
  ASSERT_TRUE(mem_fapl.valid());
  ASSERT_GE(
      H5Pset_file_image(mem_fapl.get(), corrupt_img.data(), corrupt_img.size()),
      0);
  ScopedFile bad_img(
      H5Fopen("corrupt_mem_image.h5", H5F_ACC_RDONLY, mem_fapl.get()));
  EXPECT_FALSE(bad_img.valid());
}

}  // namespace
}  // namespace hdf5
