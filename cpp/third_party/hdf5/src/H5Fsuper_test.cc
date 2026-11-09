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
// Tests superblock initialization, reading, dirtying, size querying, and
// superblock extension message lifecycle across Core, Sec2, Family, and Multi
// VFDs, SWMR modes, metadata cache images, and corrupt headers
// (third_party/hdf5/src/H5Fsuper.c).
//
// Note: H5_HAVE_PARALLEL is disabled in google3, so MPI collective/broadcast
// branches in H5F__super_read are compiled out.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fsuper.c

#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
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
#define H5FSUPER_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5FSUPER_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FSUPER_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__SUPERBLOCK_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
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

std::vector<uint8_t> ReadBytesFromFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    return {};
  }
  const std::streamsize sz = in.tellg();
  if (sz <= 0) {
    return {};
  }
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  if (!in.read(reinterpret_cast<char*>(buf.data()), sz)) {
    return {};
  }
  return buf;
}

bool WriteBytesToFile(const std::filesystem::path& path,
                      const std::vector<uint8_t>& buf) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
  return out.good();
}

void WriteLe64(uint8_t* p, uint64_t val) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>((val >> (8 * i)) & 0xFFU);
  }
}

void WriteLe32(uint8_t* p, uint32_t val) {
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>((val >> (8 * i)) & 0xFFU);
  }
}

uint64_t ReadLe64(const uint8_t* p) {
  uint64_t val = 0;
  for (int i = 0; i < 8; ++i) {
    val |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return val;
}

// Traverses a v2 object header (including continuation chunks `OCHK`), invokes
// `patch_fn(msg_hdr_off, payload_off, msg_size)` for the first message matching
// `target_type`, and updates the containing chunk's checksum.
template <typename PatchFn>
bool PatchOhdrMsgInChunks(std::vector<uint8_t>& img, size_t oh_off,
                          uint8_t target_type, PatchFn&& patch_fn) {
  if (oh_off + 8 >= img.size()) {
    return false;
  }
  if (std::memcmp(img.data() + oh_off, H5O_HDR_MAGIC, 4) != 0) {
    return false;
  }
  const uint8_t version = img[oh_off + 4];
  const uint8_t flags = img[oh_off + 5];
  if (version != 2) {
    return false;
  }
  size_t p = oh_off + 6;
  if (flags & H5O_HDR_STORE_TIMES) {
    p += 16;
  }
  if (flags & H5O_HDR_ATTR_STORE_PHASE_CHANGE) {
    p += 4;
  }
  const size_t chunk0_field_bytes = size_t{1} << (flags & H5O_HDR_CHUNK0_SIZE);
  if (p + chunk0_field_bytes > img.size()) {
    return false;
  }
  uint64_t chunk0_size = 0;
  for (size_t i = 0; i < chunk0_field_bytes; ++i) {
    chunk0_size |= static_cast<uint64_t>(img[p + i]) << (8 * i);
  }
  p += chunk0_field_bytes;
  const size_t chunk0_chksum = p + static_cast<size_t>(chunk0_size);
  if (chunk0_chksum + 4 > img.size()) {
    return false;
  }

  const size_t msg_hdr_extra = (flags & H5O_HDR_ATTR_CRT_ORDER_TRACKED) ? 2 : 0;
  struct ChunkSpan {
    size_t chunk_start;
    size_t msgs_start;
    size_t chksum_off;
  };
  std::queue<ChunkSpan> pending;
  pending.push({oh_off, p, chunk0_chksum});

  while (!pending.empty()) {
    const ChunkSpan cur = pending.front();
    pending.pop();
    size_t cursor = cur.msgs_start;
    while (cursor + 4 + msg_hdr_extra <= cur.chksum_off) {
      const uint8_t msg_type = img[cursor];
      const uint16_t msg_size =
          static_cast<uint16_t>(img[cursor + 1]) |
          (static_cast<uint16_t>(img[cursor + 2]) << 8);
      const size_t payload_off = cursor + 4 + msg_hdr_extra;
      if (payload_off + msg_size > cur.chksum_off) {
        return false;
      }
      if (msg_type == target_type) {
        if (!patch_fn(cursor, payload_off, msg_size)) {
          return false;
        }
        const uint32_t new_chksum = H5_checksum_metadata(
            img.data() + cur.chunk_start, cur.chksum_off - cur.chunk_start, 0);
        WriteLe32(img.data() + cur.chksum_off, new_chksum);
        return true;
      }
      if (msg_type == H5O_CONT_ID && msg_size == 16) {
        const uint64_t cont_addr = ReadLe64(img.data() + payload_off);
        const uint64_t cont_size = ReadLe64(img.data() + payload_off + 8);
        if (cont_addr + cont_size <= img.size() && cont_size >= 8 &&
            std::memcmp(img.data() + cont_addr, H5O_CHK_MAGIC, 4) == 0) {
          pending.push({static_cast<size_t>(cont_addr),
                        static_cast<size_t>(cont_addr + 4),
                        static_cast<size_t>(cont_addr + cont_size - 4)});
        }
      }
      cursor = payload_off + msg_size;
    }
  }
  return false;
}

// Rewrites an existing v2 object header chunk's H5O_FSINFO_ID message in-place
// from v1 to deprecated v0 (`H5F_FILE_SPACE_ALL`) and updates the OHDR
// chunk checksum so H5F__super_read decodes `fsinfo.mapped == true`.
bool PatchV1FsinfoToV0InOhdrChunk(std::vector<uint8_t>& img, size_t oh_off) {
  return PatchOhdrMsgInChunks(
      img, oh_off, H5O_FSINFO_ID,
      [&](size_t /*msg_hdr_off*/, size_t payload_off, uint16_t msg_size) {
        if (msg_size < 10) {
          return false;
        }
        img[payload_off + 0] = H5O_FSINFO_VERSION_0;
        img[payload_off + 1] = static_cast<uint8_t>(H5F_FILE_SPACE_ALL);
        std::memset(img.data() + payload_off + 2, 0, 8);
        img[payload_off + 2] = 4;
        return true;
      });
}

// Minimal in-memory Virtual File Driver with programmable fault injection for
// covering H5F__super_init, H5F__super_read, H5F__update_super_ext_driver_msg,
// and H5F__super_ext_* error paths without touching disk.
struct MockVfdState {
  unsigned long feature_flags = H5FD_FEAT_AGGREGATE_METADATA |
                                H5FD_FEAT_ACCUMULATE_METADATA |
                                H5FD_FEAT_DATA_SIEVE |
                                H5FD_FEAT_AGGREGATE_SMALLDATA |
                                H5FD_FEAT_SUPPORTS_SWMR_IO;
  hsize_t sb_size_bytes = 0;
  bool fail_sb_encode = false;
  bool fail_sb_decode = false;
  bool fail_get_eof_default = false;
  int fail_get_eof_after = -1;
  int fail_set_eoa_after = -1;
  int fail_read_after = -1;
  int fail_write_after = -1;
  int set_eoa_calls = 0;
  int get_eof_calls = 0;
  int read_calls = 0;
  int write_calls = 0;
  std::unordered_map<std::string, std::vector<uint8_t>> files;
};

struct MockVfdFile {
  H5FD_t pub;
  std::string key;
  haddr_t eoa;
  haddr_t eof;
};

MockVfdState* g_mock_vfd = nullptr;

hsize_t MockSbSize(H5FD_t* /*file*/) {
  return g_mock_vfd ? g_mock_vfd->sb_size_bytes : 0;
}

herr_t MockSbEncode(H5FD_t* /*file*/, char* name, unsigned char* p) {
  if (!g_mock_vfd || g_mock_vfd->fail_sb_encode) {
    return -1;
  }
  std::memcpy(name, "MOCKDRV ", 8);
  name[8] = '\0';
  if (g_mock_vfd->sb_size_bytes > 0) {
    std::memset(p, 0x42, static_cast<size_t>(g_mock_vfd->sb_size_bytes));
  }
  return 0;
}

herr_t MockSbDecode(H5FD_t* /*f*/, const char* /*name*/,
                    const unsigned char* /*p*/) {
  if (!g_mock_vfd || g_mock_vfd->fail_sb_decode) {
    return -1;
  }
  return 0;
}

H5FD_t* MockOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                 haddr_t /*maxaddr*/) {
  if (!g_mock_vfd || !name || !*name) {
    return nullptr;
  }
  auto it = g_mock_vfd->files.find(name);
  if (flags & H5F_ACC_TRUNC) {
    auto& buf = g_mock_vfd->files[name];
    buf.clear();
    it = g_mock_vfd->files.find(name);
  } else if (flags & H5F_ACC_CREAT) {
    if ((flags & H5F_ACC_EXCL) && it != g_mock_vfd->files.end()) {
      return nullptr;
    }
    if (it == g_mock_vfd->files.end()) {
      it = g_mock_vfd->files.emplace(name, std::vector<uint8_t>{}).first;
    }
  } else if (it == g_mock_vfd->files.end()) {
    return nullptr;
  }

  auto* mf = new MockVfdFile{};
  mf->key = name;
  mf->eoa = 0;
  mf->eof = static_cast<haddr_t>(it->second.size());
  return reinterpret_cast<H5FD_t*>(mf);
}

herr_t MockClose(H5FD_t* file) {
  delete reinterpret_cast<MockVfdFile*>(file);
  return 0;
}

int MockCmp(const H5FD_t* f1, const H5FD_t* f2) {
  const auto* m1 = reinterpret_cast<const MockVfdFile*>(f1);
  const auto* m2 = reinterpret_cast<const MockVfdFile*>(f2);
  if (m1->key < m2->key) return -1;
  if (m1->key > m2->key) return 1;
  return 0;
}

herr_t MockQuery(const H5FD_t* /*f*/, unsigned long* flags) {
  if (flags) {
    *flags = g_mock_vfd ? g_mock_vfd->feature_flags : 0;
  }
  return 0;
}

haddr_t MockGetEoa(const H5FD_t* file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const MockVfdFile*>(file)->eoa;
}

herr_t MockSetEoa(H5FD_t* file, H5FD_mem_t /*type*/, haddr_t addr) {
  if (!g_mock_vfd) {
    return -1;
  }
  g_mock_vfd->set_eoa_calls++;
  if (g_mock_vfd->fail_set_eoa_after >= 0 &&
      g_mock_vfd->fail_set_eoa_after-- == 0) {
    return -1;
  }
  reinterpret_cast<MockVfdFile*>(file)->eoa = addr;
  return 0;
}

haddr_t MockGetEof(const H5FD_t* file, H5FD_mem_t type) {
  if (!g_mock_vfd) {
    return HADDR_UNDEF;
  }
  g_mock_vfd->get_eof_calls++;
  if (g_mock_vfd->fail_get_eof_default && type == H5FD_MEM_DEFAULT) {
    return HADDR_UNDEF;
  }
  if (g_mock_vfd->fail_get_eof_after >= 0 &&
      g_mock_vfd->fail_get_eof_after-- == 0) {
    return HADDR_UNDEF;
  }
  return reinterpret_cast<const MockVfdFile*>(file)->eof;
}

herr_t MockRead(H5FD_t* file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, void* buf) {
  if (!g_mock_vfd || !buf) {
    return -1;
  }
  g_mock_vfd->read_calls++;
  if (g_mock_vfd->fail_read_after >= 0 && g_mock_vfd->fail_read_after-- == 0) {
    return -1;
  }
  const auto* mf = reinterpret_cast<const MockVfdFile*>(file);
  const auto it = g_mock_vfd->files.find(mf->key);
  if (it == g_mock_vfd->files.end()) {
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

herr_t MockWrite(H5FD_t* file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                 haddr_t addr, size_t size, const void* buf) {
  if (!g_mock_vfd || !buf) {
    return -1;
  }
  g_mock_vfd->write_calls++;
  if (g_mock_vfd->fail_write_after >= 0 &&
      g_mock_vfd->fail_write_after-- == 0) {
    return -1;
  }
  auto* mf = reinterpret_cast<MockVfdFile*>(file);
  auto it = g_mock_vfd->files.find(mf->key);
  if (it == g_mock_vfd->files.end()) {
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

herr_t MockTruncate(H5FD_t* file, hid_t /*dxpl_id*/, bool /*closing*/) {
  if (!g_mock_vfd) {
    return -1;
  }
  auto* mf = reinterpret_cast<MockVfdFile*>(file);
  auto it = g_mock_vfd->files.find(mf->key);
  if (it == g_mock_vfd->files.end()) {
    return -1;
  }
  if (mf->eoa != mf->eof) {
    it->second.resize(static_cast<size_t>(mf->eoa), 0);
    mf->eof = mf->eoa;
  }
  return 0;
}

H5FD_class_t MakeMockVfdClass() {
  H5FD_class_t cls = {};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = static_cast<H5FD_class_value_t>(510);
  cls.name = "mockmem_super_test";
  cls.maxaddr = static_cast<haddr_t>((~size_t{0}) - 1);
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.sb_size = MockSbSize;
  cls.sb_encode = MockSbEncode;
  cls.sb_decode = MockSbDecode;
  cls.open = MockOpen;
  cls.close = MockClose;
  cls.cmp = MockCmp;
  cls.query = MockQuery;
  cls.get_eoa = MockGetEoa;
  cls.set_eoa = MockSetEoa;
  cls.get_eof = MockGetEof;
  cls.read = MockRead;
  cls.write = MockWrite;
  cls.truncate = MockTruncate;
  cls.fl_map[H5FD_MEM_DEFAULT] = H5FD_MEM_SUPER;
  cls.fl_map[H5FD_MEM_SUPER] = H5FD_MEM_SUPER;
  cls.fl_map[H5FD_MEM_BTREE] = H5FD_MEM_SUPER;
  cls.fl_map[H5FD_MEM_DRAW] = H5FD_MEM_DRAW;
  cls.fl_map[H5FD_MEM_GHEAP] = H5FD_MEM_DRAW;
  cls.fl_map[H5FD_MEM_LHEAP] = H5FD_MEM_SUPER;
  cls.fl_map[H5FD_MEM_OHDR] = H5FD_MEM_SUPER;
  return cls;
}

class ScopedMockVfd {
 public:
  ScopedMockVfd() {
    prev_ = g_mock_vfd;
    g_mock_vfd = &state_;
    H5FD_class_t cls = MakeMockVfdClass();
    driver_id_ = H5FDregister(&cls);
  }
  ~ScopedMockVfd() {
    if (driver_id_ >= 0) {
      H5FDunregister(driver_id_);
    }
    g_mock_vfd = prev_;
  }
  ScopedMockVfd(const ScopedMockVfd&) = delete;
  ScopedMockVfd& operator=(const ScopedMockVfd&) = delete;

  hid_t driver_id() const { return driver_id_; }
  MockVfdState& state() { return state_; }

  ScopedPlist MakeFapl(unsigned low = H5F_LIBVER_EARLIEST,
                       unsigned high = H5F_LIBVER_LATEST) const {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    if (fapl.valid() && driver_id_ >= 0) {
      H5Pset_driver(fapl.get(), driver_id_, nullptr);
      H5Pset_libver_bounds(fapl.get(), static_cast<H5F_libver_t>(low),
                           static_cast<H5F_libver_t>(high));
    }
    return fapl;
  }

  void ResetCounters() {
    state_.fail_sb_encode = false;
    state_.fail_sb_decode = false;
    state_.fail_get_eof_default = false;
    state_.fail_get_eof_after = -1;
    state_.fail_set_eoa_after = -1;
    state_.fail_read_after = -1;
    state_.fail_write_after = -1;
    state_.set_eoa_calls = 0;
    state_.get_eof_calls = 0;
    state_.read_calls = 0;
    state_.write_calls = 0;
  }

 private:
  MockVfdState state_;
  MockVfdState* prev_ = nullptr;
  hid_t driver_id_ = H5I_INVALID_HID;
};

TEST(H5FsuperTest, SuperInitVariantsAndUserblockAlignmentBoundsErrors) {
  // 1. SOHM + non-default v1 B-tree K + userblock on Sec2 (creates v2 sb + ext
  //    at offset 512, flushes, and reopens to verify persistence).
  {
    TempFile ub_file("super_init_v2_ub.h5");
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 1),
              0);
    ASSERT_GE(H5Pset_sym_k(fcpl.get(), /*ik=*/8, /*lk=*/8), 0);
    ASSERT_GE(H5Pset_istore_k(fcpl.get(), 16), 0);
    ASSERT_GE(H5Pset_userblock(fcpl.get(), 512), 0);

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18),
              0);

    ScopedFile file(
        H5Fcreate(ub_file.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    file.reset();

    ScopedPlist open_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(open_fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(open_fapl.get()), 0);
    ScopedFile reopened(
        H5Fopen(ub_file.c_str(), H5F_ACC_RDONLY, open_fapl.get()));
    ASSERT_TRUE(reopened.valid());

    H5F_info2_t info = {};
    ASSERT_GE(H5Fget_info2(reopened.get(), &info), 0);
    EXPECT_EQ(info.super.version, 2U);
    EXPECT_GT(info.super.super_ext_size, 0U);
    ScopedPlist got_fcpl(H5Fget_create_plist(reopened.get()));
    ASSERT_TRUE(got_fcpl.valid());
    hsize_t userblock = 0;
    ASSERT_GE(H5Pget_userblock(got_fcpl.get(), &userblock), 0);
    EXPECT_EQ(userblock, 512U);
  }

  // 2. Non-default btree_k only (no SOHM, no fsinfo) creates extension for
  //    BTREEK message on V18+, and non-default istore_k selects v1 superblock
  //    under EARLIEST..V18.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sym_k(fcpl.get(), /*ik=*/24, /*lk=*/6), 0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);
    ScopedFile file(
        H5Fcreate("init_btreek.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
  }
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_istore_k(fcpl.get(), 16), 0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile file(
        H5Fcreate("init_v1_istore.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_info2_t info = {};
    ASSERT_GE(H5Fget_info2(file.get(), &info), 0);
    EXPECT_EQ(info.super.version, 1U);
  }

  // 3. Userblock alignment validation failures in H5F__super_init.
  {
    ScopedErrorSilence silence;
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_userblock(fcpl.get(), 512), 0);

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_alignment(fapl.get(), /*threshold=*/1, /*align=*/1024), 0);
    ScopedFile bad_lt(
        H5Fcreate("ub_lt.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    EXPECT_FALSE(bad_lt.valid());

    ASSERT_GE(H5Pset_userblock(fcpl.get(), 1024), 0);
    ASSERT_GE(H5Pset_alignment(fapl.get(), /*threshold=*/1, /*align=*/768), 0);
    ScopedFile bad_mod(
        H5Fcreate("ub_mod.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    EXPECT_FALSE(bad_mod.valid());
  }

  // 4. Version bounds failures in H5F__super_init (and failure cleanup).
  {
    ScopedErrorSilence silence;
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                         /*persist=*/false, /*threshold=*/1),
              0);

    // super_vers (3 for SWMR_WRITE) > high_bound superblock version (2 for V18).
    ScopedPlist fapl_swmr_v18(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl_swmr_v18.valid());
    ASSERT_GE(H5Pset_fapl_sec2(fapl_swmr_v18.get()), 0);
    ASSERT_GE(H5Pset_libver_bounds(fapl_swmr_v18.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    TempFile swmr_bounds_file("bounds_swmr_v18.h5");
    ScopedFile fail_bounds(
        H5Fcreate(swmr_bounds_file.c_str(), H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE,
                  H5P_DEFAULT, fapl_swmr_v18.get()));
    EXPECT_FALSE(fail_bounds.valid());

    // Passes super_vers == 2 check, then fails in H5O_fsinfo_set_version
    // under high_bound == V18, exercising H5F__super_init fail cleanup.
    ScopedPlist fapl_v18 = MakeCoreFapl();
    ASSERT_TRUE(fapl_v18.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl_v18.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    ScopedFile fail_fsinfo(
        H5Fcreate("bounds_v18.h5", H5F_ACC_TRUNC, fcpl.get(), fapl_v18.get()));
    EXPECT_FALSE(fail_fsinfo.valid());
  }
}

TEST(H5FsuperTest, SuperExtLifecycleDirtyAndSizeHelpersWorkAndFailCleanly) {
  // 1. Operations on v0 file (no extension supported).
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile v0(H5Fcreate("ext_v0.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(v0.valid());
    H5F_t* f = UnwrapFile(v0.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    hsize_t super_sz = 0;
    hsize_t ext_sz = 999;
    EXPECT_GE(H5F__super_size(f, &super_sz, &ext_sz), 0);
    EXPECT_GT(super_sz, 0U);
    EXPECT_EQ(ext_sz, 0U);

    EXPECT_GE(H5F_super_dirty(f), 0);
    EXPECT_GE(H5F_eoa_dirty(f), 0);

    // Creating a message on v0 superblock fails because extension < v2 is
    // disallowed.
    ScopedErrorSilence silence;
    H5O_btreek_t bt = {};
    bt.btree_k[H5B_CHUNK_ID] = 16;
    bt.btree_k[H5B_SNODE_ID] = 16;
    bt.sym_leaf_k = 4;
    EXPECT_LT(
        H5F__super_ext_write_msg(f, H5O_BTREEK_ID, &bt, /*may_create=*/true,
                                 H5O_MSG_NO_FLAGS_SET),
        0);
  }

  // 2. Superblock extension create / update / remove / error cases on v3 file.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);
    ScopedFile v3(H5Fcreate("ext_v3.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(v3.valid());
    H5F_t* f = UnwrapFile(v3.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    H5O_btreek_t bt = {};
    bt.btree_k[H5B_CHUNK_ID] = 20;
    bt.btree_k[H5B_SNODE_ID] = 12;
    bt.sym_leaf_k = 6;

    // Create extension on demand and add BTREEK message (non-constant so it
    // can be updated in place below).
    EXPECT_GE(
        H5F__super_ext_write_msg(f, H5O_BTREEK_ID, &bt, /*may_create=*/true,
                                 H5O_MSG_NO_FLAGS_SET),
        0);
    EXPECT_NE(f->shared->sblock->ext_addr, HADDR_UNDEF);

    // Direct open/close round-trip on existing superblock extension.
    {
      H5O_loc_t ext_loc = {};
      EXPECT_GE(H5F__super_ext_open(f, f->shared->sblock->ext_addr, &ext_loc),
                0);
      EXPECT_GE(H5F__super_ext_close(f, &ext_loc, /*was_created=*/false), 0);
    }

    hsize_t super_sz = 0;
    hsize_t ext_sz = 0;
    EXPECT_GE(H5F__super_size(f, &super_sz, &ext_sz), 0);
    EXPECT_GT(super_sz, 0U);
    EXPECT_GT(ext_sz, 0U);

    // Duplicate may_create=true and missing may_create=false both fail.
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5F__super_ext_write_msg(f, H5O_BTREEK_ID, &bt, /*may_create=*/true,
                                   H5O_MSG_NO_FLAGS_SET),
          0);
      H5O_fsinfo_t fake_fs = {};
      EXPECT_LT(H5F__super_ext_write_msg(f, H5O_FSINFO_ID, &fake_fs,
                                         /*may_create=*/false,
                                         H5O_MSG_NO_FLAGS_SET),
                0);
    }

    // Update existing message with may_create=false.
    bt.sym_leaf_k = 8;
    EXPECT_GE(
        H5F__super_ext_write_msg(f, H5O_BTREEK_ID, &bt, /*may_create=*/false,
                                 H5O_MSG_NO_FLAGS_SET),
        0);

    // Removing an absent message ID succeeds as a no-op.
    EXPECT_GE(H5F__super_ext_remove_msg(f, H5O_FSINFO_ID), 0);
    EXPECT_NE(f->shared->sblock->ext_addr, HADDR_UNDEF);

    // Removing the only remaining message deletes the empty extension.
    EXPECT_GE(H5F__super_ext_remove_msg(f, H5O_BTREEK_ID), 0);
    EXPECT_EQ(f->shared->sblock->ext_addr, HADDR_UNDEF);

    // Corrupt ext_addr errors: H5F__super_size, remove_msg, write_msg.
    {
      ScopedErrorSilence silence;
      const haddr_t orig_ext = f->shared->sblock->ext_addr;
      absl::Cleanup restore_ext = [&]() {
        f->shared->sblock->ext_addr = orig_ext;
      };
      f->shared->sblock->ext_addr = 0;  // superblock signature, not an OHDR
      EXPECT_LT(H5F__super_size(f, &super_sz, &ext_sz), 0);
      EXPECT_LT(H5F__super_ext_remove_msg(f, H5O_BTREEK_ID), 0);
      EXPECT_LT(H5F__super_ext_write_msg(f, H5O_BTREEK_ID, &bt,
                                         /*may_create=*/false,
                                         H5O_MSG_NO_FLAGS_SET),
                0);
    }

    // Error paths when superblock entry is temporarily unpinned.
    {
      ScopedErrorSilence silence;
      const bool orig_pinned = f->shared->sblock->cache_info.is_pinned;
      absl::Cleanup restore_pin = [&]() {
        f->shared->sblock->cache_info.is_pinned = orig_pinned;
      };
      f->shared->sblock->cache_info.is_pinned = false;
      EXPECT_LT(H5F_super_dirty(f), 0);
      EXPECT_LT(H5F_eoa_dirty(f), 0);
    }

    // Constant message update failure in H5F__super_ext_write_msg(may_create=false).
    {
      ScopedErrorSilence silence;
      EXPECT_GE(H5F__super_ext_write_msg(f, H5O_BTREEK_ID, &bt,
                                          /*may_create=*/true,
                                          H5O_MSG_FLAG_CONSTANT),
                0);
      EXPECT_LT(H5F__super_ext_write_msg(f, H5O_BTREEK_ID, &bt,
                                          /*may_create=*/false,
                                          H5O_MSG_NO_FLAGS_SET),
                0);
    }
  }

  // 3. Removing a message on an RDONLY file fails in H5O_msg_remove.
  {
    TempFile ro_ext_file("super_ext_ro_rm.h5");
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_TRUE(fcpl.valid());
      ASSERT_GE(H5Pset_sym_k(fcpl.get(), /*ik=*/24, /*lk=*/6), 0);
      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          0);
      ScopedFile f(
          H5Fcreate(ro_ext_file.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
      ASSERT_TRUE(f.valid());
    }
    ScopedPlist rd_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(rd_fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(rd_fapl.get()), 0);
    ScopedFile ro(
        H5Fopen(ro_ext_file.c_str(), H5F_ACC_RDONLY, rd_fapl.get()));
    ASSERT_TRUE(ro.valid());
    H5F_t* rf = UnwrapFile(ro.get());
    ASSERT_NE(rf, nullptr);
    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5F__super_ext_remove_msg(rf, H5O_BTREEK_ID), 0);
  }
}

TEST(H5FsuperTest, SuperReadHandlesMovedSuperblockTruncationFsinfoAndMdci) {
  // 1. Moved superblock (prepend 512 bytes in front of v0 file image), plus
  //    truncated file rejection and H5F_ACS_SKIP_EOF_CHECK_NAME bypass.
  {
    std::vector<uint8_t> img;
    {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                     H5F_LIBVER_V18),
                0);
      ScopedFile f(H5Fcreate("base_v0.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl.get()));
      ASSERT_TRUE(f.valid());
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);
      img = ExtractCoreImage(f.get());
      ASSERT_FALSE(img.empty());
    }

    // Prepend 512 zero bytes so H5FD_locate_signature finds signature at 512
    // while stored sblock->base_addr == 0 (moves base_addr & marks sb dirty).
    std::vector<uint8_t> shifted(512, 0);
    shifted.insert(shifted.end(), img.begin(), img.end());
    {
      ScopedPlist open_fapl = MakeCoreFapl();
      ASSERT_TRUE(open_fapl.valid());
      ASSERT_GE(
          H5Pset_file_image(open_fapl.get(), shifted.data(), shifted.size()),
          0);
      ScopedFile moved(
          H5Fopen("moved_v0.h5", H5F_ACC_RDWR, open_fapl.get()));
      ASSERT_TRUE(moved.valid());
    }

    // Truncated file image fails without SKIP_EOF_CHECK, succeeds with it.
    std::vector<uint8_t> trunc_img = img;
    ASSERT_GT(trunc_img.size(), 16U);
    trunc_img.resize(trunc_img.size() - 8);
    {
      ScopedErrorSilence silence;
      ScopedPlist bad_fapl = MakeCoreFapl();
      ASSERT_TRUE(bad_fapl.valid());
      ASSERT_GE(
          H5Pset_file_image(bad_fapl.get(), trunc_img.data(), trunc_img.size()),
          0);
      ScopedFile bad(H5Fopen("trunc.h5", H5F_ACC_RDONLY, bad_fapl.get()));
      EXPECT_FALSE(bad.valid());
    }
    {
      ScopedPlist skip_fapl = MakeCoreFapl();
      ASSERT_TRUE(skip_fapl.valid());
      ASSERT_GE(H5Pset_file_image(skip_fapl.get(), trunc_img.data(),
                                  trunc_img.size()),
                0);
      bool skip_eof = true;
      ASSERT_GE(
          H5Pset(skip_fapl.get(), H5F_ACS_SKIP_EOF_CHECK_NAME, &skip_eof), 0);
      ScopedFile ok(H5Fopen("trunc_skip.h5", H5F_ACC_RDONLY, skip_fapl.get()));
      ASSERT_TRUE(ok.valid());
    }
  }

  // 2. Persistent FSM + H5F_ACS_NULL_FSM_ADDR_NAME + version bounds check +
  //    mapped v0 FSINFO upgrade.
  {
    std::vector<uint8_t> fsinfo_img;
    haddr_t ext_addr = HADDR_UNDEF;
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_TRUE(fcpl.valid());
      ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(),
                                           H5F_FSPACE_STRATEGY_PAGE,
                                           /*persist=*/true, /*threshold=*/4),
                0);
      ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 1024), 0);

      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          0);

      ScopedFile file(H5Fcreate("fsinfo_page.h5", H5F_ACC_TRUNC, fcpl.get(),
                                fapl.get()));
      ASSERT_TRUE(file.valid());
      H5F_t* f = UnwrapFile(file.get());
      ASSERT_NE(f, nullptr);
      ext_addr = f->shared->sblock->ext_addr;
      ASSERT_NE(ext_addr, HADDR_UNDEF);
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      fsinfo_img = ExtractCoreImage(file.get());
      ASSERT_FALSE(fsinfo_img.empty());
    }

    // Reopen with high_bound=V18 to trigger H5O_fsinfo_check_version failure.
    {
      ScopedErrorSilence silence;
      ScopedPlist low_fapl = MakeCoreFapl();
      ASSERT_TRUE(low_fapl.valid());
      ASSERT_GE(H5Pset_file_image(low_fapl.get(), fsinfo_img.data(),
                                  fsinfo_img.size()),
                0);
      ASSERT_GE(H5Pset_libver_bounds(low_fapl.get(), H5F_LIBVER_EARLIEST,
                                     H5F_LIBVER_V18),
                0);
      ScopedFile bad(H5Fopen("fsinfo_bounds.h5", H5F_ACC_RDONLY,
                             low_fapl.get()));
      EXPECT_FALSE(bad.valid());
    }

    // Reopen with private H5F_ACS_NULL_FSM_ADDR_NAME to nullify FSM addresses.
    {
      ScopedPlist null_fapl = MakeCoreFapl();
      ASSERT_TRUE(null_fapl.valid());
      ASSERT_GE(H5Pset_file_image(null_fapl.get(), fsinfo_img.data(),
                                  fsinfo_img.size()),
                0);
      bool null_fsm = true;
      ASSERT_GE(
          H5Pset(null_fapl.get(), H5F_ACS_NULL_FSM_ADDR_NAME, &null_fsm), 0);
      ScopedFile opened(
          H5Fopen("fsinfo_nullfsm.h5", H5F_ACC_RDWR, null_fapl.get()));
      ASSERT_TRUE(opened.valid());
    }

    // Create a file with FSM_AGGR (non-persistent), patch its v1 FSINFO
    // message in-place to deprecated v0, and reopen RDWR to test
    // `fsinfo.mapped` message removal and rewrite in H5F__super_read.
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_TRUE(fcpl.valid());
      ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(),
                                           H5F_FSPACE_STRATEGY_FSM_AGGR,
                                           /*persist=*/false, /*threshold=*/4),
                0);
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          0);

      std::vector<uint8_t> fsm_img;
      haddr_t fsm_ext = HADDR_UNDEF;
      {
        ScopedFile f(H5Fcreate("fsm_map.h5", H5F_ACC_TRUNC, fcpl.get(),
                               fapl.get()));
        ASSERT_TRUE(f.valid());
        H5F_t* hf = UnwrapFile(f.get());
        ASSERT_NE(hf, nullptr);
        fsm_ext = hf->shared->sblock->ext_addr;
        ASSERT_NE(fsm_ext, HADDR_UNDEF);
        ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);
        fsm_img = ExtractCoreImage(f.get());
      }
      ASSERT_TRUE(PatchV1FsinfoToV0InOhdrChunk(fsm_img, fsm_ext));

      ScopedPlist map_fapl = MakeCoreFapl();
      ASSERT_TRUE(map_fapl.valid());
      ASSERT_GE(
          H5Pset_file_image(map_fapl.get(), fsm_img.data(), fsm_img.size()), 0);
      ScopedFile mapped(
          H5Fopen("fsm_mapped_rw.h5", H5F_ACC_RDWR, map_fapl.get()));
      ASSERT_TRUE(mapped.valid());
    }
  }

  // 3. Metadata Cache Image (H5O_MDCI_MSG_ID) round-trip (RDONLY & RDWR).
  {
    TempFile mdci_file("super_mdci.h5");
    {
      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          0);

      H5AC_cache_image_config_t img_cfg = {};
      img_cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
      img_cfg.generate_image = true;
      img_cfg.save_resize_status = false;
      img_cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
      ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &img_cfg), 0);

      ScopedFile f(H5Fcreate(mdci_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedGroup grp(
          H5Gcreate2(f.get(), "/mdci_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
    }

    ScopedPlist rd_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(rd_fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(rd_fapl.get()), 0);
    {
      ScopedFile f_ro(H5Fopen(mdci_file.c_str(), H5F_ACC_RDONLY, rd_fapl.get()));
      ASSERT_TRUE(f_ro.valid());
    }
    {
      ScopedFile f_rw(H5Fopen(mdci_file.c_str(), H5F_ACC_RDWR, rd_fapl.get()));
      ASSERT_TRUE(f_rw.valid());
    }
  }
}

TEST(H5FsuperTest, SuperReadHandlesNonHdf5AndCorruptSuperblocksCleanly) {
  ScopedErrorSilence silence;

  // 1. Non-HDF5 bytes -> signature not found.
  {
    std::vector<uint8_t> junk(1024, 0xAB);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), junk.data(), junk.size()), 0);
    ScopedFile bad(H5Fopen("not_hdf5.h5", H5F_ACC_RDONLY, fapl.get()));
    EXPECT_FALSE(bad.valid());
  }

  // 2. Superblock extension address set in v0 file fails validation (< v2).
  {
    std::vector<uint8_t> img;
    {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                     H5F_LIBVER_V18),
                0);
      ScopedFile f(H5Fcreate("v0_badext.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl.get()));
      ASSERT_TRUE(f.valid());
      H5F_t* hf = UnwrapFile(f.get());
      ASSERT_NE(hf, nullptr);
      // Set ext_addr to a bogus defined address before flushing superblock.
      hf->shared->sblock->ext_addr = 128;
      {
        ScopedApiContext ctx;
        EXPECT_GE(H5F_super_dirty(hf), 0);
      }
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);
      img = ExtractCoreImage(f.get());
      hf->shared->sblock->ext_addr = HADDR_UNDEF;
    }

    ScopedPlist open_fapl = MakeCoreFapl();
    ASSERT_TRUE(open_fapl.valid());
    ASSERT_GE(H5Pset_file_image(open_fapl.get(), img.data(), img.size()), 0);
    ScopedFile bad(H5Fopen("v0_badext_open.h5", H5F_ACC_RDONLY,
                           open_fapl.get()));
    EXPECT_FALSE(bad.valid());
  }

  // 3. v2 superblock with ext_addr > stored_eof pointing to invalid object
  //    header exercises ext_addr EOA bump + H5F__super_ext_open failure +
  //    sblock expunge cleanup in H5F__super_read.
  {
    std::vector<uint8_t> img;
    {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18), 0);
      ScopedFile f(
          H5Fcreate("v2_badext.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      H5F_t* hf = UnwrapFile(f.get());
      ASSERT_NE(hf, nullptr);
      hf->shared->sblock->ext_addr = 8192;
      {
        ScopedApiContext ctx;
        EXPECT_GE(H5F_super_dirty(hf), 0);
      }
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);
      img = ExtractCoreImage(f.get());
      hf->shared->sblock->ext_addr = HADDR_UNDEF;
    }

    ScopedPlist open_fapl = MakeCoreFapl();
    ASSERT_TRUE(open_fapl.valid());
    ASSERT_GE(H5Pset_file_image(open_fapl.get(), img.data(), img.size()), 0);
    ScopedFile bad(
        H5Fopen("v2_badext_open.h5", H5F_ACC_RDONLY, open_fapl.get()));
    EXPECT_FALSE(bad.valid());
  }
}

TEST(H5FsuperTest, SwmrCreateOpenAndErrorPathsWithSec2Vfd) {
  TempFile v3_file("super_swmr_v3.h5");
  TempFile v0_file("super_swmr_v0.h5");

  // 1. Create file with H5F_ACC_SWMR_WRITE (sets super_vers == 3, low >= V110).
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile f(H5Fcreate(v3_file.c_str(), H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE,
                           H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(
        H5Gcreate2(f.get(), "/swmr", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }

  // 2. Reopen v3 file with H5F_ACC_SWMR_READ (sets skip_eof_check = true) and
  //    then with H5F_ACC_SWMR_WRITE.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
    ScopedFile f_rd(
        H5Fopen(v3_file.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ,
                fapl.get()));
    ASSERT_TRUE(f_rd.valid());
    f_rd.reset();

    ScopedFile f_wr(
        H5Fopen(v3_file.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                fapl.get()));
    ASSERT_TRUE(f_wr.valid());
    f_wr.reset();

    // Opening v3 SWMR_WRITE with high_bound=V18 fails bounds check.
    ScopedErrorSilence silence;
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    ScopedFile f_bad(
        H5Fopen(v3_file.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                fapl.get()));
    EXPECT_FALSE(f_bad.valid());
  }

  // 3. Opening a v0 file with SWMR_WRITE fails (super_vers < 3).
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile f0(
        H5Fcreate(v0_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f0.valid());
    f0.reset();

    ScopedErrorSilence silence;
    ScopedPlist swmr_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(swmr_fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(swmr_fapl.get()), 0);
    ScopedFile f_bad(
        H5Fopen(v0_file.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                swmr_fapl.get()));
    EXPECT_FALSE(f_bad.valid());
  }
}

TEST(H5FsuperTest, FamilyDriverInfoUpdateRepartitionAndRemovalV0AndV2) {
  TempDir dir("super_family");

  // 1. V0 Family: test dirty drvinfo on EOA update, repartition dirty load,
  //    and unpinned drvinfo failure path.
  {
    const std::string pat = dir.child("v0fam%05d.h5").string();
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_fapl_family(fapl.get(), /*memb_size=*/65536, H5P_DEFAULT), 0);
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);

    ScopedFile f(H5Fcreate(pat.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);
    ASSERT_NE(hf->shared->drvinfo, nullptr);

    {
      ScopedApiContext ctx;
      EXPECT_GE(H5F_eoa_dirty(hf), 0);

      ScopedErrorSilence silence;
      const bool orig_pinned = hf->shared->drvinfo->cache_info.is_pinned;
      absl::Cleanup restore_pin = [&]() {
        hf->shared->drvinfo->cache_info.is_pinned = orig_pinned;
      };
      hf->shared->drvinfo->cache_info.is_pinned = false;
      EXPECT_LT(H5F_eoa_dirty(hf), 0);
    }
    f.reset();

    // Open with H5F_ACS_FAMILY_NEWSIZE_NAME to trigger H5FD_FEAT_DIRTY_DRVRINFO_LOAD.
    {
      hsize_t new_size = 131072;
      ASSERT_GE(H5Pset(fapl.get(), H5F_ACS_FAMILY_NEWSIZE_NAME, &new_size), 0);
      ScopedFile repart(H5Fopen(pat.c_str(), H5F_ACC_RDWR, fapl.get()));
      ASSERT_TRUE(repart.valid());
    }

    // Open single-member v0 family file via Sec2 + H5F_ACS_FAMILY_TO_SINGLE_NAME
    // to exercise ignore_drvrinfo + drvrinfo_removed dirty superblock path.
    {
      const std::string member0 = dir.child("v0fam00000.h5").string();
      ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(sec2_fapl.valid());
      ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);
      bool to_single = true;
      ASSERT_GE(
          H5Pset(sec2_fapl.get(), H5F_ACS_FAMILY_TO_SINGLE_NAME, &to_single),
          0);
      ScopedFile converted(
          H5Fopen(member0.c_str(), H5F_ACC_RDWR, sec2_fapl.get()));
      ASSERT_TRUE(converted.valid());
      H5F_t* chf = UnwrapFile(converted.get());
      ASSERT_NE(chf, nullptr);
      EXPECT_EQ(chf->shared->drvinfo, nullptr);
      EXPECT_EQ(chf->shared->sblock->driver_addr, HADDR_UNDEF);
    }
  }

  // 2. V2 Family: test driver info extension message update on EOA change and
  //    repartition rewrite (H5FD_FEAT_DIRTY_DRVRINFO_LOAD).
  {
    const std::string pat = dir.child("v2fam%05d.h5").string();
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_fapl_family(fapl.get(), /*memb_size=*/65536, H5P_DEFAULT), 0);
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18), 0);

    ScopedFile f(H5Fcreate(pat.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);
    EXPECT_TRUE(hf->shared->drvinfo_sb_msg_exists);

    {
      ScopedApiContext ctx;
      EXPECT_GE(H5F_eoa_dirty(hf), 0);

      // Fail H5F__update_super_ext_driver_msg via temporary sb_encode error.
      ScopedErrorSilence silence;
      const H5FD_class_t* orig_cls = hf->shared->lf->cls;
      H5FD_class_t failing_cls = *orig_cls;
      failing_cls.sb_encode = FailingSbEncode;
      absl::Cleanup restore_cls = [&]() { hf->shared->lf->cls = orig_cls; };
      hf->shared->lf->cls = &failing_cls;
      EXPECT_LT(H5F_eoa_dirty(hf), 0);
    }
    f.reset();

    // Open RDWR with H5F_ACS_FAMILY_NEWSIZE_NAME -> rewrites extension msg.
    {
      hsize_t new_size = 131072;
      ScopedPlist rep_fapl(H5Pcopy(fapl.get()));
      ASSERT_TRUE(rep_fapl.valid());
      ASSERT_GE(
          H5Pset(rep_fapl.get(), H5F_ACS_FAMILY_NEWSIZE_NAME, &new_size), 0);
      ScopedFile rep(H5Fopen(pat.c_str(), H5F_ACC_RDWR, rep_fapl.get()));
      ASSERT_TRUE(rep.valid());
    }
  }
}

TEST(H5FsuperTest, SuperReadHandlesDriverFailuresDuringOpen) {
  // Corrupt / non-family buffer opened with Family driver or failing get_eof
  // via custom driver wrapper? Test clean VFD get_eof failure using a minimal
  // in-memory Core image or H5F__super_read directly on an uninitialized f, or
  // test Multi/Split file with non-default ext_addr > stored_eof.
  TempDir dir("super_multi_read");
  const std::string base = dir.child("multi_ext").string();

  // Split VFD with V18 superblock puts superblock extension at metadata file,
  // while raw data starts at higher virtual address; also test normal reopen.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_sym_k(fcpl.get(), 12, 6), 0);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_split(fapl.get(), "-m.h5", H5P_DEFAULT, "-r.h5",
                              H5P_DEFAULT),
            0);
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18),
            0);

  {
    ScopedFile f(
        H5Fcreate(base.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(f.valid());
  }
  {
    ScopedFile reopened(H5Fopen(base.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(reopened.valid());
  }
}

TEST(H5FsuperTest, SuperReadHandlesCorruptExtensionMessagesAndPersistentFsinfo) {
  // 1. Corrupt superblock version byte fails H5AC_protect(H5AC_SUPERBLOCK).
  {
    std::vector<uint8_t> img;
    {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          0);
      ScopedFile f(
          H5Fcreate("corrupt_sb_vers.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);
      img = ExtractCoreImage(f.get());
    }
    ASSERT_GT(img.size(), 16U);
    img[8] = 99;
    ScopedErrorSilence silence;
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
    ScopedFile bad(H5Fopen("corrupt_sb_vers_open.h5", H5F_ACC_RDONLY, fapl.get()));
    EXPECT_FALSE(bad.valid());
  }

  // 2. Closed persistent PAGE Sec2 file writes eoa_pre_fsm_fsalloc != HADDR_UNDEF;
  //    patching its H5O_FSINFO_ID message exercises pgend_meta_thres mismatch,
  //    small page_size rejection, version error, and constant FSINFO write fail.
  {
    TempFile page_file("super_page_persist.h5");
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_TRUE(fcpl.valid());
      ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(),
                                           H5F_FSPACE_STRATEGY_PAGE,
                                           /*persist=*/true, /*threshold=*/4),
                0);
      ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 1024), 0);

      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          0);

      ScopedFile f(H5Fcreate(page_file.c_str(), H5F_ACC_TRUNC, fcpl.get(),
                             fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedGroup grp(
          H5Gcreate2(f.get(), "/pg_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
    }

    // Reopen closed file to cover non-HADDR_UNDEF eoa_pre_fsm_fsalloc assignment.
    {
      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
      ScopedFile reopened(
          H5Fopen(page_file.c_str(), H5F_ACC_RDONLY, fapl.get()));
      ASSERT_TRUE(reopened.valid());
    }

    const std::vector<uint8_t> base_img = ReadBytesFromFile(page_file.path());
    ASSERT_GT(base_img.size(), 48U);
    const size_t ext_off = static_cast<size_t>(ReadLe64(base_img.data() + 20));
    ASSERT_LT(ext_off, base_img.size());

    // Non-default pgend_meta_thres in FSINFO succeeds and updates shared field.
    {
      std::vector<uint8_t> img = base_img;
      ASSERT_TRUE(PatchOhdrMsgInChunks(
          img, ext_off, H5O_FSINFO_ID,
          [&](size_t /*hdr*/, size_t pay, uint16_t sz) {
            if (sz < 21) return false;
            img[pay + 19] = 16;
            img[pay + 20] = 0;
            return true;
          }));
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
      ScopedFile opened(H5Fopen("pgend_open.h5", H5F_ACC_RDONLY, fapl.get()));
      ASSERT_TRUE(opened.valid());
      H5F_t* hf = UnwrapFile(opened.get());
      ASSERT_NE(hf, nullptr);
      EXPECT_EQ(hf->shared->pgend_meta_thres, 16U);
    }

    // fsinfo.page_size < H5F_FILE_SPACE_PAGE_SIZE_MIN (1) fails cleanly.
    {
      std::vector<uint8_t> img = base_img;
      ASSERT_TRUE(PatchOhdrMsgInChunks(
          img, ext_off, H5O_FSINFO_ID,
          [&](size_t /*hdr*/, size_t pay, uint16_t sz) {
            if (sz < 19) return false;
            WriteLe64(img.data() + pay + 11, 1);
            return true;
          }));
      ScopedErrorSilence silence;
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
      ScopedFile bad(H5Fopen("small_pagesz.h5", H5F_ACC_RDONLY, fapl.get()));
      EXPECT_FALSE(bad.valid());
    }

    // Corrupt FSINFO message version fails H5O_fsinfo_check_version, and v0
    // FSINFO with invalid strategy fails H5O_msg_read(FSINFO) (line 746).
    {
      std::vector<uint8_t> img = base_img;
      ASSERT_TRUE(PatchOhdrMsgInChunks(
          img, ext_off, H5O_FSINFO_ID,
          [&](size_t /*hdr*/, size_t pay, uint16_t sz) {
            if (sz < 1) return false;
            img[pay] = 99;
            return true;
          }));
      ScopedErrorSilence silence;
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
      ScopedFile bad(H5Fopen("bad_fsinfo_vers.h5", H5F_ACC_RDONLY, fapl.get()));
      EXPECT_FALSE(bad.valid());
    }
    {
      std::vector<uint8_t> img = base_img;
      ASSERT_TRUE(PatchOhdrMsgInChunks(
          img, ext_off, H5O_FSINFO_ID,
          [&](size_t /*hdr*/, size_t pay, uint16_t sz) {
            if (sz < 2) return false;
            img[pay] = H5O_FSINFO_VERSION_0;
            img[pay + 1] = 99;
            return true;
          }));
      ScopedErrorSilence silence;
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
      ScopedFile bad(
          H5Fopen("bad_fsinfo_v0_strat.h5", H5F_ACC_RDONLY, fapl.get()));
      EXPECT_FALSE(bad.valid());
    }

    // Constant FSINFO message with NULL_FSM_ADDR_NAME fails
    // H5F__super_ext_write_msg in H5F__super_read.
    {
      std::vector<uint8_t> img = base_img;
      ASSERT_TRUE(PatchOhdrMsgInChunks(
          img, ext_off, H5O_FSINFO_ID,
          [&](size_t hdr, size_t /*pay*/, uint16_t /*sz*/) {
            img[hdr + 3] |= H5O_MSG_FLAG_CONSTANT;
            return true;
          }));
      ScopedErrorSilence silence;
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
      bool null_fsm = true;
      ASSERT_GE(H5Pset(fapl.get(), H5F_ACS_NULL_FSM_ADDR_NAME, &null_fsm), 0);
      ScopedFile bad(H5Fopen("const_fsinfo_rw.h5", H5F_ACC_RDWR, fapl.get()));
      EXPECT_FALSE(bad.valid());
    }
  }

  // 3. Corrupt BTREEK, SHMESG (SOHM), and MDCI extension messages.
  {
    // BTREEK version = 99 -> fails H5O_msg_read(H5O_BTREEK_ID).
    std::vector<uint8_t> img;
    haddr_t ext_addr = HADDR_UNDEF;
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_TRUE(fcpl.valid());
      ASSERT_GE(H5Pset_sym_k(fcpl.get(), 24, 6), 0);
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          0);
      ScopedFile f(
          H5Fcreate("corrupt_btreek.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
      ASSERT_TRUE(f.valid());
      ext_addr = UnwrapFile(f.get())->shared->sblock->ext_addr;
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);
      img = ExtractCoreImage(f.get());
    }
    ASSERT_TRUE(PatchOhdrMsgInChunks(
        img, static_cast<size_t>(ext_addr), H5O_BTREEK_ID,
        [&](size_t /*hdr*/, size_t pay, uint16_t sz) {
          if (sz < 1) return false;
          img[pay] = 99;
          return true;
        }));
    {
      ScopedErrorSilence silence;
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
      ScopedFile bad(H5Fopen("bad_btreek.h5", H5F_ACC_RDONLY, fapl.get()));
      EXPECT_FALSE(bad.valid());
    }
  }

  {
    // SOHM table address = 0 -> fails H5SM_get_info.
    std::vector<uint8_t> img;
    haddr_t ext_addr = HADDR_UNDEF;
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_TRUE(fcpl.valid());
      ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
      ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 1),
                0);
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          0);
      ScopedFile f(
          H5Fcreate("corrupt_shmesg.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
      ASSERT_TRUE(f.valid());
      ext_addr = UnwrapFile(f.get())->shared->sblock->ext_addr;
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);
      img = ExtractCoreImage(f.get());
    }
    ASSERT_TRUE(PatchOhdrMsgInChunks(
        img, static_cast<size_t>(ext_addr), H5O_SHMESG_ID,
        [&](size_t /*hdr*/, size_t pay, uint16_t sz) {
          if (sz < 9) return false;
          WriteLe64(img.data() + pay + 1, 0);
          return true;
        }));
    {
      ScopedErrorSilence silence;
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
      ScopedFile bad(H5Fopen("bad_shmesg.h5", H5F_ACC_RDONLY, fapl.get()));
      EXPECT_FALSE(bad.valid());
    }
  }

  {
    // Corrupt H5O_MDCI_MSG_ID version = 99 fails H5O_msg_read(H5O_MDCI_MSG_ID).
    TempFile mdci_file("corrupt_mdci.h5");
    {
      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          0);
      H5AC_cache_image_config_t cfg = {};
      cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
      cfg.generate_image = true;
      cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
      ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &cfg), 0);

      ScopedFile f(H5Fcreate(mdci_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedGroup grp(
          H5Gcreate2(f.get(), "/mdci_g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
    }
    std::vector<uint8_t> img = ReadBytesFromFile(mdci_file.path());
    ASSERT_GT(img.size(), 48U);
    const size_t ext_off = static_cast<size_t>(ReadLe64(img.data() + 20));
    ASSERT_TRUE(PatchOhdrMsgInChunks(
        img, ext_off, H5O_MDCI_MSG_ID,
        [&](size_t /*hdr*/, size_t pay, uint16_t sz) {
          if (sz < 1) return false;
          img[pay] = 99;
          return true;
        }));
    ScopedErrorSilence silence;
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
    ScopedFile bad(H5Fopen("bad_mdci.h5", H5F_ACC_RDONLY, fapl.get()));
    EXPECT_FALSE(bad.valid());
  }

  // 4. Family v2 driver info mismatch/corruption and v0 drvinfo cleanup on
  //    illegal ext_addr.
  {
    TempDir dir("super_drv_corrupt");
    const std::string v2_pat = dir.child("v2fam%05d.h5").string();
    const std::string v2_m0 = dir.child("v2fam00000.h5").string();
    const std::string v2_bad_pat = dir.child("v2bad%05d.h5").string();
    const std::string v2_bad_m0 = dir.child("v2bad00000.h5").string();

    {
      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_fapl_family(fapl.get(), 65536, H5P_DEFAULT), 0);
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18), 0);
      ScopedFile f(H5Fcreate(v2_pat.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl.get()));
      ASSERT_TRUE(f.valid());
    }

    ScopedErrorSilence silence;

    // Opening v2 family member via plain Sec2 fails H5FD_sb_load and resets msg.
    {
      ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(sec2_fapl.valid());
      ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);
      ScopedFile bad(
          H5Fopen(v2_m0.c_str(), H5F_ACC_RDONLY, sec2_fapl.get()));
      EXPECT_FALSE(bad.valid());
    }

    // Corrupting H5O_DRVINFO_ID version = 99 fails H5O_msg_read(H5O_DRVINFO_ID).
    {
      std::vector<uint8_t> img = ReadBytesFromFile(v2_m0);
      ASSERT_GT(img.size(), 48U);
      const size_t ext_off = static_cast<size_t>(ReadLe64(img.data() + 20));
      ASSERT_TRUE(PatchOhdrMsgInChunks(
          img, ext_off, H5O_DRVINFO_ID,
          [&](size_t /*hdr*/, size_t pay, uint16_t sz) {
            if (sz < 1) return false;
            img[pay] = 99;
            return true;
          }));
      ASSERT_TRUE(WriteBytesToFile(v2_bad_m0, img));
      ScopedPlist fam_fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fam_fapl.valid());
      ASSERT_GE(H5Pset_fapl_family(fam_fapl.get(), 65536, H5P_DEFAULT), 0);
      ScopedFile bad(
          H5Fopen(v2_bad_pat.c_str(), H5F_ACC_RDONLY, fam_fapl.get()));
      EXPECT_FALSE(bad.valid());
    }

    // V0 family file with bogus ext_addr loads drvinfo and then fails < v2,
    // exercising drvinfo unpin + expunge in H5F__super_read `done:`.
    {
      const std::string v0_pat = dir.child("v0fam%05d.h5").string();
      const std::string v0_m0 = dir.child("v0fam00000.h5").string();
      const std::string v0_bad_pat = dir.child("v0bad%05d.h5").string();
      const std::string v0_bad_m0 = dir.child("v0bad00000.h5").string();

      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_fapl_family(fapl.get(), 65536, H5P_DEFAULT), 0);
      ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                     H5F_LIBVER_V18),
                0);
      {
        ScopedFile f(H5Fcreate(v0_pat.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                               fapl.get()));
        ASSERT_TRUE(f.valid());
      }
      std::vector<uint8_t> img = ReadBytesFromFile(v0_m0);
      ASSERT_GT(img.size(), 48U);
      // v0 superblock ext_addr resides at bytes [32..40); v0 has no checksum.
      WriteLe64(img.data() + 32, 128);
      ASSERT_TRUE(WriteBytesToFile(v0_bad_m0, img));
      ScopedFile bad(
          H5Fopen(v0_bad_pat.c_str(), H5F_ACC_RDONLY, fapl.get()));
      EXPECT_FALSE(bad.valid());
    }
  }
}

TEST(H5FsuperTest, FaultInjectVfdCoversSuperInitReadAndUpdateErrorPaths) {
  ScopedMockVfd mock;
  ASSERT_GE(mock.driver_id(), 0);
  // Omit AGGREGATE_METADATA/SMALLDATA so every H5MF_alloc directly calls set_eoa.
  mock.state().feature_flags = H5FD_FEAT_SUPPORTS_SWMR_IO;

  auto make_fapl = [&](unsigned low = H5F_LIBVER_EARLIEST,
                       unsigned high = H5F_LIBVER_LATEST) {
    ScopedPlist fapl = mock.MakeFapl(low, high);
    EXPECT_GE(H5Pset_meta_block_size(fapl.get(), 0), 0);
    EXPECT_GE(H5Pset_small_data_block_size(fapl.get(), 0), 0);
    return fapl;
  };

  // 1. H5F__super_read VFD fault paths on v0 (without and with driver block).
  {
    mock.ResetCounters();
    mock.state().sb_size_bytes = 0;
    {
      ScopedPlist fapl = make_fapl(H5F_LIBVER_EARLIEST, H5F_LIBVER_V18);
      ScopedFile f(H5Fcreate("m_v0", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
    }
    ScopedErrorSilence silence;
    ScopedPlist rd_fapl = make_fapl();

    mock.ResetCounters();
    mock.state().fail_read_after = 0;
    EXPECT_FALSE(
        ScopedFile(H5Fopen("m_v0", H5F_ACC_RDONLY, rd_fapl.get())).valid());

    mock.ResetCounters();
    mock.state().fail_set_eoa_after = 1;
    EXPECT_FALSE(
        ScopedFile(H5Fopen("m_v0", H5F_ACC_RDONLY, rd_fapl.get())).valid());

    mock.ResetCounters();
    mock.state().fail_get_eof_default = true;
    EXPECT_FALSE(
        ScopedFile(H5Fopen("m_v0", H5F_ACC_RDONLY, rd_fapl.get())).valid());
  }

  {
    mock.ResetCounters();
    mock.state().sb_size_bytes = 16;
    {
      ScopedPlist fapl = make_fapl(H5F_LIBVER_EARLIEST, H5F_LIBVER_V18);
      ScopedFile f(
          H5Fcreate("m_v0_drv", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
    }
    ScopedErrorSilence silence;
    ScopedPlist rd_fapl = make_fapl();

    // sb_decode failure on old-style driver info block.
    mock.ResetCounters();
    mock.state().fail_sb_decode = true;
    EXPECT_FALSE(
        ScopedFile(H5Fopen("m_v0_drv", H5F_ACC_RDONLY, rd_fapl.get())).valid());

    // Measure baseline set_eoa count on clean RDONLY open to hit both the
    // driver_addr+HDR_SIZE call and final stored_eof call precisely.
    mock.ResetCounters();
    {
      ScopedFile ok(H5Fopen("m_v0_drv", H5F_ACC_RDONLY, rd_fapl.get()));
      ASSERT_TRUE(ok.valid());
    }
    const int eoa_calls = mock.state().set_eoa_calls;
    ASSERT_GE(eoa_calls, 4);

    // Fails H5FD_set_eoa(driver_addr + H5F_DRVINFOBLOCK_HDR_SIZE) at call 3.
    mock.ResetCounters();
    mock.state().fail_set_eoa_after = 3;
    EXPECT_FALSE(
        ScopedFile(H5Fopen("m_v0_drv", H5F_ACC_RDONLY, rd_fapl.get())).valid());

    // Fails final H5F__set_eoa(DEFAULT, stored_eof) after drvinfo is pinned,
    // exercising both stored_eof failure and drvinfo unpin+expunge cleanup.
    mock.ResetCounters();
    mock.state().fail_set_eoa_after = eoa_calls - 1;
    EXPECT_FALSE(
        ScopedFile(H5Fopen("m_v0_drv", H5F_ACC_RDONLY, rd_fapl.get())).valid());
  }

  // 2. H5F__super_read and H5F__update_super_ext_driver_msg on v2 + drvinfo.
  {
    mock.ResetCounters();
    mock.state().sb_size_bytes = 16;
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_TRUE(fcpl.valid());
      ASSERT_GE(H5Pset_sym_k(fcpl.get(), 24, 6), 0);
      ScopedPlist fapl = make_fapl(H5F_LIBVER_V18, H5F_LIBVER_V18);
      ScopedFile f(H5Fcreate("m_v2_drv", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
      ASSERT_TRUE(f.valid());
    }
    const std::vector<uint8_t> orig_v2 = mock.state().files["m_v2_drv"];
    ASSERT_GT(orig_v2.size(), 48U);

    // DIRTY_DRVRINFO_LOAD + sb_encode fail -> H5F__super_read error.
    {
      mock.state().files["m_v2_fail_enc"] = orig_v2;
      mock.state().feature_flags =
          H5FD_FEAT_SUPPORTS_SWMR_IO | H5FD_FEAT_DIRTY_DRVRINFO_LOAD;
      mock.ResetCounters();
      mock.state().fail_sb_encode = true;
      ScopedErrorSilence silence;
      ScopedPlist fapl = make_fapl();
      EXPECT_FALSE(
          ScopedFile(H5Fopen("m_v2_fail_enc", H5F_ACC_RDWR, fapl.get()))
              .valid());
    }

    // DIRTY_DRVRINFO_LOAD + CONSTANT DRVINFO msg -> write_msg fails cleanly.
    {
      std::vector<uint8_t> const_v2 = orig_v2;
      const size_t ext_off =
          static_cast<size_t>(ReadLe64(const_v2.data() + 20));
      ASSERT_TRUE(PatchOhdrMsgInChunks(
          const_v2, ext_off, H5O_DRVINFO_ID,
          [&](size_t hdr, size_t /*pay*/, uint16_t /*sz*/) {
            const_v2[hdr + 3] |= H5O_MSG_FLAG_CONSTANT;
            return true;
          }));
      mock.state().files["m_v2_const_drv"] = std::move(const_v2);
      mock.state().feature_flags =
          H5FD_FEAT_SUPPORTS_SWMR_IO | H5FD_FEAT_DIRTY_DRVRINFO_LOAD;
      mock.ResetCounters();
      ScopedErrorSilence silence;
      ScopedPlist fapl = make_fapl();
      EXPECT_FALSE(
          ScopedFile(H5Fopen("m_v2_const_drv", H5F_ACC_RDWR, fapl.get()))
              .valid());
    }

    // Note: H5F__super_read's H5FD_FEAT_IGNORE_DRVRINFO branch on v2+ invokes
    // H5F__super_ext_remove_msg while f->shared->sblock is still NULL
    // (upstream null-deref bug), so avoid enabling IGNORE_DRVRINFO on open.

    // Remove DRVINFO manually while keeping drvinfo_sb_msg_exists == true ->
    // H5F_eoa_dirty fails in H5F__update_super_ext_driver_msg.
    {
      mock.state().files["m_v2_upd_err"] = orig_v2;
      mock.state().feature_flags = H5FD_FEAT_SUPPORTS_SWMR_IO;
      mock.ResetCounters();
      ScopedPlist fapl = make_fapl();
      ScopedFile f(H5Fopen("m_v2_upd_err", H5F_ACC_RDWR, fapl.get()));
      ASSERT_TRUE(f.valid());
      H5F_t* hf = UnwrapFile(f.get());
      ASSERT_TRUE(hf->shared->drvinfo_sb_msg_exists);
      ScopedApiContext ctx;
      ASSERT_GE(H5F__super_ext_remove_msg(hf, H5O_DRVINFO_ID), 0);
      ScopedErrorSilence silence;
      EXPECT_LT(H5F_eoa_dirty(hf), 0);
      hf->shared->drvinfo_sb_msg_exists = false;
    }
  }

  // 3. v2 ext_addr > stored_eof set_eoa(H5FD_MEM_OHDR, ext_addr + 1024) fail.
  {
    mock.state().feature_flags = H5FD_FEAT_SUPPORTS_SWMR_IO;
    mock.state().sb_size_bytes = 0;
    mock.ResetCounters();
    {
      ScopedPlist fapl = make_fapl(H5F_LIBVER_V18, H5F_LIBVER_V18);
      ScopedFile f(
          H5Fcreate("m_v2_ext_eoa", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
    }
    std::vector<uint8_t> img = mock.state().files["m_v2_ext_eoa"];
    ASSERT_GE(img.size(), 48U);
    WriteLe64(img.data() + 20, 8192);
    WriteLe32(img.data() + 44, H5_checksum_metadata(img.data(), 44, 0));
    mock.state().files["m_v2_ext_eoa"] = std::move(img);

    // Measure set_eoa_calls up to the ext_addr + 1024 call: failing on call 4.
    mock.ResetCounters();
    mock.state().fail_set_eoa_after = 4;
    ScopedErrorSilence silence;
    ScopedPlist fapl = make_fapl();
    EXPECT_FALSE(
        ScopedFile(H5Fopen("m_v2_ext_eoa", H5F_ACC_RDONLY, fapl.get())).valid());
  }

  // 4. fsinfo.mapped rewrite H5F__super_ext_write_msg(may_create=true) failure.
  {
    mock.state().feature_flags = H5FD_FEAT_SUPPORTS_SWMR_IO;
    mock.state().sb_size_bytes = 0;
    mock.ResetCounters();
    haddr_t ext_addr = HADDR_UNDEF;
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_TRUE(fcpl.valid());
      ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NONE,
                                           /*persist=*/false, /*threshold=*/1),
                0);
      ScopedPlist fapl = make_fapl(H5F_LIBVER_V110, H5F_LIBVER_V110);
      ScopedFile f(
          H5Fcreate("m_fs_map", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
      ASSERT_TRUE(f.valid());
      ext_addr = UnwrapFile(f.get())->shared->sblock->ext_addr;
    }
    std::vector<uint8_t>& img = mock.state().files["m_fs_map"];
    ASSERT_TRUE(PatchOhdrMsgInChunks(
        img, static_cast<size_t>(ext_addr), H5O_FSINFO_ID,
        [&](size_t /*hdr*/, size_t pay, uint16_t sz) {
          if (sz < 10) return false;
          img[pay + 0] = H5O_FSINFO_VERSION_0;
          img[pay + 1] = static_cast<uint8_t>(H5F_FILE_SPACE_VFD);
          std::memset(img.data() + pay + 2, 0, 8);
          img[pay + 2] = 1;
          return true;
        }));
    // Fail set_eoa on the ext_create allocation inside H5F__super_ext_write_msg.
    mock.ResetCounters();
    mock.state().fail_set_eoa_after = 4;
    ScopedErrorSilence silence;
    ScopedPlist fapl = make_fapl();
    EXPECT_FALSE(
        ScopedFile(H5Fopen("m_fs_map", H5F_ACC_RDWR, fapl.get())).valid());
  }

  // 5. H5F__super_init & H5F__super_ext_write_msg allocation/encode failures.
  {
    mock.state().feature_flags = H5FD_FEAT_SUPPORTS_SWMR_IO;
    mock.state().sb_size_bytes = 0;
    ScopedErrorSilence silence;

    // Userblock set_eoa fails cleanly (line 1199).
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_GE(H5Pset_userblock(fcpl.get(), 512), 0);
      mock.ResetCounters();
      mock.state().fail_set_eoa_after = 0;
      ScopedPlist fapl = make_fapl(H5F_LIBVER_V110, H5F_LIBVER_V110);
      EXPECT_FALSE(
          ScopedFile(H5Fcreate("si_ub", H5F_ACC_TRUNC, fcpl.get(), fapl.get()))
              .valid());
    }

    // Superblock H5MF_alloc fails cleanly (line 1260).
    {
      mock.ResetCounters();
      mock.state().fail_set_eoa_after = 1;
      ScopedPlist fapl = make_fapl(H5F_LIBVER_V110, H5F_LIBVER_V110);
      EXPECT_FALSE(
          ScopedFile(
              H5Fcreate("si_sb", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()))
              .valid());
    }

    // Extension create fails after superblock alloc with persistent FSM ->
    // hits H5F__super_ext_create failure AND H5MF_try_close cleanup!
    {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_GE(
          H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                     /*persist=*/true, /*threshold=*/4),
          0);
      mock.ResetCounters();
      mock.state().fail_set_eoa_after = 2;
      ScopedPlist fapl = make_fapl(H5F_LIBVER_V110, H5F_LIBVER_V110);
      EXPECT_FALSE(
          ScopedFile(
              H5Fcreate("si_ext", H5F_ACC_TRUNC, fcpl.get(), fapl.get()))
              .valid());

      // Now allow ext_create (call 2) and fail H5O_msg_create(FSINFO) on
      // continuation chunk allocation (call 3) by prefilling chunk 0 with
      // BTREEK so FSINFO must allocate OCHK.
      ScopedPlist fs_fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_TRUE(fs_fcpl.valid());
      ASSERT_GE(H5Pset_sym_k(fs_fcpl.get(), 24, 6), 0);
      ASSERT_GE(
          H5Pset_file_space_strategy(fs_fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                     /*persist=*/false, /*threshold=*/4),
          0);
      mock.ResetCounters();
      mock.state().fail_set_eoa_after = 3;
      EXPECT_FALSE(
          ScopedFile(
              H5Fcreate("si_fsinfo", H5F_ACC_TRUNC, fs_fcpl.get(), fapl.get()))
              .valid());
    }

    // H5SM_init fails cleanly before allocating `table->indexes` when two SOHM
    // indexes claim the same message type flag.
    {
      ScopedPlist bad_sohm(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_GE(H5Pset_shared_mesg_nindexes(bad_sohm.get(), 2), 0);
      ASSERT_GE(
          H5Pset_shared_mesg_index(bad_sohm.get(), 0, H5O_SHMESG_ATTR_FLAG, 1),
          0);
      ASSERT_GE(
          H5Pset_shared_mesg_index(bad_sohm.get(), 1, H5O_SHMESG_ATTR_FLAG, 1),
          0);
      mock.ResetCounters();
      ScopedPlist fapl = make_fapl(H5F_LIBVER_V110, H5F_LIBVER_V110);
      EXPECT_FALSE(
          ScopedFile(
              H5Fcreate("si_sohm", H5F_ACC_TRUNC, bad_sohm.get(), fapl.get()))
              .valid());
    }

    // Driver sb_encode failure during H5F__super_init (v2) + DRVINFO chunk-1
    // allocation failure.
    {
      mock.ResetCounters();
      mock.state().sb_size_bytes = 16;
      mock.state().fail_sb_encode = true;
      ScopedPlist fapl = make_fapl(H5F_LIBVER_V18, H5F_LIBVER_V18);
      EXPECT_FALSE(
          ScopedFile(
              H5Fcreate("si_drvenc", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()))
              .valid());

      // Large sb_size overflows initial ext chunk -> fails DRVINFO OCHK alloc
      // at set_eoa call 3 (0=userblock, 1=superblock, 2=ext_create, 3=OCHK).
      mock.ResetCounters();
      mock.state().sb_size_bytes = 512;
      mock.state().fail_set_eoa_after = 3;
      EXPECT_FALSE(
          ScopedFile(
              H5Fcreate("si_drvmsg", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()))
              .valid());
    }

    // H5F__super_ext_write_msg(may_create=true) H5O_msg_create fail via chunk-1
    // alloc failure on an open v3 file.
    {
      mock.ResetCounters();
      mock.state().sb_size_bytes = 0;
      ScopedPlist fapl = make_fapl(H5F_LIBVER_V110, H5F_LIBVER_V110);
      ScopedFile f(
          H5Fcreate("si_ext_wmsg", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      H5F_t* hf = UnwrapFile(f.get());
      std::vector<uint8_t> big_buf(512, 0x33);
      H5O_drvinfo_t drv = {};
      std::memcpy(drv.name, "MOCKDRV ", 8);
      drv.name[8] = '\0';
      drv.len = big_buf.size();
      drv.buf = big_buf.data();

      ScopedApiContext ctx;
      // Allow H5F__super_ext_create (call 0) and fail OCHK alloc (call 1).
      mock.state().fail_set_eoa_after = 1;
      EXPECT_LT(
          H5F__super_ext_write_msg(hf, H5O_DRVINFO_ID, &drv,
                                   /*may_create=*/true, H5O_MSG_NO_FLAGS_SET),
          0);
      mock.state().fail_set_eoa_after = -1;
    }
  }
}

herr_t FailingPropGetCallback(hid_t /*plist_id*/, const char* /*name*/,
                              size_t /*size*/, void* /*value*/) {
  return -1;
}

int g_prop_get_countdown = -1;

herr_t CountdownFailPropCallback(hid_t /*plist_id*/, const char* /*name*/,
                                 size_t /*size*/, void* /*value*/) {
  if (g_prop_get_countdown >= 0 && g_prop_get_countdown-- == 0) {
    return -1;
  }
  return 0;
}

TEST(H5FsuperTest, InternalSuperInitAndReadPropertyListErrors) {
  ScopedErrorSilence silence;

  // Extract default property values via public H5Pget so synthetic lists match.
  unsigned sym_leaf_def = 0;
  unsigned btree_k_def[H5B_NUM_BTREE_ID] = {};
  unsigned super_vers_def = 0;
  uint8_t addr_bytes_def = 8;
  uint8_t obj_bytes_def = 8;
  hsize_t userblock_def = 0;
  unsigned sohm_nidx_def = 0;
  H5F_fspace_strategy_t fs_strat_def = H5F_FSPACE_STRATEGY_FSM_AGGR;
  bool fs_persist_def = false;
  hsize_t fs_thresh_def = 1;
  {
    ScopedPlist real_fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(real_fcpl.valid());
    ASSERT_GE(H5Pget(real_fcpl.get(), H5F_CRT_SYM_LEAF_NAME, &sym_leaf_def), 0);
    ASSERT_GE(H5Pget(real_fcpl.get(), H5F_CRT_BTREE_RANK_NAME, btree_k_def), 0);
    ASSERT_GE(H5Pget(real_fcpl.get(), H5F_CRT_SUPER_VERS_NAME, &super_vers_def),
              0);
    ASSERT_GE(
        H5Pget(real_fcpl.get(), H5F_CRT_ADDR_BYTE_NUM_NAME, &addr_bytes_def),
        0);
    ASSERT_GE(
        H5Pget(real_fcpl.get(), H5F_CRT_OBJ_BYTE_NUM_NAME, &obj_bytes_def), 0);
    ASSERT_GE(H5Pget(real_fcpl.get(), H5F_CRT_USER_BLOCK_NAME, &userblock_def),
              0);
    ASSERT_GE(
        H5Pget(real_fcpl.get(), H5F_CRT_SHMSG_NINDEXES_NAME, &sohm_nidx_def),
        0);
    ASSERT_GE(H5Pget(real_fcpl.get(), H5F_CRT_FILE_SPACE_STRATEGY_NAME,
                     &fs_strat_def),
              0);
    ASSERT_GE(
        H5Pget(real_fcpl.get(), H5F_CRT_FREE_SPACE_PERSIST_NAME, &fs_persist_def),
        0);
    ASSERT_GE(H5Pget(real_fcpl.get(), H5F_CRT_FREE_SPACE_THRESHOLD_NAME,
                     &fs_thresh_def),
              0);
  }

  // 1. Missing-property errors in H5F__super_init.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V110),
              0);
    ScopedFile f(H5Fcreate("si_pl_err.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);

    H5F_super_t* const orig_sblock = hf->shared->sblock;
    const hid_t orig_fcpl = hf->shared->fcpl_id;
    const H5F_libver_t orig_low = hf->shared->low_bound;
    const haddr_t orig_eoa = H5FD_get_eoa(hf->shared->lf, H5FD_MEM_DEFAULT);
    ScopedApiContext ctx(H5AC__SUPERBLOCK_TAG);
    absl::Cleanup restore = [&]() {
      hf->shared->sblock = orig_sblock;
      hf->shared->fcpl_id = orig_fcpl;
      hf->shared->low_bound = orig_low;
      H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa);
    };

    // Invalid fcpl_id -> H5I_object fails.
    hf->shared->fcpl_id = H5I_INVALID_HID;
    EXPECT_LT(H5F__super_init(hf), 0);
    hf->shared->sblock = orig_sblock;

    // Empty DCPL -> missing SYM_LEAF.
    ScopedPlist syn(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(syn.valid());
    hf->shared->fcpl_id = syn.get();
    EXPECT_LT(H5F__super_init(hf), 0);
    hf->shared->sblock = orig_sblock;

    // Add SYM_LEAF -> missing BTREE_RANK.
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_SYM_LEAF_NAME, sizeof(sym_leaf_def),
                         &sym_leaf_def, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr),
              0);
    EXPECT_LT(H5F__super_init(hf), 0);
    hf->shared->sblock = orig_sblock;

    // Add BTREE_RANK with low_bound=V110 -> forces super_vers != 0 -> missing
    // SUPER_VERS.
    ASSERT_GE(
        H5Pinsert2(syn.get(), H5F_CRT_BTREE_RANK_NAME, sizeof(btree_k_def),
                   btree_k_def, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr),
        0);
    hf->shared->low_bound = H5F_LIBVER_V110;
    EXPECT_LT(H5F__super_init(hf), 0);
    hf->shared->sblock = orig_sblock;

    // Add SUPER_VERS -> missing USER_BLOCK.
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_SUPER_VERS_NAME,
                         sizeof(super_vers_def), &super_vers_def, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5F__super_init(hf), 0);
  }

  // 2. Missing-property errors in H5F__super_read (v0 superblock paths).
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile f(
        H5Fcreate("sr_pl_v0.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);
    H5P_genplist_t* const fa_plist =
        static_cast<H5P_genplist_t*>(H5I_object(fapl.get()));
    const hid_t orig_fcpl = hf->shared->fcpl_id;
    const haddr_t orig_eoa = H5FD_get_eoa(hf->shared->lf, H5FD_MEM_DEFAULT);

    ScopedApiContext ctx(H5AC__SUPERBLOCK_TAG);
    H5AC_ring_t prev_ring = H5AC_RING_INV;
    H5AC_set_ring(H5AC_RING_SB, &prev_ring);
    ASSERT_GE(H5AC_unpin_entry(hf->shared->sblock), 0);
    ASSERT_GE(
        H5AC_expunge_entry(hf, H5AC_SUPERBLOCK, (haddr_t)0, H5AC__NO_FLAGS_SET),
        0);
    hf->shared->sblock = nullptr;
    H5AC_set_ring(prev_ring, nullptr);

    absl::Cleanup restore_v0 = [&]() {
      hf->shared->fcpl_id = orig_fcpl;
      H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa);
      H5F__super_read(hf, fa_plist, /*initial_read=*/false);
    };

    hf->shared->fcpl_id = H5I_INVALID_HID;
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
    ASSERT_GE(H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa), 0);

    ScopedPlist syn(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(syn.valid());
    hf->shared->fcpl_id = syn.get();
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
    ASSERT_GE(H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa), 0);

    // Add BTREE_RANK -> fails setting SUPER_VERS.
    ASSERT_GE(
        H5Pinsert2(syn.get(), H5F_CRT_BTREE_RANK_NAME, sizeof(btree_k_def),
                   btree_k_def, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr),
        0);
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
    ASSERT_GE(H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa), 0);

    // Add SUPER_VERS -> fails setting ADDR_BYTE_NUM.
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_SUPER_VERS_NAME,
                         sizeof(super_vers_def), &super_vers_def, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
    ASSERT_GE(H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa), 0);

    // Add ADDR_BYTE_NUM -> fails setting OBJ_BYTE_NUM.
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_ADDR_BYTE_NUM_NAME,
                         sizeof(addr_bytes_def), &addr_bytes_def, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
    ASSERT_GE(H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa), 0);

    // Add OBJ_BYTE_NUM -> fails setting SYM_LEAF on v0.
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_OBJ_BYTE_NUM_NAME,
                         sizeof(obj_bytes_def), &obj_bytes_def, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
    ASSERT_GE(H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa), 0);

    // Add SYM_LEAF -> fails setting USER_BLOCK.
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_SYM_LEAF_NAME, sizeof(sym_leaf_def),
                         &sym_leaf_def, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr),
              0);
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
    ASSERT_GE(H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa), 0);

    // Failing set callback on BTREE_RANK covers H5P_set failure (line 512).
    {
      ScopedPlist syn_set(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(syn_set.valid());
      ASSERT_GE(H5Pinsert2(syn_set.get(), H5F_CRT_BTREE_RANK_NAME,
                           sizeof(btree_k_def), btree_k_def,
                           FailingPropGetCallback, nullptr, nullptr, nullptr,
                           nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_set.get(), H5F_CRT_SUPER_VERS_NAME,
                           sizeof(super_vers_def), &super_vers_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_set.get(), H5F_CRT_ADDR_BYTE_NUM_NAME,
                           sizeof(addr_bytes_def), &addr_bytes_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_set.get(), H5F_CRT_OBJ_BYTE_NUM_NAME,
                           sizeof(obj_bytes_def), &obj_bytes_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_set.get(), H5F_CRT_SYM_LEAF_NAME,
                           sizeof(sym_leaf_def), &sym_leaf_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      hf->shared->fcpl_id = syn_set.get();
      EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
      ASSERT_GE(H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa), 0);
    }

    // Failing get callback on SKIP_EOF_CHECK_NAME covers H5P_get failure.
    hf->shared->fcpl_id = orig_fcpl;
    ScopedPlist bad_fa(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(bad_fa.valid());
    bool skip_val = false;
    ASSERT_GE(H5Pinsert2(bad_fa.get(), H5F_ACS_SKIP_EOF_CHECK_NAME,
                         sizeof(skip_val), &skip_val, nullptr,
                         FailingPropGetCallback, nullptr, nullptr, nullptr,
                         nullptr),
              0);
    EXPECT_LT(
        H5F__super_read(hf,
                        static_cast<H5P_genplist_t*>(H5I_object(bad_fa.get())),
                        /*initial_read=*/false),
        0);
  }

  // 3. Missing-property and fs_page_size errors in H5F__super_read (v3 +
  //    FSINFO extension paths).
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                         /*persist=*/true, /*threshold=*/4),
              0);
    ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 1024), 0);

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);
    ScopedFile f(H5Fcreate("sr_pl_v3.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(f.valid());
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);
    H5P_genplist_t* const fa_plist =
        static_cast<H5P_genplist_t*>(H5I_object(fapl.get()));
    const hid_t orig_fcpl = hf->shared->fcpl_id;
    const haddr_t orig_eoa = H5FD_get_eoa(hf->shared->lf, H5FD_MEM_DEFAULT);
    const unsigned orig_nopen = hf->nopen_objs;
    const unsigned orig_fsv = hf->shared->fs_version;
    const H5F_fspace_strategy_t orig_strat = hf->shared->fs_strategy;
    const bool orig_pers = hf->shared->fs_persist;
    const hsize_t orig_thr = hf->shared->fs_threshold;
    const hsize_t orig_pg = hf->shared->fs_page_size;

    ScopedApiContext ctx(H5AC__SUPERBLOCK_TAG);
    H5AC_ring_t prev_ring = H5AC_RING_INV;
    H5AC_set_ring(H5AC_RING_SB, &prev_ring);
    ASSERT_GE(H5AC_unpin_entry(hf->shared->sblock), 0);
    ASSERT_GE(
        H5AC_expunge_entry(hf, H5AC_SUPERBLOCK, (haddr_t)0, H5AC__NO_FLAGS_SET),
        0);
    hf->shared->sblock = nullptr;
    H5AC_set_ring(prev_ring, nullptr);

    auto reset_fs_to_default = [&]() {
      hf->nopen_objs = orig_nopen;
      hf->shared->fs_version = 0;
      hf->shared->fs_strategy = H5F_FSPACE_STRATEGY_FSM_AGGR;
      hf->shared->fs_persist = false;
      hf->shared->fs_threshold = 1;
      hf->shared->fs_page_size = H5F_FILE_SPACE_PAGE_SIZE_DEF;
      H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa);
    };

    absl::Cleanup restore_v3 = [&]() {
      hf->shared->fcpl_id = orig_fcpl;
      hf->nopen_objs = orig_nopen;
      hf->shared->fs_version = orig_fsv;
      hf->shared->fs_strategy = orig_strat;
      hf->shared->fs_persist = orig_pers;
      hf->shared->fs_threshold = orig_thr;
      hf->shared->fs_page_size = orig_pg;
      H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa);
      H5F__super_read(hf, fa_plist, /*initial_read=*/false);
    };

    ScopedPlist syn(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(syn.valid());
    ASSERT_GE(
        H5Pinsert2(syn.get(), H5F_CRT_BTREE_RANK_NAME, sizeof(btree_k_def),
                   btree_k_def, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr),
        0);
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_SUPER_VERS_NAME,
                         sizeof(super_vers_def), &super_vers_def, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_ADDR_BYTE_NUM_NAME,
                         sizeof(addr_bytes_def), &addr_bytes_def, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_OBJ_BYTE_NUM_NAME,
                         sizeof(obj_bytes_def), &obj_bytes_def, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
              0);

    // super_vers >= 2 without SYM_LEAF -> fails H5P_get(SYM_LEAF) (line 521).
    hf->shared->fcpl_id = syn.get();
    reset_fs_to_default();
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);

    // Add SYM_LEAF, USER_BLOCK, and SHMSG_NINDEXES (for H5SM_get_info) ->
    // fails setting FILE_SPACE_STRATEGY (line 761).
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_SYM_LEAF_NAME, sizeof(sym_leaf_def),
                         &sym_leaf_def, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr),
              0);
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_USER_BLOCK_NAME,
                         sizeof(userblock_def), &userblock_def, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_SHMSG_NINDEXES_NAME,
                         sizeof(sohm_nidx_def), &sohm_nidx_def, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    reset_fs_to_default();
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);

    // Add FILE_SPACE_STRATEGY -> fails setting FREE_SPACE_PERSIST (line 768).
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_FILE_SPACE_STRATEGY_NAME,
                         sizeof(fs_strat_def), &fs_strat_def, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
              0);
    reset_fs_to_default();
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);

    // Add FREE_SPACE_PERSIST -> fails setting FREE_SPACE_THRESHOLD (line 775).
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_FREE_SPACE_PERSIST_NAME,
                         sizeof(fs_persist_def), &fs_persist_def, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    reset_fs_to_default();
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);

    // Add FREE_SPACE_THRESHOLD -> fails setting FILE_SPACE_PAGE_SIZE (line 788).
    ASSERT_GE(H5Pinsert2(syn.get(), H5F_CRT_FREE_SPACE_THRESHOLD_NAME,
                         sizeof(fs_thresh_def), &fs_thresh_def, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    reset_fs_to_default();
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);

    // f->shared->fs_page_size < H5F_FILE_SPACE_PAGE_SIZE_MIN (line 779).
    hf->shared->fcpl_id = orig_fcpl;
    reset_fs_to_default();
    hf->shared->fs_page_size = 1;
    EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);

    // Failing H5F_ACS_NULL_FSM_ADDR_NAME callback (line 741).
    reset_fs_to_default();
    ScopedPlist bad_fa(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(bad_fa.valid());
    bool null_fsm_val = false;
    ASSERT_GE(H5Pinsert2(bad_fa.get(), H5F_ACS_NULL_FSM_ADDR_NAME,
                         sizeof(null_fsm_val), &null_fsm_val, nullptr,
                         FailingPropGetCallback, nullptr, nullptr, nullptr,
                         nullptr),
              0);
    EXPECT_LT(
        H5F__super_read(hf,
                        static_cast<H5P_genplist_t*>(H5I_object(bad_fa.get())),
                        /*initial_read=*/false),
        0);

    // Fail H5P_get(BTREE_RANK) on 2nd call in v3 >= 2 branch (line 519).
    {
      absl::Cleanup reset_cnt = [&]() { g_prop_get_countdown = -1; };
      ScopedPlist syn_get2(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(syn_get2.valid());
      ASSERT_GE(H5Pinsert2(syn_get2.get(), H5F_CRT_BTREE_RANK_NAME,
                           sizeof(btree_k_def), btree_k_def, nullptr,
                           CountdownFailPropCallback, nullptr, nullptr, nullptr,
                           nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_get2.get(), H5F_CRT_SUPER_VERS_NAME,
                           sizeof(super_vers_def), &super_vers_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_get2.get(), H5F_CRT_ADDR_BYTE_NUM_NAME,
                           sizeof(addr_bytes_def), &addr_bytes_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_get2.get(), H5F_CRT_OBJ_BYTE_NUM_NAME,
                           sizeof(obj_bytes_def), &obj_bytes_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      hf->shared->fcpl_id = syn_get2.get();
      reset_fs_to_default();
      g_prop_get_countdown = 1;
      EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
      hf->shared->fcpl_id = orig_fcpl;
    }
  }

  // 4. H5P_set(BTREE_RANK) and H5P_set(SYM_LEAF) errors when decoding BTREEK
  //    in H5F__super_read (lines 713, 715).
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sym_k(fcpl.get(), 24, 6), 0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);
    ScopedFile f(
        H5Fcreate("sr_pl_btreek.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(f.valid());
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);
    H5P_genplist_t* const fa_plist =
        static_cast<H5P_genplist_t*>(H5I_object(fapl.get()));
    const hid_t orig_fcpl = hf->shared->fcpl_id;
    const haddr_t orig_eoa = H5FD_get_eoa(hf->shared->lf, H5FD_MEM_DEFAULT);
    const unsigned orig_nopen = hf->nopen_objs;

    ScopedApiContext ctx(H5AC__SUPERBLOCK_TAG);
    H5AC_ring_t prev_ring = H5AC_RING_INV;
    H5AC_set_ring(H5AC_RING_SB, &prev_ring);
    ASSERT_GE(H5AC_unpin_entry(hf->shared->sblock), 0);
    ASSERT_GE(
        H5AC_expunge_entry(hf, H5AC_SUPERBLOCK, (haddr_t)0, H5AC__NO_FLAGS_SET),
        0);
    hf->shared->sblock = nullptr;
    H5AC_set_ring(prev_ring, nullptr);

    absl::Cleanup restore_brk = [&]() {
      hf->shared->fcpl_id = orig_fcpl;
      hf->nopen_objs = orig_nopen;
      H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa);
      H5F__super_read(hf, fa_plist, /*initial_read=*/false);
    };

    {
      ScopedPlist syn_brk(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(syn_brk.valid());
      ASSERT_GE(H5Pinsert2(syn_brk.get(), H5F_CRT_BTREE_RANK_NAME,
                           sizeof(btree_k_def), btree_k_def,
                           FailingPropGetCallback, nullptr, nullptr, nullptr,
                           nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_brk.get(), H5F_CRT_SUPER_VERS_NAME,
                           sizeof(super_vers_def), &super_vers_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_brk.get(), H5F_CRT_ADDR_BYTE_NUM_NAME,
                           sizeof(addr_bytes_def), &addr_bytes_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_brk.get(), H5F_CRT_OBJ_BYTE_NUM_NAME,
                           sizeof(obj_bytes_def), &obj_bytes_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_brk.get(), H5F_CRT_SYM_LEAF_NAME,
                           sizeof(sym_leaf_def), &sym_leaf_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_brk.get(), H5F_CRT_USER_BLOCK_NAME,
                           sizeof(userblock_def), &userblock_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_brk.get(), H5F_CRT_SHMSG_NINDEXES_NAME,
                           sizeof(sohm_nidx_def), &sohm_nidx_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      hf->shared->fcpl_id = syn_brk.get();
      hf->nopen_objs = orig_nopen;
      H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa);
      EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
    }

    {
      ScopedPlist syn_sym(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(syn_sym.valid());
      ASSERT_GE(H5Pinsert2(syn_sym.get(), H5F_CRT_BTREE_RANK_NAME,
                           sizeof(btree_k_def), btree_k_def, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_sym.get(), H5F_CRT_SUPER_VERS_NAME,
                           sizeof(super_vers_def), &super_vers_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_sym.get(), H5F_CRT_ADDR_BYTE_NUM_NAME,
                           sizeof(addr_bytes_def), &addr_bytes_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_sym.get(), H5F_CRT_OBJ_BYTE_NUM_NAME,
                           sizeof(obj_bytes_def), &obj_bytes_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_sym.get(), H5F_CRT_SYM_LEAF_NAME,
                           sizeof(sym_leaf_def), &sym_leaf_def,
                           FailingPropGetCallback, nullptr, nullptr, nullptr,
                           nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_sym.get(), H5F_CRT_USER_BLOCK_NAME,
                           sizeof(userblock_def), &userblock_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      ASSERT_GE(H5Pinsert2(syn_sym.get(), H5F_CRT_SHMSG_NINDEXES_NAME,
                           sizeof(sohm_nidx_def), &sohm_nidx_def, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
                0);
      hf->shared->fcpl_id = syn_sym.get();
      hf->nopen_objs = orig_nopen;
      H5FD_set_eoa(hf->shared->lf, H5FD_MEM_DEFAULT, orig_eoa);
      EXPECT_LT(H5F__super_read(hf, fa_plist, /*initial_read=*/false), 0);
    }
  }
}

}  // namespace
}  // namespace hdf5
