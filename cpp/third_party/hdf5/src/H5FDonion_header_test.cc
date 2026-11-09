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
// Tests the Onion Virtual File Driver (VFD) header encoding, decoding,
// checksum validation, and backing-file read/write/error handling in
// third_party/hdf5/src/H5FDonion_header.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDonion_header.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDONION_HEADER_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDonion_priv.h"
#include "third_party/hdf5/src/H5FDpkg.h"
}  // extern "C"

#ifdef H5FDONION_HEADER_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDONION_HEADER_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Move-only RAII wrapper for raw H5FD_t* handles opened via H5FDopen.
class ScopedFdFile {
 public:
  ScopedFdFile() = default;
  explicit ScopedFdFile(H5FD_t* file) : file_(file) {}
  ~ScopedFdFile() { reset(); }

  ScopedFdFile(const ScopedFdFile&) = delete;
  ScopedFdFile& operator=(const ScopedFdFile&) = delete;

  ScopedFdFile(ScopedFdFile&& other) noexcept
      : file_(std::exchange(other.file_, nullptr)) {}

  ScopedFdFile& operator=(ScopedFdFile&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.file_, nullptr));
    }
    return *this;
  }

  void reset(H5FD_t* next = nullptr) {
    if (file_ != nullptr) {
      H5FDclose(file_);
    }
    file_ = next;
  }

  H5FD_t* get() const { return file_; }
  bool valid() const { return file_ != nullptr; }

 private:
  H5FD_t* file_ = nullptr;
};

// Move-only RAII wrapper for custom VFD driver IDs registered with
// H5FDregister.
class ScopedVfdDriver {
 public:
  ScopedVfdDriver() = default;
  explicit ScopedVfdDriver(hid_t id) : id_(id) {}
  ~ScopedVfdDriver() { reset(); }

  ScopedVfdDriver(const ScopedVfdDriver&) = delete;
  ScopedVfdDriver& operator=(const ScopedVfdDriver&) = delete;

  ScopedVfdDriver(ScopedVfdDriver&& other) noexcept
      : id_(std::exchange(other.id_, H5I_INVALID_HID)) {}

  ScopedVfdDriver& operator=(ScopedVfdDriver&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.id_, H5I_INVALID_HID));
    }
    return *this;
  }

  void reset(hid_t next = H5I_INVALID_HID) {
    if (id_ != H5I_INVALID_HID) {
      H5FDunregister(id_);
    }
    id_ = next;
  }

  hid_t get() const { return id_; }
  bool valid() const { return id_ >= 0; }

 private:
  hid_t id_ = H5I_INVALID_HID;
};

// RAII wrapper for pushing an HDF5 API context required by internal H5FD_read
// and H5FD_write calls (which query H5CX for the DXPL and selection I/O mode).
class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5CX_set_dxpl(H5P_DATASET_XFER_DEFAULT);
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

// In-memory fault-injecting VFD for deterministic header ingest/write error
// testing without disk I/O.
struct MockHeaderVfdState {
  std::vector<uint8_t> data;
  haddr_t eoa = 0;
  bool fail_set_eoa = false;
  bool fail_read = false;
  bool fail_write = false;
};

struct MockHeaderVfdFile {
  H5FD_t pub{};
  MockHeaderVfdState* state = nullptr;
};

static_assert(std::is_standard_layout_v<MockHeaderVfdFile>);
static_assert(offsetof(MockHeaderVfdFile, pub) == 0);

MockHeaderVfdState* g_mock_state = nullptr;

class ScopedMockState {
 public:
  explicit ScopedMockState(MockHeaderVfdState* state)
      : prev_(std::exchange(g_mock_state, state)) {}
  ~ScopedMockState() { g_mock_state = prev_; }

  ScopedMockState(const ScopedMockState&) = delete;
  ScopedMockState& operator=(const ScopedMockState&) = delete;

 private:
  MockHeaderVfdState* prev_;
};

H5FD_t* MockOpen(const char* /*name*/, unsigned /*flags*/, hid_t /*fapl_id*/,
                 haddr_t /*maxaddr*/) {
  if (g_mock_state == nullptr) {
    return nullptr;
  }
  auto* file = new MockHeaderVfdFile();
  file->state = g_mock_state;
  return &file->pub;
}

herr_t MockClose(H5FD_t* _file) {
  delete reinterpret_cast<MockHeaderVfdFile*>(_file);
  return 0;
}

herr_t MockQuery(const H5FD_t* /*file*/,
                 unsigned long* flags) {  // NOLINT(runtime/int)
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

haddr_t MockGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const MockHeaderVfdFile*>(_file)->state->eoa;
}

herr_t MockSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<MockHeaderVfdFile*>(_file);
  if (file->state->fail_set_eoa) {
    return -1;
  }
  file->state->eoa = addr;
  return 0;
}

haddr_t MockGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return static_cast<haddr_t>(
      reinterpret_cast<const MockHeaderVfdFile*>(_file)->state->data.size());
}

herr_t MockRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, void* buf) {
  auto* file = reinterpret_cast<MockHeaderVfdFile*>(_file);
  if (file->state->fail_read) {
    return -1;
  }
  std::memset(buf, 0, size);
  if (addr < file->state->data.size()) {
    const size_t avail = file->state->data.size() - static_cast<size_t>(addr);
    std::memcpy(buf, file->state->data.data() + addr, std::min(size, avail));
  }
  return 0;
}

herr_t MockWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                 haddr_t addr, size_t size, const void* buf) {
  auto* file = reinterpret_cast<MockHeaderVfdFile*>(_file);
  if (file->state->fail_write) {
    return -1;
  }
  const size_t end = static_cast<size_t>(addr) + size;
  if (end > file->state->data.size()) {
    file->state->data.resize(end, 0);
  }
  if (size > 0) {
    std::memcpy(file->state->data.data() + addr, buf, size);
  }
  return 0;
}

H5FD_class_t MakeMockHeaderVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = 261;
  cls.name = "mock_onion_header_vfd";
  cls.maxaddr = HADDR_MAX;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = MockOpen;
  cls.close = MockClose;
  cls.query = MockQuery;
  cls.get_eoa = MockGetEoa;
  cls.set_eoa = MockSetEoa;
  cls.get_eof = MockGetEof;
  cls.read = MockRead;
  cls.write = MockWrite;
  for (int i = 0; i < H5FD_MEM_NTYPES; ++i) {
    cls.fl_map[i] = H5FD_MEM_SUPER;
  }
  return cls;
}

H5FD_onion_header_t MakeHeader(uint32_t flags, uint32_t page_size,
                               uint64_t origin_eof, uint64_t history_addr,
                               uint64_t history_size) {
  H5FD_onion_header_t hdr{};
  hdr.version = H5FD_ONION_HEADER_VERSION_CURR;
  hdr.flags = flags;
  hdr.page_size = page_size;
  hdr.origin_eof = origin_eof;
  hdr.history_addr = history_addr;
  hdr.history_size = history_size;
  return hdr;
}

TEST(H5FDonionHeaderTest, ConstantsMatchOnDiskFormat) {
  static_assert(H5FD_ONION_ENCODED_SIZE_HEADER == 40);
  static_assert(H5FD_ONION_HEADER_VERSION_CURR == 1);
  static_assert(H5FD_ONION_HEADER_FLAG_WRITE_LOCK == 0x1);
  static_assert(H5FD_ONION_HEADER_FLAG_PAGE_ALIGNMENT == 0x2);

  EXPECT_STREQ(H5FD_ONION_HEADER_SIGNATURE, "OHDH");
}

TEST(H5FDonionHeaderTest, EncodeAndDecodeRoundTripPreservesFields) {
  struct HeaderCase {
    uint32_t flags;
    uint32_t page_size;
    uint32_t origin_eof;
    uint32_t history_addr;
    uint32_t history_size;
  };

  // Note: upstream H5FD__onion_header_decode() uses UINT32DECODE into
  // uint64_t fields for origin_eof, history_addr, and history_size, so test
  // values stay within 32 bits for exact round-trip checks.
  constexpr auto kCases = std::to_array<HeaderCase>({
      {/*.flags=*/0u, /*.page_size=*/64u, /*.origin_eof=*/0u,
       /*.history_addr=*/40u, /*.history_size=*/20u},
      {/*.flags=*/H5FD_ONION_HEADER_FLAG_WRITE_LOCK |
           H5FD_ONION_HEADER_FLAG_PAGE_ALIGNMENT,
       /*.page_size=*/4096u, /*.origin_eof=*/8192u,
       /*.history_addr=*/16384u, /*.history_size=*/128u},
      {/*.flags=*/0x00ABCDEFu, /*.page_size=*/65536u,
       /*.origin_eof=*/0x12345678u, /*.history_addr=*/0x9ABCDEF0u,
       /*.history_size=*/0x00FEDCBAu},
  });

  for (const HeaderCase& tc : kCases) {
    H5FD_onion_header_t in = MakeHeader(tc.flags, tc.page_size, tc.origin_eof,
                                        tc.history_addr, tc.history_size);
    std::array<unsigned char, H5FD_ONION_ENCODED_SIZE_HEADER> buf{};
    uint32_t checksum = 0;

    const size_t encoded =
        H5FD__onion_header_encode(&in, buf.data(), &checksum);
    ASSERT_EQ(encoded, size_t{H5FD_ONION_ENCODED_SIZE_HEADER});

    // Verify signature ("OHDH"), version, and 3-byte little-endian flags.
    EXPECT_THAT(std::to_array({buf[0], buf[1], buf[2], buf[3], buf[4]}),
                ElementsAre('O', 'H', 'D', 'H', 1));
    EXPECT_EQ(buf[5], static_cast<unsigned char>(tc.flags & 0xFFu));
    EXPECT_EQ(buf[6], static_cast<unsigned char>((tc.flags >> 8) & 0xFFu));
    EXPECT_EQ(buf[7], static_cast<unsigned char>((tc.flags >> 16) & 0xFFu));
    EXPECT_EQ(checksum, H5_checksum_fletcher32(
                            buf.data(), H5FD_ONION_ENCODED_SIZE_HEADER - 4));

    H5FD_onion_header_t out{};
    out.version = H5FD_ONION_HEADER_VERSION_CURR;
    const size_t decoded = H5FD__onion_header_decode(buf.data(), &out);
    ASSERT_EQ(decoded, size_t{H5FD_ONION_ENCODED_SIZE_HEADER});

    EXPECT_EQ(out.version, H5FD_ONION_HEADER_VERSION_CURR);
    EXPECT_EQ(out.flags, tc.flags);
    EXPECT_EQ(out.page_size, tc.page_size);
    EXPECT_EQ(out.origin_eof, tc.origin_eof);
    EXPECT_EQ(out.history_addr, tc.history_addr);
    EXPECT_EQ(out.history_size, tc.history_size);
    EXPECT_EQ(out.checksum, checksum);
  }
}

TEST(H5FDonionHeaderTest, DecodeRejectsInvalidSignatureVersionAndChecksum) {
  H5FD_onion_header_t src =
      MakeHeader(H5FD_ONION_HEADER_FLAG_PAGE_ALIGNMENT, 4096, 1024, 2048, 64);
  std::array<unsigned char, H5FD_ONION_ENCODED_SIZE_HEADER> valid_buf{};
  uint32_t checksum = 0;
  ASSERT_EQ(H5FD__onion_header_encode(&src, valid_buf.data(), &checksum),
            size_t{H5FD_ONION_ENCODED_SIZE_HEADER});

  ScopedErrorSilence silence;

  // 1. Corrupt signature.
  {
    auto buf = valid_buf;
    buf[0] = 'X';
    H5FD_onion_header_t out{};
    out.version = H5FD_ONION_HEADER_VERSION_CURR;
    EXPECT_EQ(H5FD__onion_header_decode(buf.data(), &out), size_t{0});
  }

  // 2. Corrupt version byte.
  {
    auto buf = valid_buf;
    buf[4] = H5FD_ONION_HEADER_VERSION_CURR + 1;
    H5FD_onion_header_t out{};
    out.version = H5FD_ONION_HEADER_VERSION_CURR;
    EXPECT_EQ(H5FD__onion_header_decode(buf.data(), &out), size_t{0});
  }

  // 3. Corrupt payload byte (triggers checksum mismatch).
  {
    auto buf = valid_buf;
    buf[10] ^= 0x55u;
    H5FD_onion_header_t out{};
    out.version = H5FD_ONION_HEADER_VERSION_CURR;
    EXPECT_EQ(H5FD__onion_header_decode(buf.data(), &out), size_t{0});
  }

  // 4. Corrupt stored checksum bytes.
  {
    auto buf = valid_buf;
    buf[H5FD_ONION_ENCODED_SIZE_HEADER - 1] ^= 0xFFu;
    H5FD_onion_header_t out{};
    out.version = H5FD_ONION_HEADER_VERSION_CURR;
    EXPECT_EQ(H5FD__onion_header_decode(buf.data(), &out), size_t{0});
  }
}

TEST(H5FDonionHeaderTest, WriteAndIngestHeaderViaCoreVfdSucceeds) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFdFile fd(H5FDopen("onion_header_core.h5",
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(fd.valid());

  ASSERT_GE(
      H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, H5FD_ONION_ENCODED_SIZE_HEADER), 0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Write header at offset 0 and ingest it back.
  H5FD_onion_header_t in =
      MakeHeader(H5FD_ONION_HEADER_FLAG_WRITE_LOCK, 4096, 12345, 4096, 240);
  ASSERT_GE(H5FD__onion_write_header(&in, fd.get()), 0);

  H5FD_onion_header_t out{};
  out.version = H5FD_ONION_HEADER_VERSION_CURR;
  ASSERT_GE(H5FD__onion_ingest_header(&out, fd.get(), 0), 0);
  EXPECT_EQ(out.flags, in.flags);
  EXPECT_EQ(out.page_size, in.page_size);
  EXPECT_EQ(out.origin_eof, in.origin_eof);
  EXPECT_EQ(out.history_addr, in.history_addr);
  EXPECT_EQ(out.history_size, in.history_size);

  // 2. Write an encoded header at a non-zero offset and ingest from that addr.
  constexpr haddr_t kNonZeroAddr = 64;
  H5FD_onion_header_t in_at_offset =
      MakeHeader(H5FD_ONION_HEADER_FLAG_PAGE_ALIGNMENT, 8192, 999, 256, 40);
  std::array<unsigned char, H5FD_ONION_ENCODED_SIZE_HEADER> raw{};
  uint32_t checksum = 0;
  ASSERT_EQ(H5FD__onion_header_encode(&in_at_offset, raw.data(), &checksum),
            size_t{H5FD_ONION_ENCODED_SIZE_HEADER});

  ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW,
                        kNonZeroAddr + H5FD_ONION_ENCODED_SIZE_HEADER),
            0);
  ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, kNonZeroAddr,
                      raw.size(), raw.data()),
            0);

  H5FD_onion_header_t out_at_offset{};
  out_at_offset.version = H5FD_ONION_HEADER_VERSION_CURR;
  ASSERT_GE(H5FD__onion_ingest_header(&out_at_offset, fd.get(), kNonZeroAddr),
            0);
  EXPECT_EQ(out_at_offset.flags, in_at_offset.flags);
  EXPECT_EQ(out_at_offset.page_size, in_at_offset.page_size);
  EXPECT_EQ(out_at_offset.origin_eof, in_at_offset.origin_eof);
  EXPECT_EQ(out_at_offset.history_addr, in_at_offset.history_addr);
  EXPECT_EQ(out_at_offset.history_size, in_at_offset.history_size);
  EXPECT_EQ(out_at_offset.checksum, checksum);
}

TEST(H5FDonionHeaderTest, IngestAndWriteHeaderErrorPathsViaMockVfd) {
  MockHeaderVfdState state;
  ScopedMockState state_guard(&state);

  H5FD_class_t cls = MakeMockHeaderVfdClass();
  ScopedVfdDriver driver(H5FDregister(&cls));
  ASSERT_TRUE(driver.valid());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_driver(fapl.get(), driver.get(), nullptr), 0);

  ScopedFdFile fd(H5FDopen("mock_onion_hdr", H5F_ACC_RDWR | H5F_ACC_CREAT,
                           fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(fd.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // 1. Ingest fails when EOF is smaller than addr + header size.
  state.data.assign(H5FD_ONION_ENCODED_SIZE_HEADER - 1, 0);
  H5FD_onion_header_t out{};
  out.version = H5FD_ONION_HEADER_VERSION_CURR;
  EXPECT_LT(H5FD__onion_ingest_header(&out, fd.get(), 0), 0);

  // Populate valid header bytes for subsequent fault-injection checks.
  state.eoa = H5FD_ONION_ENCODED_SIZE_HEADER;
  H5FD_onion_header_t valid = MakeHeader(0, 4096, 512, 64, 20);
  ASSERT_GE(H5FD__onion_write_header(&valid, fd.get()), 0);

  // 2. Write fails when driver write returns error.
  state.fail_write = true;
  EXPECT_LT(H5FD__onion_write_header(&valid, fd.get()), 0);
  state.fail_write = false;

  // 3. Ingest fails when set_eoa fails.
  state.fail_set_eoa = true;
  EXPECT_LT(H5FD__onion_ingest_header(&out, fd.get(), 0), 0);
  state.fail_set_eoa = false;

  // 4. Ingest fails when read fails.
  state.fail_read = true;
  EXPECT_LT(H5FD__onion_ingest_header(&out, fd.get(), 0), 0);
  state.fail_read = false;

  // 5. Ingest fails when header bytes fail decoding.
  state.data[0] = 'Z';
  EXPECT_LT(H5FD__onion_ingest_header(&out, fd.get(), 0), 0);
}

}  // namespace
}  // namespace hdf5
