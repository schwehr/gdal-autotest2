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
// Tests driver info object header message encoding, decoding, copying, sizing,
// resetting, and debugging routines in third_party/hdf5/src/H5Odrvinfo.c,
// including superblock extension driver-info persistence.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Odrvinfo.c

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits #ifndef NDEBUG fields from
// H5C_cache_entry_t (H5AC_info_t), the first member of H5O_drvinfo_t and
// H5F_super_t. Align NDEBUG around hdf5_internal_prelude.h and internal
// headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5ODRVINFO_TEST_RESTORE_UNDEF_NDEBUG 1
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
#define H5O_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDdevelop.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5ODRVINFO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ODRVINFO_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

class ScopedApiContext {
 public:
  explicit ScopedApiContext(H5AC_ring_t ring = H5AC_RING_SBE,
                            haddr_t tag = H5AC__SUPERBLOCK_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_set_ring(ring, &prev_ring_);
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }
  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
      H5AC_set_ring(prev_ring_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }
  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  H5AC_ring_t prev_ring_ = H5AC_RING_INV;
  bool pushed_ = false;
  bool tagged_ = false;
};

// Minimal in-memory VFD implementing sb_size/sb_encode/sb_decode so superblock
// extension H5O_DRVINFO_ID creation/read/update/debug can run hermetically
// without touching real disk except for explicit Family VFD coverage.
struct MemDrvFile {
  H5FD_t pub{};
  std::string key;
  haddr_t eoa = 0;
  haddr_t eof = 0;
};

struct MemDrvState {
  std::unordered_map<std::string, std::vector<uint8_t>> files;
  std::vector<uint8_t> drv_payload{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  std::vector<uint8_t> decoded_payload;
  char decoded_name[9] = {};
};

MemDrvState* g_mem_drv_state = nullptr;

hsize_t MemDrvSbSize(H5FD_t* /*file*/) {
  return g_mem_drv_state != nullptr ? g_mem_drv_state->drv_payload.size() : 0;
}

herr_t MemDrvSbEncode(H5FD_t* /*file*/, char* name, unsigned char* p) {
  std::memcpy(name, "MEMDRV01", 8);
  if (g_mem_drv_state != nullptr && !g_mem_drv_state->drv_payload.empty()) {
    std::memcpy(p, g_mem_drv_state->drv_payload.data(),
                g_mem_drv_state->drv_payload.size());
  }
  return 0;
}

herr_t MemDrvSbDecode(H5FD_t* /*file*/, const char* name,
                      const unsigned char* p) {
  if (g_mem_drv_state == nullptr) {
    return -1;
  }
  std::memcpy(g_mem_drv_state->decoded_name, name, 8);
  g_mem_drv_state->decoded_name[8] = '\0';
  g_mem_drv_state->decoded_payload.assign(
      p, p + g_mem_drv_state->drv_payload.size());
  return 0;
}

H5FD_t* MemDrvOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                   haddr_t /*maxaddr*/) {
  if (g_mem_drv_state == nullptr || name == nullptr) {
    return nullptr;
  }
  if (flags & H5F_ACC_TRUNC) {
    g_mem_drv_state->files[name].clear();
  } else if (flags & H5F_ACC_CREAT) {
    if ((flags & H5F_ACC_EXCL) &&
        g_mem_drv_state->files.find(name) != g_mem_drv_state->files.end()) {
      return nullptr;
    }
    g_mem_drv_state->files.emplace(name, std::vector<uint8_t>{});
  } else {
    if (g_mem_drv_state->files.find(name) == g_mem_drv_state->files.end()) {
      return nullptr;
    }
  }
  auto* f = new MemDrvFile{};
  f->key = name;
  f->eof = static_cast<haddr_t>(g_mem_drv_state->files[name].size());
  return &f->pub;
}

herr_t MemDrvClose(H5FD_t* file) {
  delete reinterpret_cast<MemDrvFile*>(file);
  return 0;
}

int MemDrvCmp(const H5FD_t* f1, const H5FD_t* f2) {
  const auto* m1 = reinterpret_cast<const MemDrvFile*>(f1);
  const auto* m2 = reinterpret_cast<const MemDrvFile*>(f2);
  if (m1->key < m2->key) return -1;
  if (m1->key > m2->key) return 1;
  return 0;
}

herr_t MemDrvQuery(const H5FD_t* /*file*/, unsigned long* flags) {
  *flags = H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_ACCUMULATE_METADATA |
           H5FD_FEAT_AGGREGATE_SMALLDATA;
  return 0;
}

haddr_t MemDrvGetEoa(const H5FD_t* file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const MemDrvFile*>(file)->eoa;
}

herr_t MemDrvSetEoa(H5FD_t* file, H5FD_mem_t /*type*/, haddr_t addr) {
  reinterpret_cast<MemDrvFile*>(file)->eoa = addr;
  return 0;
}

haddr_t MemDrvGetEof(const H5FD_t* file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const MemDrvFile*>(file)->eof;
}

herr_t MemDrvRead(H5FD_t* file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                  haddr_t addr, size_t size, void* buf) {
  const auto* mf = reinterpret_cast<const MemDrvFile*>(file);
  const auto it = g_mem_drv_state->files.find(mf->key);
  if (it == g_mem_drv_state->files.end()) {
    return -1;
  }
  const std::vector<uint8_t>& data = it->second;
  size_t copied = 0;
  if (addr < data.size()) {
    const size_t avail = data.size() - static_cast<size_t>(addr);
    copied = avail < size ? avail : size;
    std::memcpy(buf, data.data() + addr, copied);
  }
  if (copied < size) {
    std::memset(static_cast<uint8_t*>(buf) + copied, 0, size - copied);
  }
  return 0;
}

herr_t MemDrvWrite(H5FD_t* file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                   haddr_t addr, size_t size, const void* buf) {
  auto* mf = reinterpret_cast<MemDrvFile*>(file);
  auto it = g_mem_drv_state->files.find(mf->key);
  if (it == g_mem_drv_state->files.end()) {
    return -1;
  }
  std::vector<uint8_t>& data = it->second;
  const size_t end = static_cast<size_t>(addr) + size;
  if (end > data.size()) {
    data.resize(end, 0);
  }
  std::memcpy(data.data() + addr, buf, size);
  mf->eof = static_cast<haddr_t>(data.size());
  return 0;
}

herr_t MemDrvTruncate(H5FD_t* file, hid_t /*dxpl_id*/, bool /*closing*/) {
  auto* mf = reinterpret_cast<MemDrvFile*>(file);
  auto it = g_mem_drv_state->files.find(mf->key);
  if (it == g_mem_drv_state->files.end()) {
    return -1;
  }
  if (mf->eoa != mf->eof) {
    it->second.resize(static_cast<size_t>(mf->eoa), 0);
    mf->eof = mf->eoa;
  }
  return 0;
}

class ScopedMemDrvVfd {
 public:
  ScopedMemDrvVfd() {
    prev_ = g_mem_drv_state;
    g_mem_drv_state = &state_;

    H5FD_class_t cls = {};
    cls.version = H5FD_CLASS_VERSION;
    cls.value = static_cast<H5FD_class_value_t>(512);
    cls.name = "memdrv_drvinfo_test";
    cls.maxaddr = static_cast<haddr_t>((~size_t{0}) - 1);
    cls.fc_degree = H5F_CLOSE_WEAK;
    cls.sb_size = MemDrvSbSize;
    cls.sb_encode = MemDrvSbEncode;
    cls.sb_decode = MemDrvSbDecode;
    cls.open = MemDrvOpen;
    cls.close = MemDrvClose;
    cls.cmp = MemDrvCmp;
    cls.query = MemDrvQuery;
    cls.get_eoa = MemDrvGetEoa;
    cls.set_eoa = MemDrvSetEoa;
    cls.get_eof = MemDrvGetEof;
    cls.read = MemDrvRead;
    cls.write = MemDrvWrite;
    cls.truncate = MemDrvTruncate;
    cls.fl_map[H5FD_MEM_DEFAULT] = H5FD_MEM_SUPER;
    cls.fl_map[H5FD_MEM_SUPER] = H5FD_MEM_SUPER;
    cls.fl_map[H5FD_MEM_BTREE] = H5FD_MEM_SUPER;
    cls.fl_map[H5FD_MEM_DRAW] = H5FD_MEM_DRAW;
    cls.fl_map[H5FD_MEM_GHEAP] = H5FD_MEM_DRAW;
    cls.fl_map[H5FD_MEM_LHEAP] = H5FD_MEM_SUPER;
    cls.fl_map[H5FD_MEM_OHDR] = H5FD_MEM_SUPER;
    driver_id_ = H5FDregister(&cls);
  }

  ~ScopedMemDrvVfd() {
    if (driver_id_ >= 0) {
      H5FDunregister(driver_id_);
    }
    g_mem_drv_state = prev_;
  }

  ScopedMemDrvVfd(const ScopedMemDrvVfd&) = delete;
  ScopedMemDrvVfd& operator=(const ScopedMemDrvVfd&) = delete;

  hid_t driver_id() const { return driver_id_; }
  MemDrvState& state() { return state_; }

 private:
  MemDrvState state_;
  MemDrvState* prev_ = nullptr;
  hid_t driver_id_ = H5I_INVALID_HID;
};

TEST(H5OdrvinfoTest, DescriptorMetadataAndRoundTripEncodeDecodeCopySizeReset) {
  ScopedFile file = CreateInMemoryFile("drvinfo_roundtrip.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  // Verify H5O_MSG_DRVINFO descriptor registration.
  const H5O_msg_class_t& cls = H5O_MSG_DRVINFO[0];
  EXPECT_EQ(cls.id, static_cast<unsigned>(H5O_DRVINFO_ID));
  EXPECT_STREQ(cls.name, "driver info");
  EXPECT_EQ(cls.native_size, sizeof(H5O_drvinfo_t));
  EXPECT_EQ(cls.share_flags, 0U);
  EXPECT_EQ(H5O_msg_class_g[H5O_DRVINFO_ID], H5O_MSG_DRVINFO);

  // Prepare native driver info message with an exact 8-char name (no internal
  // NUL) and multi-byte length payload to test encode/decode/size/copy/reset.
  std::vector<uint8_t> payload(300);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>((i * 7U + 3U) & 0xFFU);
  }
  H5O_drvinfo_t src = {};
  std::memcpy(src.name, "DRV8CHAR", 8);
  src.name[8] = '\0';
  src.len = payload.size();
  src.buf = payload.data();

  const size_t raw_size =
      H5O_msg_raw_size(f, H5O_DRVINFO_ID, /*disable_shared=*/false, &src);
  EXPECT_EQ(raw_size, 1U + 8U + 2U + payload.size());

  std::vector<uint8_t> encoded(raw_size, 0xAAU);
  ASSERT_GE(H5O_msg_encode(f, H5O_DRVINFO_ID, /*disable_shared=*/false,
                           encoded.data(), &src),
            0);
  EXPECT_EQ(encoded[0], 0U);  // H5O_DRVINFO_VERSION == 0
  EXPECT_EQ(std::memcmp(encoded.data() + 1, "DRV8CHAR", 8), 0);
  EXPECT_EQ(encoded[9], static_cast<uint8_t>(payload.size() & 0xFFU));
  EXPECT_EQ(encoded[10], static_cast<uint8_t>((payload.size() >> 8) & 0xFFU));
  EXPECT_EQ(std::memcmp(encoded.data() + 11, payload.data(), payload.size()),
            0);

  auto* decoded = static_cast<H5O_drvinfo_t*>(H5O_msg_decode(
      f, /*open_oh=*/nullptr, H5O_DRVINFO_ID, encoded.size(), encoded.data()));
  ASSERT_NE(decoded, nullptr);
  absl::Cleanup free_decoded = [&] { H5O_msg_free(H5O_DRVINFO_ID, decoded); };
  EXPECT_STREQ(decoded->name, "DRV8CHAR");
  EXPECT_EQ(decoded->len, payload.size());
  ASSERT_NE(decoded->buf, nullptr);
  EXPECT_EQ(std::memcmp(decoded->buf, payload.data(), payload.size()), 0);

  // Copy with dest == nullptr (allocates new H5O_drvinfo_t + buffer).
  auto* copied_alloc =
      static_cast<H5O_drvinfo_t*>(H5O_msg_copy(H5O_DRVINFO_ID, &src, nullptr));
  ASSERT_NE(copied_alloc, nullptr);
  absl::Cleanup free_copied_alloc = [&] {
    H5O_msg_free(H5O_DRVINFO_ID, copied_alloc);
  };
  EXPECT_STREQ(copied_alloc->name, "DRV8CHAR");
  EXPECT_EQ(copied_alloc->len, payload.size());
  ASSERT_NE(copied_alloc->buf, nullptr);
  EXPECT_NE(copied_alloc->buf, src.buf);
  EXPECT_EQ(std::memcmp(copied_alloc->buf, payload.data(), payload.size()), 0);

  // Copy into caller-provided struct (dest != nullptr), then reset via
  // H5O_msg_reset.
  H5O_drvinfo_t copied_in_place = {};
  void* ret_ptr = H5O_msg_copy(H5O_DRVINFO_ID, &src, &copied_in_place);
  EXPECT_EQ(ret_ptr, &copied_in_place);
  ASSERT_NE(copied_in_place.buf, nullptr);
  EXPECT_EQ(copied_in_place.len, payload.size());
  EXPECT_STREQ(copied_in_place.name, "DRV8CHAR");
  EXPECT_GE(H5O_msg_reset(H5O_DRVINFO_ID, &copied_in_place), 0);
  EXPECT_EQ(copied_in_place.buf, nullptr);

  // Also test H5O__drvinfo_debug output via H5O_debug_id.
  char* mem_buf = nullptr;
  size_t mem_len = 0;
  FILE* stream = open_memstream(&mem_buf, &mem_len);
  ASSERT_NE(stream, nullptr);
  absl::Cleanup free_mem_buf = [&] { std::free(mem_buf); };
  EXPECT_GE(H5O_debug_id(H5O_DRVINFO_ID, f, decoded, stream, 2, 20), 0);
  std::fclose(stream);
  std::string dbg_text(mem_buf != nullptr ? mem_buf : "", mem_len);
  EXPECT_THAT(dbg_text, HasSubstr("Driver name:"));
  EXPECT_THAT(dbg_text, HasSubstr("DRV8CHAR"));
  EXPECT_THAT(dbg_text, HasSubstr("Buffer size:"));
  EXPECT_THAT(dbg_text, HasSubstr("300"));
}

TEST(H5OdrvinfoTest, DecodeRejectsTruncatedAndMalformedBuffersCleanly) {
  ScopedFile file = CreateInMemoryFile("drvinfo_decode_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedErrorSilence silence;

  // 1. p_size == 0 overflow on version byte. Pass &buf[1] so `p + p_size - 1`
  //    points to &buf[0] inside the same array (well-defined pointer arithmetic).
  {
    const std::array<uint8_t, 2> zero_guard = {0x00U, 0x00U};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_DRVINFO_ID, 0, &zero_guard[1]),
              nullptr);
  }

  // 2. Unsupported version byte (!= H5O_DRVINFO_VERSION).
  {
    const std::array<uint8_t, 12> bad_vers = {
        0x01U, 'D', 'R', 'V', 'I', 'N', 'F', 'O', ' ', 0x01U, 0x00U, 0x42U};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_DRVINFO_ID, bad_vers.size(),
                             bad_vers.data()),
              nullptr);
  }

  // 3. Overflow while reading 8-byte driver name.
  {
    const std::array<uint8_t, 4> short_name = {0x00U, 'A', 'B', 'C'};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_DRVINFO_ID, short_name.size(),
                             short_name.data()),
              nullptr);
  }

  // 4. Overflow while reading 2-byte buffer length.
  {
    const std::array<uint8_t, 10> short_len = {
        0x00U, 'D', 'R', 'V', 'I', 'N', 'F', 'O', ' ', 0x01U};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_DRVINFO_ID, short_len.size(),
                             short_len.data()),
              nullptr);
  }

  // 5. Zero driver info buffer length (mesg->len == 0) rejected and mesg freed.
  {
    const std::array<uint8_t, 11> zero_len = {
        0x00U, 'D', 'R', 'V', 'I', 'N', 'F', 'O', ' ', 0x00U, 0x00U};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_DRVINFO_ID, zero_len.size(),
                             zero_len.data()),
              nullptr);
  }

  // 6. Overflow while reading driver info payload bytes (allocates mesg->buf,
  //    then fails overflow check and cleans up both mesg->buf and mesg).
  {
    const std::array<uint8_t, 13> truncated_payload = {
        0x00U, 'D', 'R', 'V', 'I', 'N', 'F', 'O', ' ', 0x08U, 0x00U, 0xAAU,
        0xBBU};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_DRVINFO_ID,
                             truncated_payload.size(), truncated_payload.data()),
              nullptr);
  }
}

TEST(H5OdrvinfoTest, PersistsAndReloadsInSuperblockExtensionForCustomAndFamilyVfds) {
  // 1. In-memory custom VFD with sb_size/sb_encode/sb_decode on V18+ triggers
  //    H5O_DRVINFO_ID in the superblock extension; inspect via H5O_debug.
  {
    ScopedMemDrvVfd vfd;
    ASSERT_GE(vfd.driver_id(), 0);

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl.get(), vfd.driver_id(), nullptr), 0);
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

    ScopedFile file(
        H5Fcreate("memdrv_ext.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(f->shared, nullptr);
    ASSERT_NE(f->shared->sblock, nullptr);
    ASSERT_NE(f->shared->sblock->ext_addr, HADDR_UNDEF);

    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      char* buf = nullptr;
      size_t len = 0;
      FILE* stream = open_memstream(&buf, &len);
      ASSERT_NE(stream, nullptr);
      absl::Cleanup free_buf = [&] { std::free(buf); };
      EXPECT_GE(H5O_debug(f, f->shared->sblock->ext_addr, stream, 0, 24), 0);
      std::fclose(stream);
      std::string out(buf != nullptr ? buf : "", len);
      EXPECT_THAT(out, HasSubstr("driver info"));
      EXPECT_THAT(out, HasSubstr("MEMDRV01"));
    }

    file.reset();

    ScopedFile reopened(H5Fopen("memdrv_ext.h5", H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(reopened.valid());
    EXPECT_STREQ(vfd.state().decoded_name, "MEMDRV01");
    EXPECT_EQ(vfd.state().decoded_payload, vfd.state().drv_payload);
  }

  // 2. Family VFD end-to-end check ensures public driver info encoding/decoding
  //    through the superblock extension works across file reopen.
  {
    TempDir dir("family_drvinfo");
    std::string pattern = (dir.path() / "fam_%03d.h5").string();

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_family(fapl.get(), 4096, H5P_DEFAULT), 0);
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

    ScopedFile file(
        H5Fcreate(pattern.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(
        H5Gcreate2(file.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    grp.reset();
    file.reset();

    ScopedFile reopened(H5Fopen(pattern.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(reopened.valid());
    H5F_info2_t finfo = {};
    ASSERT_GE(H5Fget_info2(reopened.get(), &finfo), 0);
    EXPECT_GT(finfo.super.super_ext_size, 0U);
  }
}

}  // namespace
}  // namespace hdf5
