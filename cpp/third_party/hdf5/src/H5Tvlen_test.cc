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
// Tests variable-length datatype creation, memory/disk location callbacks,
// custom memory managers, dataset/attribute I/O, recursive reclamation, and
// VOL error propagation (third_party/hdf5/src/H5Tvlen.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tvlen.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG; match that macro around internal headers
// so internal struct layouts (e.g., H5T_t, H5T_shared_t) match the library.
#ifndef NDEBUG
#define H5TVLEN_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
// H5private.h skips typedef'ing H5__Float16 in C++, yet H5Tpkg.h uses it to
// declare unused internal +/-Inf globals under H5_HAVE__FLOAT16.
typedef uint16_t H5__Float16;
#endif
extern "C" {
#define H5R_FRIEND
#define H5T_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Rpkg.h"
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5TVLEN_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TVLEN_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

H5T_t* LookupType(hid_t tid) {
  return static_cast<H5T_t*>(H5I_object_verify(tid, H5I_DATATYPE));
}

struct AllocTracker {
  int alloc_calls = 0;
  int free_calls = 0;
  bool return_null = false;
  size_t last_alloc_size = 0;
};

void* TrackedAlloc(size_t size, void* alloc_info) {
  auto* tracker = static_cast<AllocTracker*>(alloc_info);
  ++tracker->alloc_calls;
  tracker->last_alloc_size = size;
  if (tracker->return_null) {
    return nullptr;
  }
  return std::malloc(size);
}

void TrackedFree(void* mem, void* free_info) {
  auto* tracker = static_cast<AllocTracker*>(free_info);
  ++tracker->free_calls;
  std::free(mem);
}

struct MockVolState {
  int file_token = 1;
  bool fail_get_cont_info = false;
  bool fail_blob_put = false;
  bool fail_blob_get = false;
  bool fail_blob_isnull = false;
  bool fail_blob_setnull = false;
  bool fail_blob_delete = false;
  size_t blob_id_size = 16;
  bool next_isnull = false;
};

MockVolState& GetMockVolState() {
  static MockVolState state;
  return state;
}

extern const H5VL_class_t kMockVlenVolClass;

void* MockFileCreate(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                     void** /*req*/) {
  return &GetMockVolState().file_token;
}

herr_t MockFileGet(void* /*obj*/, H5VL_file_get_args_t* args, hid_t /*dxpl_id*/,
                   void** /*req*/) {
  const auto& state = GetMockVolState();
  if (args->op_type == H5VL_FILE_GET_CONT_INFO) {
    if (state.fail_get_cont_info) {
      return -1;
    }
    args->args.get_cont_info.info->version = H5VL_CONTAINER_INFO_VERSION;
    args->args.get_cont_info.info->feature_flags = 0;
    args->args.get_cont_info.info->token_size = sizeof(haddr_t);
    args->args.get_cont_info.info->blob_id_size = state.blob_id_size;
    return 0;
  }
  return -1;
}

herr_t MockFileClose(void* /*file*/, hid_t /*dxpl_id*/, void** /*req*/) {
  return 0;
}

herr_t MockGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                      const H5VL_class_t** conn_cls) {
  *conn_cls = &kMockVlenVolClass;
  return 0;
}

herr_t MockGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  *cap_flags = kMockVlenVolClass.cap_flags;
  return 0;
}

herr_t MockOptQuery(void* /*obj*/, H5VL_subclass_t /*cls*/, int /*opt_type*/,
                    uint64_t* flags) {
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

herr_t MockBlobPut(void* /*obj*/, const void* /*buf*/, size_t size,
                   void* blob_id, void* /*ctx*/) {
  const auto& state = GetMockVolState();
  if (state.fail_blob_put) {
    return -1;
  }
  std::memset(blob_id, 0x2a, state.blob_id_size);
  static_cast<uint8_t*>(blob_id)[0] = static_cast<uint8_t>(size & 0xff);
  return 0;
}

herr_t MockBlobGet(void* /*obj*/, const void* /*blob_id*/, void* buf,
                   size_t size, void* /*ctx*/) {
  if (GetMockVolState().fail_blob_get) {
    return -1;
  }
  std::memset(buf, 0x55, size);
  return 0;
}

herr_t MockBlobSpecific(void* /*obj*/, void* blob_id,
                        H5VL_blob_specific_args_t* args) {
  const auto& state = GetMockVolState();
  switch (args->op_type) {
    case H5VL_BLOB_ISNULL:
      if (state.fail_blob_isnull) {
        return -1;
      }
      *args->args.is_null.isnull = state.next_isnull;
      return 0;
    case H5VL_BLOB_SETNULL:
      if (state.fail_blob_setnull) {
        return -1;
      }
      std::memset(blob_id, 0, state.blob_id_size);
      return 0;
    case H5VL_BLOB_DELETE:
      if (state.fail_blob_delete) {
        return -1;
      }
      return 0;
    default:
      return -1;
  }
}

const H5VL_class_t kMockVlenVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(5119),
    .name = "h5tvlen_test_mock_vol",
    .conn_version = 1,
    .cap_flags = 0,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls = {},
    .attr_cls = {},
    .dataset_cls = {},
    .datatype_cls = {},
    .file_cls =
        {
            .create = MockFileCreate,
            .get = MockFileGet,
            .close = MockFileClose,
        },
    .group_cls = {},
    .link_cls = {},
    .object_cls = {},
    .introspect_cls =
        {
            .get_conn_cls = MockGetConnCls,
            .get_cap_flags = MockGetCapFlags,
            .opt_query = MockOptQuery,
        },
    .request_cls = {},
    .blob_cls =
        {
            .put = MockBlobPut,
            .get = MockBlobGet,
            .specific = MockBlobSpecific,
        },
    .token_cls = {},
    .optional = nullptr,
};

TEST(H5TvlenTest, CreateInvalidAndValidSequenceTypes) {
  ASSERT_GE(H5open(), 0);

  ScopedSpace non_type(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(non_type.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Tvlen_create(H5I_INVALID_HID), H5I_INVALID_HID);
    EXPECT_EQ(H5Tvlen_create(non_type.get()), H5I_INVALID_HID);
  }

  ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_i32.valid());
  EXPECT_EQ(H5Tget_class(vl_i32.get()), H5T_VLEN);
  EXPECT_EQ(H5Tget_size(vl_i32.get()), sizeof(hvl_t));

  ScopedType super_type(H5Tget_super(vl_i32.get()));
  ASSERT_TRUE(super_type.valid());
  EXPECT_GT(H5Tequal(super_type.get(), H5T_NATIVE_INT32), 0);

  const H5T_t* vl_dt = LookupType(vl_i32.get());
  ASSERT_NE(vl_dt, nullptr);
  EXPECT_TRUE(vl_dt->shared->force_conv);
  EXPECT_EQ(vl_dt->shared->u.vlen.type, H5T_VLEN_SEQUENCE);
  EXPECT_EQ(vl_dt->shared->u.vlen.loc, H5T_LOC_MEMORY);

  // Version inheritance from complex base datatype (encoding version 5).
  ScopedType cplx(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ASSERT_TRUE(cplx.valid());
  ScopedType vl_cplx(H5Tvlen_create(cplx.get()));
  ASSERT_TRUE(vl_cplx.valid());
  const H5T_t* cplx_dt = LookupType(cplx.get());
  const H5T_t* vl_cplx_dt = LookupType(vl_cplx.get());
  ASSERT_NE(cplx_dt, nullptr);
  ASSERT_NE(vl_cplx_dt, nullptr);
  EXPECT_EQ(vl_cplx_dt->shared->version, cplx_dt->shared->version);
}

TEST(H5TvlenTest, MemSequenceConvertDefaultAndCustomAllocators) {
  ASSERT_GE(H5open(), 0);

  ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_i32.valid());

  // 1. In-memory conversion with custom allocator, including nil encodings.
  AllocTracker tracker;
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_vlen_mem_manager(dxpl.get(), TrackedAlloc, &tracker,
                                    TrackedFree, &tracker),
            0);

  std::array<int32_t, 3> src_vals = {11, 22, 33};
  std::array<int32_t, 1> non_null_dummy = {99};
  std::array<hvl_t, 4> buf = {{
      {.len = 3, .p = src_vals.data()},
      {.len = 0, .p = nullptr},
      {.len = 2, .p = nullptr},
      {.len = 0, .p = non_null_dummy.data()},
  }};

  ASSERT_GE(H5Tconvert(vl_i32.get(), vl_i32.get(), buf.size(), buf.data(),
                       nullptr, dxpl.get()),
            0);
  EXPECT_EQ(tracker.alloc_calls, 1);
  EXPECT_EQ(tracker.last_alloc_size, 3 * sizeof(int32_t));
  EXPECT_EQ(buf[0].len, 3);
  ASSERT_NE(buf[0].p, nullptr);
  EXPECT_NE(buf[0].p, src_vals.data());
  const auto* copied = static_cast<const int32_t*>(buf[0].p);
  EXPECT_EQ(copied[0], 11);
  EXPECT_EQ(copied[1], 22);
  EXPECT_EQ(copied[2], 33);
  for (size_t i = 1; i < buf.size(); ++i) {
    EXPECT_EQ(buf[i].len, 0);
    EXPECT_EQ(buf[i].p, nullptr);
  }

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  constexpr hsize_t kCount = 4;
  ScopedSpace vec_space(H5Screate_simple(1, &kCount, nullptr));
  ASSERT_TRUE(vec_space.valid());
  ASSERT_GE(H5Treclaim(vl_i32.get(), vec_space.get(), dxpl.get(), buf.data()),
            0);
  EXPECT_EQ(tracker.free_calls, 1);

  // 2. Default malloc/free allocator path on non-empty sequence.
  std::array<int32_t, 2> def_vals = {7, 8};
  hvl_t def_vl{.len = 2, .p = def_vals.data()};
  ASSERT_GE(
      H5Tconvert(vl_i32.get(), vl_i32.get(), 1, &def_vl, nullptr, H5P_DEFAULT),
      0);
  EXPECT_EQ(def_vl.len, 2);
  ASSERT_NE(def_vl.p, nullptr);
  EXPECT_NE(def_vl.p, def_vals.data());
  EXPECT_GE(H5Treclaim(vl_i32.get(), space.get(), H5P_DEFAULT, &def_vl), 0);

  // 3. Failing custom allocator in H5T__vlen_mem_seq_write.
  tracker.return_null = true;
  hvl_t fail_vl{.len = 2, .p = def_vals.data()};
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Tconvert(vl_i32.get(), vl_i32.get(), 1, &fail_vl, nullptr, dxpl.get()),
        0);
  }
  tracker.return_null = false;

  // 4. Direct zero-length call to H5T__vlen_mem_seq_write (seq_len == 0).
  H5T_t* dt = LookupType(vl_i32.get());
  ASSERT_NE(dt, nullptr);
  hvl_t zero_out{.len = 99, .p = non_null_dummy.data()};
  H5T_vlen_alloc_info_t alloc_info{};
  EXPECT_GE(dt->shared->u.vlen.cls->write(nullptr, &alloc_info, &zero_out,
                                          non_null_dummy.data(), nullptr, 0,
                                          sizeof(int32_t)),
            0);
  EXPECT_EQ(zero_out.len, 0);
  EXPECT_EQ(zero_out.p, nullptr);
}

TEST(H5TvlenTest, MemStringConvertDefaultAndCustomAllocators) {
  ASSERT_GE(H5open(), 0);

  ScopedType vstr(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vstr.valid());
  ASSERT_GE(H5Tset_size(vstr.get(), H5T_VARIABLE), 0);

  constexpr hsize_t kCount = 3;
  ScopedSpace vec_space(H5Screate_simple(1, &kCount, nullptr));
  ASSERT_TRUE(vec_space.valid());

  // 1. Default malloc/free covering nullptr, empty string "", and "hello".
  {
    const char* s0 = nullptr;
    const char* s1 = "";
    const char* s2 = "hello";
    std::array<char*, 3> bufs = {const_cast<char*>(s0), const_cast<char*>(s1),
                                 const_cast<char*>(s2)};
    ASSERT_GE(H5Tconvert(vstr.get(), vstr.get(), bufs.size(), bufs.data(),
                         nullptr, H5P_DEFAULT),
              0);
    EXPECT_EQ(bufs[0], nullptr);
    ASSERT_NE(bufs[1], nullptr);
    EXPECT_NE(bufs[1], s1);
    EXPECT_STREQ(bufs[1], "");
    ASSERT_NE(bufs[2], nullptr);
    EXPECT_NE(bufs[2], s2);
    EXPECT_STREQ(bufs[2], "hello");

    EXPECT_GE(
        H5Treclaim(vstr.get(), vec_space.get(), H5P_DEFAULT, bufs.data()), 0);
  }

  // 2. Custom allocator and failure path.
  AllocTracker tracker;
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_vlen_mem_manager(dxpl.get(), TrackedAlloc, &tracker,
                                    TrackedFree, &tracker),
            0);

  {
    const char* s_src = "world";
    char* ptr = const_cast<char*>(s_src);
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());

    ASSERT_GE(
        H5Tconvert(vstr.get(), vstr.get(), 1, &ptr, nullptr, dxpl.get()), 0);
    EXPECT_EQ(tracker.alloc_calls, 1);
    EXPECT_EQ(tracker.last_alloc_size, 6);
    EXPECT_STREQ(ptr, "world");
    EXPECT_GE(H5Treclaim(vstr.get(), scalar.get(), dxpl.get(), &ptr), 0);
    EXPECT_EQ(tracker.free_calls, 1);
  }

  {
    tracker.return_null = true;
    const char* s_src = "fail";
    char* ptr = const_cast<char*>(s_src);
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tconvert(vstr.get(), vstr.get(), 1, &ptr, nullptr, dxpl.get()),
              0);
  }
}

TEST(H5TvlenTest, FileDatasetAndAttributeRoundTripExercisesDiskAndGetptr) {
  ScopedFile file = CreateInMemoryFile("vlen_io.h5");
  ASSERT_TRUE(file.valid());

  // 1. vlen<int32> dataset (noop base type -> getptr + disk write/read/setnull
  //    + overwrite background deletion).
  {
    ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_i32.valid());
    constexpr hsize_t kDim = 3;
    ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/seq32", vl_i32.get(),
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::array<int32_t, 2> row0 = {10, 20};
    std::array<int32_t, 3> row2 = {30, 40, 50};
    std::array<hvl_t, 3> write1 = {{
        {.len = 2, .p = row0.data()},
        {.len = 0, .p = nullptr},
        {.len = 3, .p = row2.data()},
    }};
    ASSERT_GE(H5Dwrite(dset.get(), vl_i32.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       write1.data()),
              0);

    std::array<hvl_t, 3> read1{};
    ASSERT_GE(H5Dread(dset.get(), vl_i32.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read1.data()),
              0);
    absl::Cleanup reclaim_read1 = [&] {
      H5Treclaim(vl_i32.get(), space.get(), H5P_DEFAULT, read1.data());
    };
    EXPECT_EQ(read1[0].len, 2);
    EXPECT_EQ(static_cast<const int32_t*>(read1[0].p)[1], 20);
    EXPECT_EQ(read1[1].len, 0);
    EXPECT_EQ(read1[1].p, nullptr);
    EXPECT_EQ(read1[2].len, 3);
    EXPECT_EQ(static_cast<const int32_t*>(read1[2].p)[2], 50);

    // Overwrite: non-nil -> nil (disk_setnull with bg), nil -> non-nil
    // (disk_delete with seq_len == 0), non-nil -> non-nil (disk_delete > 0).
    std::array<int32_t, 2> new_row1 = {7, 8};
    std::array<int32_t, 1> new_row2 = {99};
    std::array<hvl_t, 3> write2 = {{
        {.len = 0, .p = nullptr},
        {.len = 2, .p = new_row1.data()},
        {.len = 1, .p = new_row2.data()},
    }};
    ASSERT_GE(H5Dwrite(dset.get(), vl_i32.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       write2.data()),
              0);

    std::array<hvl_t, 3> read2{};
    ASSERT_GE(H5Dread(dset.get(), vl_i32.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read2.data()),
              0);
    absl::Cleanup reclaim_read2 = [&] {
      H5Treclaim(vl_i32.get(), space.get(), H5P_DEFAULT, read2.data());
    };
    EXPECT_EQ(read2[0].len, 0);
    EXPECT_EQ(read2[0].p, nullptr);
    EXPECT_EQ(read2[1].len, 2);
    EXPECT_EQ(static_cast<const int32_t*>(read2[1].p)[0], 7);
    EXPECT_EQ(read2[2].len, 1);
    EXPECT_EQ(static_cast<const int32_t*>(read2[2].p)[0], 99);
  }

  // 2. Converted base type (vlen<int32> mem -> vlen<int64> file) hits read().
  {
    ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
    ScopedType vl_i64(H5Tvlen_create(H5T_NATIVE_INT64));
    ASSERT_TRUE(vl_i32.valid());
    ASSERT_TRUE(vl_i64.valid());

    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/seq64", vl_i64.get(),
                                  scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::array<int32_t, 2> vals = {-5, 42};
    hvl_t w_vl{.len = 2, .p = vals.data()};
    ASSERT_GE(
        H5Dwrite(dset.get(), vl_i32.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT, &w_vl),
        0);

    hvl_t r_vl{};
    ASSERT_GE(
        H5Dread(dset.get(), vl_i32.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT, &r_vl),
        0);
    absl::Cleanup reclaim_r = [&] {
      H5Treclaim(vl_i32.get(), scalar.get(), H5P_DEFAULT, &r_vl);
    };
    EXPECT_EQ(r_vl.len, 2);
    EXPECT_EQ(static_cast<const int32_t*>(r_vl.p)[0], -5);
    EXPECT_EQ(static_cast<const int32_t*>(r_vl.p)[1], 42);
  }

  // 3. VL string dataset (getptr, nullptr <-> disk nil, "" and non-empty).
  {
    ScopedType vstr(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(vstr.valid());
    ASSERT_GE(H5Tset_size(vstr.get(), H5T_VARIABLE), 0);

    constexpr hsize_t kDim = 3;
    ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/vstrs", vstr.get(), space.get(),
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::array<const char*, 3> w1 = {nullptr, "", "alpha"};
    ASSERT_GE(H5Dwrite(dset.get(), vstr.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       w1.data()),
              0);

    std::array<char*, 3> r1{};
    ASSERT_GE(
        H5Dread(dset.get(), vstr.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT, r1.data()),
        0);
    absl::Cleanup reclaim_r1 = [&] {
      H5Treclaim(vstr.get(), space.get(), H5P_DEFAULT, r1.data());
    };
    EXPECT_EQ(r1[0], nullptr);
    EXPECT_STREQ(r1[1], "");
    EXPECT_STREQ(r1[2], "alpha");

    std::array<const char*, 3> w2 = {"beta", nullptr, "g"};
    ASSERT_GE(H5Dwrite(dset.get(), vstr.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       w2.data()),
              0);
    std::array<char*, 3> r2{};
    ASSERT_GE(
        H5Dread(dset.get(), vstr.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT, r2.data()),
        0);
    absl::Cleanup reclaim_r2 = [&] {
      H5Treclaim(vstr.get(), space.get(), H5P_DEFAULT, r2.data());
    };
    EXPECT_STREQ(r2[0], "beta");
    EXPECT_EQ(r2[1], nullptr);
    EXPECT_STREQ(r2[2], "g");
  }

  // 4. Nested VL dataset + VL attribute.
  {
    ScopedType inner(H5Tvlen_create(H5T_NATIVE_INT32));
    ScopedType outer(H5Tvlen_create(inner.get()));
    ASSERT_TRUE(inner.valid());
    ASSERT_TRUE(outer.valid());

    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/nested", outer.get(),
                                  scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::array<int32_t, 2> a = {1, 2};
    std::array<int32_t, 1> b = {3};
    std::array<hvl_t, 2> inner_arr1 = {{
        {.len = 2, .p = a.data()},
        {.len = 1, .p = b.data()},
    }};
    hvl_t w_nested1{.len = 2, .p = inner_arr1.data()};
    ASSERT_GE(H5Dwrite(dset.get(), outer.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       &w_nested1),
              0);

    // Shorter nested overwrite frees leftover nested heap objects.
    std::array<int32_t, 1> c = {9};
    hvl_t inner_arr2{.len = 1, .p = c.data()};
    hvl_t w_nested2{.len = 1, .p = &inner_arr2};
    ASSERT_GE(H5Dwrite(dset.get(), outer.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       &w_nested2),
              0);

    hvl_t r_nested{};
    ASSERT_GE(H5Dread(dset.get(), outer.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      &r_nested),
              0);
    absl::Cleanup reclaim_nested = [&] {
      H5Treclaim(outer.get(), scalar.get(), H5P_DEFAULT, &r_nested);
    };
    EXPECT_EQ(r_nested.len, 1);
    const auto* inner_read = static_cast<const hvl_t*>(r_nested.p);
    EXPECT_EQ(inner_read[0].len, 1);
    EXPECT_EQ(static_cast<const int32_t*>(inner_read[0].p)[0], 9);

    ScopedAttr attr(H5Acreate2(file.get(), "seq_attr", inner.get(),
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    ASSERT_GE(H5Awrite(attr.get(), inner.get(), &inner_arr2), 0);
    hvl_t attr_read{};
    ASSERT_GE(H5Aread(attr.get(), inner.get(), &attr_read), 0);
    absl::Cleanup reclaim_attr = [&] {
      H5Treclaim(inner.get(), scalar.get(), H5P_DEFAULT, &attr_read);
    };
    EXPECT_EQ(attr_read.len, 1);
    EXPECT_EQ(static_cast<const int32_t*>(attr_read.p)[0], 9);
  }
}

TEST(H5TvlenTest, ReclaimAllClassesAndErrorPaths) {
  ASSERT_GE(H5open(), 0);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // 1. ARRAY of composite (vlen<int32>) and ARRAY of simple (int32).
  {
    ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
    constexpr hsize_t kDim = 2;
    ScopedType arr_vl(H5Tarray_create2(vl_i32.get(), 1, &kDim));
    ScopedType arr_i32(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kDim));
    ASSERT_TRUE(arr_vl.valid());
    ASSERT_TRUE(arr_i32.valid());

    std::array<hvl_t, 2> arr_buf = {{
        {.len = 2, .p = std::malloc(2 * sizeof(int32_t))},
        {.len = 0, .p = nullptr},
    }};
    ASSERT_NE(arr_buf[0].p, nullptr);
    EXPECT_GE(H5Dvlen_reclaim(arr_vl.get(), scalar.get(), H5P_DEFAULT,
                              arr_buf.data()),
              0);

    std::array<int32_t, 2> simple_arr_buf = {1, 2};
    EXPECT_GE(H5Treclaim(arr_i32.get(), scalar.get(), H5P_DEFAULT,
                         simple_arr_buf.data()),
              0);
  }

  // 2. COMPOUND with composite (vlen_str, enum, complex) and simple (int32)
  //    fields, covering both H5T_ENUM and H5T_COMPLEX inside H5T__vlen_reclaim.
  {
    ScopedType vstr(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vstr.get(), H5T_VARIABLE), 0);
    ScopedType enum_type(H5Tenum_create(H5T_NATIVE_INT16));
    const int16_t eval = 1;
    ASSERT_GE(H5Tenum_insert(enum_type.get(), "ONE", &eval), 0);
    ScopedType cplx_type(H5Tcomplex_create(H5T_NATIVE_FLOAT));

    struct CompoundElem {
      int32_t tag;
      char* label;
      int16_t e;
      float c[2];
    };
    ScopedType comp(H5Tcreate(H5T_COMPOUND, sizeof(CompoundElem)));
    ASSERT_GE(H5Tinsert(comp.get(), "tag", offsetof(CompoundElem, tag),
                        H5T_NATIVE_INT32),
              0);
    ASSERT_GE(
        H5Tinsert(comp.get(), "label", offsetof(CompoundElem, label), vstr.get()),
        0);
    ASSERT_GE(
        H5Tinsert(comp.get(), "e", offsetof(CompoundElem, e), enum_type.get()),
        0);
    ASSERT_GE(
        H5Tinsert(comp.get(), "c", offsetof(CompoundElem, c), cplx_type.get()),
        0);

    CompoundElem elem{};
    elem.tag = 42;
    elem.label = static_cast<char*>(std::malloc(4));
    ASSERT_NE(elem.label, nullptr);
    std::memcpy(elem.label, "abc", 4);
    elem.e = 1;
    elem.c[0] = 1.0f;
    elem.c[1] = 2.0f;

    EXPECT_GE(H5Treclaim(comp.get(), scalar.get(), H5P_DEFAULT, &elem), 0);
  }

  // 3. Direct H5T_vlen_reclaim_elmt calls with an active API context cover all
  //    simple classes, composite no-ops, H5T_REFERENCE, invalid classes, and
  //    missing DXPL context failure.
  {
    ScopedType fixed_str(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(fixed_str.get(), 4), 0);
    ScopedType opaque(H5Tcreate(H5T_OPAQUE, 4));
    ScopedType enum_t(H5Tenum_create(H5T_NATIVE_INT32));
    ScopedType cplx_t(H5Tcomplex_create(H5T_NATIVE_FLOAT));

    H5CX_node_t api_ctx{};
    ASSERT_GE(H5CX_push(&api_ctx), 0);
    absl::Cleanup pop_ctx = [] { H5CX_pop(false); };

    uint64_t dummy_storage[4]{};
    for (hid_t tid : {H5T_NATIVE_INT32, H5T_NATIVE_DOUBLE, H5T_UNIX_D64LE,
                      fixed_str.get(), H5T_NATIVE_B32, opaque.get(),
                      enum_t.get(), cplx_t.get()}) {
      EXPECT_GE(H5T_vlen_reclaim_elmt(dummy_storage, LookupType(tid)), 0);
    }

    ScopedErrorSilence silence;
    EXPECT_LT(
        H5T_vlen_reclaim_elmt(dummy_storage, LookupType(H5T_STD_REF)), 0);

    H5T_t* opq_dt = LookupType(opaque.get());
    ASSERT_NE(opq_dt, nullptr);
    const H5T_class_t orig_cls = opq_dt->shared->type;
    absl::Cleanup restore_cls = [&] { opq_dt->shared->type = orig_cls; };
    for (H5T_class_t bad_cls : {H5T_NO_CLASS, H5T_NCLASSES}) {
      opq_dt->shared->type = bad_cls;
      EXPECT_LT(H5T_vlen_reclaim_elmt(dummy_storage, opq_dt), 0);
    }
    opq_dt->shared->type = orig_cls;

    // Invalid DXPL on a fresh context (before vl_alloc_info is cached)
    // triggers H5CX_get_vlen_alloc_info failure in H5T_vlen_reclaim_elmt.
    {
      H5CX_node_t bad_ctx{};
      ASSERT_GE(H5CX_push(&bad_ctx), 0);
      absl::Cleanup pop_bad_ctx = [] { H5CX_pop(false); };
      H5CX_set_dxpl(H5I_INVALID_HID);
      EXPECT_LT(
          H5T_vlen_reclaim_elmt(dummy_storage, LookupType(H5T_NATIVE_INT32)),
          0);
    }
  }

  // 4. Child H5T_reclaim_cb failure propagation from ARRAY, COMPOUND, and VLEN
  //    using a corrupt H5T_STD_REF element (type = H5R_MAXTYPE fails destroy).
  {
    H5R_ref_t bad_ref{};
    auto* bad_priv = reinterpret_cast<H5R_ref_priv_t*>(&bad_ref);
    bad_priv->type = H5R_MAXTYPE;

    constexpr hsize_t kOne = 1;
    ScopedType arr_ref(H5Tarray_create2(H5T_STD_REF, 1, &kOne));
    ScopedType comp_ref(H5Tcreate(H5T_COMPOUND, sizeof(H5R_ref_t)));
    ASSERT_GE(H5Tinsert(comp_ref.get(), "r", 0, H5T_STD_REF), 0);
    ScopedType vl_ref(H5Tvlen_create(H5T_STD_REF));

    ScopedErrorSilence silence;
    H5R_ref_t arr_val = bad_ref;
    EXPECT_LT(H5Treclaim(arr_ref.get(), scalar.get(), H5P_DEFAULT, &arr_val),
              0);

    H5R_ref_t comp_val = bad_ref;
    EXPECT_LT(H5Treclaim(comp_ref.get(), scalar.get(), H5P_DEFAULT, &comp_val),
              0);

    H5R_ref_t vl_elem = bad_ref;
    hvl_t vl_val{.len = 1, .p = &vl_elem};
    EXPECT_LT(H5Treclaim(vl_ref.get(), scalar.get(), H5P_DEFAULT, &vl_val), 0);
  }
}

TEST(H5TvlenTest, SetLocAndMockVolDiskCallbacks) {
  ASSERT_GE(H5open(), 0);

  GetMockVolState() = MockVolState{};
  absl::Cleanup reset_state = [] { GetMockVolState() = MockVolState{}; };

  const hid_t vol_id =
      H5VLregister_connector(&kMockVlenVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  absl::Cleanup unregister_vol = [vol_id] { H5VLunregister_connector(vol_id); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_vol(fapl.get(), vol_id, nullptr), 0);

  ScopedFile mock_file(
      H5Fcreate("mock_vlen.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(mock_file.valid());
  H5VL_object_t* file_vol =
      H5VL_vol_object_verify(mock_file.get(), H5I_FILE);
  ASSERT_NE(file_vol, nullptr);

  // Close all datatypes referencing file_vol before mock_file/vol_id tear down.
  {
    H5CX_node_t api_ctx{};
    ASSERT_GE(H5CX_push(&api_ctx), 0);
    absl::Cleanup pop_ctx = [] { H5CX_pop(false); };

    ScopedType vl_seq(H5Tvlen_create(H5T_NATIVE_INT32));
    H5T_t* dt = LookupType(vl_seq.get());
    ASSERT_NE(dt, nullptr);

    // Same location returns 0 (false).
    EXPECT_EQ(H5T__vlen_set_loc(dt, nullptr, H5T_LOC_MEMORY), 0);

    // Invalid H5T_LOC_MAXLOC returns < 0.
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T__vlen_set_loc(dt, nullptr, H5T_LOC_MAXLOC), 0);
    }

    // Container info failure when switching to H5T_LOC_DISK.
    {
      ScopedType vl_fail(H5Tvlen_create(H5T_NATIVE_INT32));
      H5T_t* dt_fail = LookupType(vl_fail.get());
      ASSERT_NE(dt_fail, nullptr);
      GetMockVolState().fail_get_cont_info = true;
      ScopedErrorSilence silence;
      EXPECT_LT(H5T__vlen_set_loc(dt_fail, file_vol, H5T_LOC_DISK), 0);
      GetMockVolState().fail_get_cont_info = false;
      EXPECT_GE(H5T__vlen_set_loc(dt_fail, nullptr, H5T_LOC_MEMORY), 0);
    }

    // Switch sequence VL type to H5T_LOC_DISK on mock_file.
    ASSERT_EQ(H5T__vlen_set_loc(dt, file_vol, H5T_LOC_DISK), 1);
    EXPECT_EQ(dt->shared->size, 4 + GetMockVolState().blob_id_size);
    EXPECT_EQ(dt->shared->owned_vol_obj, file_vol);
    EXPECT_EQ(H5T__vlen_set_loc(dt, file_vol, H5T_LOC_DISK), 0);

    std::vector<uint8_t> disk_buf(dt->shared->size, 0);
    std::vector<uint8_t> bg_buf(dt->shared->size, 0);
    bg_buf[0] = 2;  // UINT32 little-endian seq_len = 2 for background delete.
    std::array<int32_t, 2> io_vals = {10, 20};
    H5T_vlen_alloc_info_t alloc_info{};

    // Success and error paths for every H5T_vlen_disk_g callback.
    EXPECT_GE(dt->shared->u.vlen.cls->write(file_vol, &alloc_info,
                                            disk_buf.data(), io_vals.data(),
                                            nullptr, 2, sizeof(int32_t)),
              0);
    size_t seq_len = 0;
    EXPECT_GE(
        dt->shared->u.vlen.cls->getlen(file_vol, disk_buf.data(), &seq_len), 0);
    EXPECT_EQ(seq_len, 2);

    bool is_null = true;
    EXPECT_GE(
        dt->shared->u.vlen.cls->isnull(file_vol, disk_buf.data(), &is_null), 0);
    EXPECT_FALSE(is_null);

    std::array<int32_t, 2> read_vals{};
    EXPECT_GE(dt->shared->u.vlen.cls->read(file_vol, disk_buf.data(),
                                           read_vals.data(), sizeof(read_vals)),
              0);

    EXPECT_GE(
        dt->shared->u.vlen.cls->setnull(file_vol, disk_buf.data(), nullptr), 0);
    EXPECT_GE(dt->shared->u.vlen.cls->del(file_vol, nullptr), 0);
    EXPECT_GE(dt->shared->u.vlen.cls->del(file_vol, disk_buf.data()), 0);

    {
      ScopedErrorSilence silence;
      GetMockVolState().fail_blob_isnull = true;
      EXPECT_LT(
          dt->shared->u.vlen.cls->isnull(file_vol, disk_buf.data(), &is_null),
          0);
      GetMockVolState().fail_blob_isnull = false;

      GetMockVolState().fail_blob_get = true;
      EXPECT_LT(dt->shared->u.vlen.cls->read(
                    file_vol, disk_buf.data(), read_vals.data(),
                    sizeof(read_vals)),
                0);
      GetMockVolState().fail_blob_get = false;

      GetMockVolState().fail_blob_put = true;
      EXPECT_LT(dt->shared->u.vlen.cls->write(file_vol, &alloc_info,
                                              disk_buf.data(), io_vals.data(),
                                              nullptr, 2, sizeof(int32_t)),
                0);
      GetMockVolState().fail_blob_put = false;

      GetMockVolState().fail_blob_setnull = true;
      EXPECT_LT(
          dt->shared->u.vlen.cls->setnull(file_vol, disk_buf.data(), nullptr),
          0);
      GetMockVolState().fail_blob_setnull = false;

      GetMockVolState().fail_blob_delete = true;
      EXPECT_LT(dt->shared->u.vlen.cls->del(file_vol, bg_buf.data()), 0);
      EXPECT_LT(dt->shared->u.vlen.cls->setnull(file_vol, disk_buf.data(),
                                                bg_buf.data()),
                0);
      EXPECT_LT(dt->shared->u.vlen.cls->write(file_vol, &alloc_info,
                                              disk_buf.data(), io_vals.data(),
                                              bg_buf.data(), 2,
                                              sizeof(int32_t)),
                0);
      GetMockVolState().fail_blob_delete = false;
    }

    // Switch back to H5T_LOC_MEMORY frees owned_vol_obj cleanly.
    EXPECT_EQ(H5T__vlen_set_loc(dt, nullptr, H5T_LOC_MEMORY), 1);
    EXPECT_EQ(dt->shared->owned_vol_obj, nullptr);
    EXPECT_EQ(dt->shared->size, sizeof(hvl_t));

    // Switch H5T_LOC_BADLOC.
    EXPECT_EQ(H5T__vlen_set_loc(dt, nullptr, H5T_LOC_BADLOC), 1);
    EXPECT_EQ(dt->shared->u.vlen.cls, nullptr);
    EXPECT_EQ(H5T__vlen_set_loc(dt, nullptr, H5T_LOC_MEMORY), 1);

    // Also test H5T_LOC_DISK <-> H5T_LOC_MEMORY transition on a VL string type.
    ScopedType vstr(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vstr.get(), H5T_VARIABLE), 0);
    H5T_t* vstr_dt = LookupType(vstr.get());
    ASSERT_NE(vstr_dt, nullptr);
    EXPECT_EQ(H5T__vlen_set_loc(vstr_dt, file_vol, H5T_LOC_DISK), 1);
    EXPECT_EQ(H5T__vlen_set_loc(vstr_dt, nullptr, H5T_LOC_MEMORY), 1);
    EXPECT_EQ(vstr_dt->shared->size, sizeof(char*));
  }
}

}  // namespace
}  // namespace hdf5
