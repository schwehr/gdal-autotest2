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
// Tests superblock (H5AC_SUPERBLOCK) and driver info block (H5AC_DRVRINFO)
// metadata cache callbacks, prefix decoders, serialization, checksum
// verification, and corrupted-buffer handling across superblock versions v0-v3
// (third_party/hdf5/src/H5Fsuper_cache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fsuper_cache.c

#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits #ifndef NDEBUG fields from
// H5C_cache_entry_t (H5AC_info_t) inside H5F_super_t and H5O_drvinfo_t.
// Define NDEBUG around hdf5_internal_prelude.h and internal headers so struct
// offsets match `:hdf5`.
#ifndef NDEBUG
#define H5FSUPER_CACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_try_param
extern "C" {
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDdevelop.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5FSUPER_CACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FSUPER_CACHE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(H5AC__SUPERBLOCK_TAG, &prev_tag_);
      tagged_ = true;
    }
  }
  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }
  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_ = {};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

haddr_t FailingGetEoa(const H5FD_t* /*file*/, H5FD_mem_t /*type*/) {
  return HADDR_UNDEF;
}

herr_t FailingSetEoa(H5FD_t* /*file*/, H5FD_mem_t /*type*/, haddr_t /*addr*/) {
  return -1;
}

herr_t FailingSbEncode(H5FD_t* /*file*/, char* /*name*/, unsigned char* /*p*/) {
  return -1;
}

std::vector<uint8_t> ExtractCoreImage(hid_t file_id) {
  const ssize_t needed = H5Fget_file_image(file_id, nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::vector<uint8_t> buf(static_cast<size_t>(needed));
  const ssize_t got = H5Fget_file_image(file_id, buf.data(), buf.size());
  if (got != needed) {
    return {};
  }
  return buf;
}

std::vector<uint8_t> SerializeSuperblock(H5F_t* f, H5F_super_t* sblock) {
  size_t len = 0;
  if (H5AC_SUPERBLOCK[0].image_len(sblock, &len) < 0 || len == 0) {
    return {};
  }
  std::vector<uint8_t> image(len);
  if (H5AC_SUPERBLOCK[0].serialize(f, image.data(), image.size(), sblock) < 0) {
    return {};
  }
  return image;
}

H5F_superblock_cache_ud_t MakeSuperblockUdata(H5F_t* f, unsigned super_vers) {
  H5F_superblock_cache_ud_t udata = {};
  udata.f = f;
  udata.ignore_drvrinfo = false;
  udata.sym_leaf_k = 0;
  udata.btree_k[H5B_SNODE_ID] = HDF5_BTREE_SNODE_IK_DEF;
  udata.btree_k[H5B_CHUNK_ID] = HDF5_BTREE_CHUNK_IK_DEF;
  udata.stored_eof = HADDR_UNDEF;
  udata.drvrinfo_removed = false;
  udata.super_vers = super_vers;
  return udata;
}

TEST(H5FsuperCacheTest, EndToEndRoundTripCoversSuperblockVersionsZeroToThree) {
  struct VersionCase {
    H5F_libver_t low;
    H5F_libver_t high;
    unsigned istore_k;
    unsigned expected_super_vers;
  };
  constexpr VersionCase kCases[] = {
      {H5F_LIBVER_EARLIEST, H5F_LIBVER_V18, 32, 0},
      {H5F_LIBVER_EARLIEST, H5F_LIBVER_V18, 16, 1},
      {H5F_LIBVER_V18, H5F_LIBVER_V18, 32, 2},
      {H5F_LIBVER_V110, H5F_LIBVER_LATEST, 32, 3},
  };

  for (const auto& tc : kCases) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_istore_k(fcpl.get(), tc.istore_k), 0);

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), tc.low, tc.high), 0);

    ScopedFile file(H5Fcreate("sb_vers.h5", H5F_ACC_TRUNC, fcpl.get(),
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    H5F_info2_t info = {};
    ASSERT_GE(H5Fget_info2(file.get(), &info), 0);
    EXPECT_EQ(info.super.version, tc.expected_super_vers);

    // Extract flushed image and reopen via H5Pset_file_image to test load.
    std::vector<uint8_t> img = ExtractCoreImage(file.get());
    ASSERT_FALSE(img.empty());
    file.reset();

    ScopedPlist open_fapl = MakeCoreFapl();
    ASSERT_TRUE(open_fapl.valid());
    ASSERT_GE(H5Pset_file_image(open_fapl.get(), img.data(), img.size()), 0);

    ScopedFile reopened(
        H5Fopen("reopen_sb.h5", H5F_ACC_RDONLY, open_fapl.get()));
    ASSERT_TRUE(reopened.valid());
    H5F_info2_t reopened_info = {};
    ASSERT_GE(H5Fget_info2(reopened.get(), &reopened_info), 0);
    EXPECT_EQ(reopened_info.super.version, tc.expected_super_vers);
  }
}

TEST(H5FsuperCacheTest, ChecksumMismatchInV2SuperblockFailsOpen) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18),
            0);

  std::vector<uint8_t> img;
  {
    ScopedFile file(H5Fcreate("v2_chksum.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    img = ExtractCoreImage(file.get());
    ASSERT_GT(img.size(), 48U);
  }

  // Corrupt the root address / status byte inside v2 superblock (byte 12).
  img[12] ^= 0xFF;

  ScopedPlist open_fapl = MakeCoreFapl();
  ASSERT_TRUE(open_fapl.valid());
  ASSERT_GE(H5Pset_file_image(open_fapl.get(), img.data(), img.size()), 0);

  ScopedErrorSilence silence;
  ScopedFile bad(H5Fopen("v2_chksum_bad.h5", H5F_ACC_RDONLY, open_fapl.get()));
  EXPECT_FALSE(bad.valid());
}

TEST(H5FsuperCacheTest, FamilyAndMultiVfdRoundTripDriverInfoAndIgnoreFlag) {
  TempDir dir("super_cache_family");
  const std::string family_pattern = dir.child("fam%05d.h5").string();

  // 1. Create a v0 Family file so H5AC_DRVRINFO serializes/flushes cleanly.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_family(fapl.get(), /*memb_size=*/65536, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile file(H5Fcreate(family_pattern.c_str(), H5F_ACC_TRUNC,
                              H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }

  // 2. Reopen Family file to exercise H5AC_DRVRINFO load/deserialize callbacks.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_family(fapl.get(), /*memb_size=*/65536, H5P_DEFAULT),
              0);
    ScopedFile file(
        H5Fopen(family_pattern.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    EXPECT_NE(f->shared->drvinfo, nullptr);

    ScopedApiContext ctx;
    size_t drvr_len = 0;
    EXPECT_GE(H5AC_DRVRINFO[0].image_len(f->shared->drvinfo, &drvr_len), 0);
    EXPECT_GT(drvr_len, static_cast<size_t>(H5F_DRVINFOBLOCK_HDR_SIZE));
  }

  // 3. Open member 0 via Sec2 + H5F_ACS_FAMILY_TO_SINGLE_NAME to remove
  //    driver info block in v0 superblock deserialize.
  {
    const std::string member0 = dir.child("fam00000.h5").string();
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
    bool to_single = true;
    ASSERT_GE(
        H5Pset(fapl.get(), H5F_ACS_FAMILY_TO_SINGLE_NAME, &to_single), 0);
    ScopedFile single(H5Fopen(member0.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(single.valid());
    H5F_t* f = UnwrapFile(single.get());
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->shared->drvinfo, nullptr);
  }

  // 4. Exercise Split/Multi driver info encoding & decoding on v0 superblock.
  {
    const std::string split_base = dir.child("split_file").string();
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_split(fapl.get(), "-m.h5", H5P_DEFAULT, "-r.h5",
                                H5P_DEFAULT),
              0);
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile split_w(H5Fcreate(split_base.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                                 fapl.get()));
    ASSERT_TRUE(split_w.valid());
    split_w.reset();

    ScopedFile split_r(H5Fopen(split_base.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(split_r.valid());
  }
}

TEST(H5FsuperCacheTest, SuperblockCallbacksHandleLoadVerifyDeserializeAndErrors) {
  // Exercise H5AC_SUPERBLOCK[0] callbacks directly on v0, v1, and v2 images.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
      0);

  ScopedFile v0_file(
      H5Fcreate("cb_v0.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(v0_file.valid());
  H5F_t* f0 = UnwrapFile(v0_file.get());
  ASSERT_NE(f0, nullptr);
  ScopedApiContext ctx;
  const haddr_t orig_f0_eoa = H5FD_get_eoa(f0->shared->lf, H5FD_MEM_DEFAULT);
  ASSERT_NE(orig_f0_eoa, HADDR_UNDEF);
  absl::Cleanup restore_f0_eoa = [&]() {
    H5FD_set_eoa(f0->shared->lf, H5FD_MEM_DEFAULT, orig_f0_eoa);
  };

  // Serialize v0 superblock and verify initial/final load size + round-trip.
  std::vector<uint8_t> v0_img = SerializeSuperblock(f0, f0->shared->sblock);
  ASSERT_FALSE(v0_img.empty());

  size_t init_len = 0;
  EXPECT_GE(H5AC_SUPERBLOCK[0].get_initial_load_size(nullptr, &init_len), 0);
  EXPECT_EQ(init_len, static_cast<size_t>(H5F_SUPERBLOCK_SPEC_READ_SIZE));

  {
    H5F_superblock_cache_ud_t ud = MakeSuperblockUdata(f0, 0);
    size_t actual_len = v0_img.size();
    EXPECT_GE(H5AC_SUPERBLOCK[0].get_final_load_size(v0_img.data(),
                                                     v0_img.size(), &ud,
                                                     &actual_len),
              0);
    EXPECT_EQ(actual_len, v0_img.size());
    EXPECT_GT(H5AC_SUPERBLOCK[0].verify_chksum(v0_img.data(), v0_img.size(),
                                               &ud),
              0);

    bool dirty = false;
    void* loaded =
        H5AC_SUPERBLOCK[0].deserialize(v0_img.data(), v0_img.size(), &ud,
                                       &dirty);
    ASSERT_NE(loaded, nullptr);
    EXPECT_GE(H5AC_SUPERBLOCK[0].free_icr(loaded), 0);
  }

  // Test ignore_drvrinfo path inside v0 deserialize when driver_addr != UNDEF.
  {
    ScopedFile v0_drv = CreateInMemoryFile("cb_v0_drv.h5");
    H5F_t* fd = UnwrapFile(v0_drv.get());
    ASSERT_NE(fd, nullptr);
    const haddr_t orig_drv = f0->shared->sblock->driver_addr;
    f0->shared->sblock->driver_addr = 256;
    std::vector<uint8_t> img_drv = SerializeSuperblock(f0, f0->shared->sblock);
    f0->shared->sblock->driver_addr = orig_drv;

    H5F_superblock_cache_ud_t ud = MakeSuperblockUdata(fd, 0);
    ud.ignore_drvrinfo = true;
    bool dirty = false;
    void* loaded = H5AC_SUPERBLOCK[0].deserialize(img_drv.data(),
                                                  img_drv.size(), &ud, &dirty);
    ASSERT_NE(loaded, nullptr);
    auto* sb = static_cast<H5F_super_t*>(loaded);
    EXPECT_EQ(sb->driver_addr, HADDR_UNDEF);
    EXPECT_TRUE(ud.drvrinfo_removed);
    EXPECT_GE(H5AC_SUPERBLOCK[0].free_icr(loaded), 0);
  }

  // Create a v2 file image and exercise verify_chksum & v2 deserialize.
  ScopedPlist fapl_v2 = MakeCoreFapl();
  ASSERT_TRUE(fapl_v2.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_v2.get(), H5F_LIBVER_V18, H5F_LIBVER_V18), 0);
  ScopedFile v2_file(
      H5Fcreate("cb_v2.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v2.get()));
  ASSERT_TRUE(v2_file.valid());
  H5F_t* f2 = UnwrapFile(v2_file.get());
  ASSERT_NE(f2, nullptr);
  const haddr_t orig_f2_eoa = H5FD_get_eoa(f2->shared->lf, H5FD_MEM_DEFAULT);
  ASSERT_NE(orig_f2_eoa, HADDR_UNDEF);
  absl::Cleanup restore_f2_eoa = [&]() {
    H5FD_set_eoa(f2->shared->lf, H5FD_MEM_DEFAULT, orig_f2_eoa);
  };

  std::vector<uint8_t> v2_img = SerializeSuperblock(f2, f2->shared->sblock);
  ASSERT_FALSE(v2_img.empty());

  {
    H5F_superblock_cache_ud_t ud = MakeSuperblockUdata(f2, 2);
    size_t actual_len = v2_img.size();
    EXPECT_GE(H5AC_SUPERBLOCK[0].get_final_load_size(v2_img.data(),
                                                     v2_img.size(), &ud,
                                                     &actual_len),
              0);
    EXPECT_EQ(actual_len, v2_img.size());
    EXPECT_GT(H5AC_SUPERBLOCK[0].verify_chksum(v2_img.data(), v2_img.size(),
                                               &ud),
              0);

    bool dirty = false;
    void* loaded =
        H5AC_SUPERBLOCK[0].deserialize(v2_img.data(), v2_img.size(), &ud,
                                       &dirty);
    ASSERT_NE(loaded, nullptr);
    EXPECT_GE(H5AC_SUPERBLOCK[0].free_icr(loaded), 0);

    // Corrupted checksum -> verify_chksum returns false (0).
    std::vector<uint8_t> bad_chk = v2_img;
    bad_chk.back() ^= 0x01;
    EXPECT_EQ(H5AC_SUPERBLOCK[0].verify_chksum(bad_chk.data(), bad_chk.size(),
                                               &ud),
              0);

    // Truncated buffer shorter than 4-byte checksum fails H5F_get_checksums.
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_SUPERBLOCK[0].verify_chksum(v2_img.data(), 2, &ud), 0);
  }

  // Error branches in superblock_prefix_decode, get_final_load_size, and
  // deserialize.
  ScopedErrorSilence silence;
  const uint8_t saved_sizeof_addr = f0->shared->sizeof_addr;
  const uint8_t saved_sizeof_size = f0->shared->sizeof_size;
  absl::Cleanup restore_sizes = [&]() {
    f0->shared->sizeof_addr = saved_sizeof_addr;
    f0->shared->sizeof_size = saved_sizeof_size;
  };

  // Helper to deserialize an image slice safely.
  auto try_deserialize_fail = [&](const std::vector<uint8_t>& buf, size_t len,
                                  H5F_t* f, unsigned vers) {
    H5F_superblock_cache_ud_t ud = MakeSuperblockUdata(f, vers);
    bool dirty = false;
    void* res = H5AC_SUPERBLOCK[0].deserialize(buf.data(), len, &ud, &dirty);
    if (res != nullptr) {
      H5AC_SUPERBLOCK[0].free_icr(res);
    }
    EXPECT_EQ(res, nullptr);
  };

  // Truncations inside prefix_decode: < 8 bytes (sig), 8 bytes (vers),
  // 14 bytes (v0 addr/size), 10 bytes (v2 addr/size).
  for (size_t bad_len : {size_t{4}, size_t{8}, size_t{14}}) {
    try_deserialize_fail(v0_img, bad_len, f0, 0);
  }
  try_deserialize_fail(v2_img, 10, f2, 2);

  // Bad super_vers (> latest), bad sizeof_addr, bad sizeof_size.
  {
    std::vector<uint8_t> bad = v0_img;
    bad[8] = HDF5_SUPERBLOCK_VERSION_LATEST + 1;
    H5F_superblock_cache_ud_t ud = MakeSuperblockUdata(f0, 4);
    size_t actual = bad.size();
    EXPECT_LT(H5AC_SUPERBLOCK[0].get_final_load_size(bad.data(), bad.size(),
                                                     &ud, &actual),
              0);
    try_deserialize_fail(bad, bad.size(), f0, 4);
  }
  {
    std::vector<uint8_t> bad = v0_img;
    bad[13] = 3;  // invalid sizeof_addr
    try_deserialize_fail(bad, bad.size(), f0, 0);
    bad = v0_img;
    bad[14] = 5;  // invalid sizeof_size
    try_deserialize_fail(bad, bad.size(), f0, 0);
  }

  // get_final_load_size failure when H5F__set_eoa fails.
  {
    const H5FD_class_t* orig_cls = f0->shared->lf->cls;
    H5FD_class_t failing_cls = *orig_cls;
    failing_cls.set_eoa = FailingSetEoa;
    absl::Cleanup restore_cls = [&]() { f0->shared->lf->cls = orig_cls; };
    f0->shared->lf->cls = &failing_cls;

    H5F_superblock_cache_ud_t ud = MakeSuperblockUdata(f0, 0);
    size_t actual = v0_img.size();
    EXPECT_LT(H5AC_SUPERBLOCK[0].get_final_load_size(v0_img.data(),
                                                     v0_img.size(), &ud,
                                                     &actual),
              0);
  }

  // v0 invalid field values inside deserialize:
  // byte 9 = freespace_vers, byte 10 = objdir_vers, byte 12 = shdr_vers,
  // bytes 16..17 = sym_leaf_k, bytes 18..19 = snode_btree_k, bytes 20..23 =
  // status_flags.
  {
    std::vector<uint8_t> bad = v0_img;
    bad[9] = 99;
    try_deserialize_fail(bad, bad.size(), f0, 0);

    bad = v0_img;
    bad[10] = 99;
    try_deserialize_fail(bad, bad.size(), f0, 0);

    bad = v0_img;
    bad[12] = 99;
    try_deserialize_fail(bad, bad.size(), f0, 0);

    bad = v0_img;
    bad[16] = 0;
    bad[17] = 0;  // sym_leaf_k == 0
    try_deserialize_fail(bad, bad.size(), f0, 0);

    bad = v0_img;
    bad[18] = 0;
    bad[19] = 0;  // snode_btree_k == 0
    try_deserialize_fail(bad, bad.size(), f0, 0);

    bad = v0_img;
    bad[21] = 1;  // status_flags > 255
    try_deserialize_fail(bad, bad.size(), f0, 0);

    bad = v0_img;
    bad[20] = 0x80;  // unknown status flag bit
    try_deserialize_fail(bad, bad.size(), f0, 0);
  }

  // v0 progressive overflows in deserialize at each field boundary.
  for (size_t trunc_len :
       {size_t{15}, size_t{16}, size_t{17}, size_t{18}, size_t{19}, size_t{20},
        size_t{22}, size_t{24}, size_t{26}, size_t{60}}) {
    if (trunc_len < v0_img.size()) {
      try_deserialize_fail(v0_img, trunc_len, f0, 0);
    }
  }

  // v1 overflows at chunk_btree_k (byte 24..25) and reserved (byte 26..27).
  {
    ScopedPlist v1_fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(v1_fcpl.valid());
    ASSERT_GE(H5Pset_istore_k(v1_fcpl.get(), 16), 0);
    ScopedFile v1_file(
        H5Fcreate("cb_v1.h5", H5F_ACC_TRUNC, v1_fcpl.get(), fapl.get()));
    ASSERT_TRUE(v1_file.valid());
    H5F_t* f1 = UnwrapFile(v1_file.get());
    ASSERT_NE(f1, nullptr);
    std::vector<uint8_t> v1_img = SerializeSuperblock(f1, f1->shared->sblock);
    ASSERT_FALSE(v1_img.empty());
    try_deserialize_fail(v1_img, 24, f1, 1);
    try_deserialize_fail(v1_img, 26, f1, 1);
  }

  // v2/v3 bad status flag and progressive overflows.
  {
    std::vector<uint8_t> bad = v2_img;
    bad[11] = 0x80;  // bad status_flags
    try_deserialize_fail(bad, bad.size(), f2, 2);

    for (size_t trunc_len : {size_t{11}, size_t{20}, v2_img.size() - 2}) {
      try_deserialize_fail(v2_img, trunc_len, f2, 2);
    }
  }

  // Serialize failure paths (get_eoa == HADDR_UNDEF, root_grp == nullptr).
  {
    const H5FD_class_t* orig_cls = f0->shared->lf->cls;
    H5FD_class_t failing_cls = *orig_cls;
    failing_cls.get_eoa = FailingGetEoa;
    absl::Cleanup restore_cls = [&]() { f0->shared->lf->cls = orig_cls; };
    f0->shared->lf->cls = &failing_cls;

    std::vector<uint8_t> out(v0_img.size());
    EXPECT_LT(H5AC_SUPERBLOCK[0].serialize(f0, out.data(), out.size(),
                                           f0->shared->sblock),
              0);
  }
  {
    const H5FD_class_t* orig_cls = f2->shared->lf->cls;
    H5FD_class_t failing_cls = *orig_cls;
    failing_cls.get_eoa = FailingGetEoa;
    absl::Cleanup restore_cls = [&]() { f2->shared->lf->cls = orig_cls; };
    f2->shared->lf->cls = &failing_cls;

    std::vector<uint8_t> out(v2_img.size());
    EXPECT_LT(H5AC_SUPERBLOCK[0].serialize(f2, out.data(), out.size(),
                                           f2->shared->sblock),
              0);
  }
  {
    H5G_t* orig_root = f2->shared->root_grp;
    absl::Cleanup restore_root = [&]() { f2->shared->root_grp = orig_root; };
    f2->shared->root_grp = nullptr;

    std::vector<uint8_t> out(v2_img.size());
    EXPECT_LT(H5AC_SUPERBLOCK[0].serialize(f2, out.data(), out.size(),
                                           f2->shared->sblock),
              0);
  }
}

TEST(H5FsuperCacheTest, DriverInfoCallbacksHandleValidAndErrorInputs) {
  TempDir dir("drvrinfo_cb");
  const std::string family_pattern = dir.child("drvcb%05d.h5").string();

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_fapl_family(fapl.get(), /*memb_size=*/65536, H5P_DEFAULT), 0);
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
      0);

  ScopedFile file(H5Fcreate(family_pattern.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(f->shared->drvinfo, nullptr);
  ScopedApiContext ctx;

  // 1. Serialize existing drvinfo block and test load size + deserialize.
  size_t len = 0;
  EXPECT_GE(H5AC_DRVRINFO[0].image_len(f->shared->drvinfo, &len), 0);
  ASSERT_GE(len, static_cast<size_t>(H5F_DRVINFOBLOCK_HDR_SIZE));
  std::vector<uint8_t> img(len);
  EXPECT_GE(
      H5AC_DRVRINFO[0].serialize(f, img.data(), img.size(), f->shared->drvinfo),
      0);

  size_t init_len = 0;
  EXPECT_GE(H5AC_DRVRINFO[0].get_initial_load_size(nullptr, &init_len), 0);
  EXPECT_EQ(init_len, static_cast<size_t>(H5F_DRVINFOBLOCK_HDR_SIZE));

  H5F_drvrinfo_cache_ud_t ud = {f, f->shared->sblock->driver_addr};
  size_t actual_len = H5F_DRVINFOBLOCK_HDR_SIZE;
  EXPECT_GE(H5AC_DRVRINFO[0].get_final_load_size(img.data(), init_len, &ud,
                                                 &actual_len),
            0);
  EXPECT_EQ(actual_len, img.size());

  bool dirty = false;
  void* loaded =
      H5AC_DRVRINFO[0].deserialize(img.data(), img.size(), &ud, &dirty);
  ASSERT_NE(loaded, nullptr);
  EXPECT_GE(H5AC_DRVRINFO[0].free_icr(loaded), 0);

  // Also test extend_eoa inside get_final_load_size when driver_addr + size >
  // eoa (restoring orig_eoa afterward so close flushes cleanly).
  const haddr_t orig_drvr_eoa = H5FD_get_eoa(f->shared->lf, H5FD_MEM_DEFAULT);
  ASSERT_NE(orig_drvr_eoa, HADDR_UNDEF);
  absl::Cleanup restore_drvr_eoa = [&]() {
    H5FD_set_eoa(f->shared->lf, H5FD_MEM_DEFAULT, orig_drvr_eoa);
  };
  {
    H5F_drvrinfo_cache_ud_t large_ud = {f, orig_drvr_eoa + 512};
    size_t act = H5F_DRVINFOBLOCK_HDR_SIZE;
    EXPECT_GE(H5AC_DRVRINFO[0].get_final_load_size(img.data(), init_len,
                                                   &large_ud, &act),
              0);
  }

  // 2. Error paths in drvrinfo_prefix_decode, get_final_load_size,
  //    deserialize, and serialize.
  ScopedErrorSilence silence;
  auto expect_drvr_deserialize_fail = [&](const uint8_t* data, size_t n) {
    void* res = H5AC_DRVRINFO[0].deserialize(data, n, &ud, &dirty);
    if (res != nullptr) {
      H5AC_DRVRINFO[0].free_icr(res);
    }
    EXPECT_EQ(res, nullptr);
  };

  // Invalid driver info version byte (!= 0).
  {
    std::vector<uint8_t> bad = img;
    bad[0] = 9;
    size_t act = H5F_DRVINFOBLOCK_HDR_SIZE;
    EXPECT_LT(H5AC_DRVRINFO[0].get_final_load_size(bad.data(), init_len, &ud,
                                                   &act),
              0);
    expect_drvr_deserialize_fail(bad.data(), bad.size());
  }

  // Truncated lengths in prefix decode: get_final_load_size skips drv_name
  // (8-byte prefix), while deserialize decodes drv_name too (16-byte prefix).
  for (size_t bad_len : {size_t{1}, size_t{2}, size_t{5}}) {
    size_t act = bad_len;
    EXPECT_LT(H5AC_DRVRINFO[0].get_final_load_size(img.data(), bad_len, &ud,
                                                   &act),
              0);
    expect_drvr_deserialize_fail(img.data(), bad_len);
  }
  expect_drvr_deserialize_fail(img.data(), 10);

  // Mismatched driver name ("NCSAmult" on a "family" file) causes H5FD_sb_load
  // to fail inside deserialize and release drvinfo.
  {
    std::vector<uint8_t> bad_name = img;
    std::memcpy(bad_name.data() + 8, "NCSAmult", 8);
    expect_drvr_deserialize_fail(bad_name.data(), bad_name.size());
  }

  // Failing get_eoa and set_eoa during get_final_load_size, plus failing
  // sb_encode during serialize.
  {
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t failing_cls = *orig_cls;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };

    failing_cls.get_eoa = FailingGetEoa;
    f->shared->lf->cls = &failing_cls;
    size_t act = H5F_DRVINFOBLOCK_HDR_SIZE;
    EXPECT_LT(H5AC_DRVRINFO[0].get_final_load_size(img.data(), init_len, &ud,
                                                   &act),
              0);

    failing_cls.get_eoa = orig_cls->get_eoa;
    failing_cls.set_eoa = FailingSetEoa;
    H5F_drvrinfo_cache_ud_t large_ud = {f, HADDR_MAX - 1024};
    EXPECT_LT(H5AC_DRVRINFO[0].get_final_load_size(img.data(), init_len,
                                                   &large_ud, &act),
              0);

    failing_cls.set_eoa = orig_cls->set_eoa;
    failing_cls.sb_encode = FailingSbEncode;
    std::vector<uint8_t> out(img.size());
    EXPECT_LT(
        H5AC_DRVRINFO[0].serialize(f, out.data(), out.size(),
                                   f->shared->drvinfo),
        0);
  }
}

}  // namespace
}  // namespace hdf5
