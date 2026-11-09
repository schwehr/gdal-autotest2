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
// Tests the Onion Virtual File Driver (VFD) history encoding, two-pass
// decoding, checksum verification, and backing-file ingest/write handling in
// third_party/hdf5/src/H5FDonion_history.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDonion_history.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDONION_HISTORY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDonion_priv.h"
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5MMprivate.h"
}  // extern "C"

#ifdef H5FDONION_HISTORY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDONION_HISTORY_TEST_RESTORE_UNDEF_NDEBUG
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
#include "third_party/absl/cleanup/cleanup.h"
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

// In-memory fault-injecting VFD for deterministic history ingest/write error
// testing without disk I/O.
struct MockHistoryVfdState {
  std::vector<uint8_t> data;
  haddr_t eoa = 0;
  bool fail_set_eoa = false;
  bool fail_read = false;
  bool fail_write = false;
};

struct MockHistoryVfdFile {
  H5FD_t pub{};
  MockHistoryVfdState* state = nullptr;
};

static_assert(std::is_standard_layout_v<MockHistoryVfdFile>);
static_assert(offsetof(MockHistoryVfdFile, pub) == 0);

MockHistoryVfdState* g_mock_state = nullptr;

class ScopedMockState {
 public:
  explicit ScopedMockState(MockHistoryVfdState* state)
      : prev_(std::exchange(g_mock_state, state)) {}
  ~ScopedMockState() { g_mock_state = prev_; }

  ScopedMockState(const ScopedMockState&) = delete;
  ScopedMockState& operator=(const ScopedMockState&) = delete;

 private:
  MockHistoryVfdState* prev_;
};

H5FD_t* MockOpen(const char* /*name*/, unsigned /*flags*/, hid_t /*fapl_id*/,
                 haddr_t /*maxaddr*/) {
  if (g_mock_state == nullptr) {
    return nullptr;
  }
  auto* file = new MockHistoryVfdFile();
  file->state = g_mock_state;
  return &file->pub;
}

herr_t MockClose(H5FD_t* _file) {
  delete reinterpret_cast<MockHistoryVfdFile*>(_file);
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
  return reinterpret_cast<const MockHistoryVfdFile*>(_file)->state->eoa;
}

herr_t MockSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<MockHistoryVfdFile*>(_file);
  if (file->state->fail_set_eoa) {
    return -1;
  }
  file->state->eoa = addr;
  return 0;
}

haddr_t MockGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return static_cast<haddr_t>(
      reinterpret_cast<const MockHistoryVfdFile*>(_file)->state->data.size());
}

herr_t MockRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, void* buf) {
  auto* file = reinterpret_cast<MockHistoryVfdFile*>(_file);
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
  auto* file = reinterpret_cast<MockHistoryVfdFile*>(_file);
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

H5FD_class_t MakeMockHistoryVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = 262;
  cls.name = "mock_onion_history_vfd";
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

TEST(H5FDonionHistoryTest, ConstantsMatchOnDiskFormat) {
  static_assert(H5FD_ONION_ENCODED_SIZE_HISTORY == 20);
  static_assert(H5FD_ONION_HISTORY_VERSION_CURR == 1);
  static_assert(H5FD_ONION_ENCODED_SIZE_RECORD_POINTER == 20);

  EXPECT_STREQ(H5FD_ONION_HISTORY_SIGNATURE, "OWHS");
}

TEST(H5FDonionHistoryTest, EncodeAndDecodeZeroRevisionsRoundTrip) {
  H5FD_onion_history_t in{};
  in.version = H5FD_ONION_HISTORY_VERSION_CURR;
  in.n_revisions = 0;
  in.record_locs = nullptr;

  std::array<unsigned char, H5FD_ONION_ENCODED_SIZE_HISTORY> buf{};
  uint32_t checksum = 0;
  const size_t encoded = H5FD__onion_history_encode(&in, buf.data(), &checksum);
  ASSERT_EQ(encoded, size_t{H5FD_ONION_ENCODED_SIZE_HISTORY});

  EXPECT_THAT(std::to_array({buf[0], buf[1], buf[2], buf[3], buf[4]}),
              ElementsAre('O', 'W', 'H', 'S', 1));
  EXPECT_EQ(checksum, H5_checksum_fletcher32(
                          buf.data(), H5FD_ONION_ENCODED_SIZE_HISTORY - 4));

  H5FD_onion_history_t out{};
  out.version = H5FD_ONION_HISTORY_VERSION_CURR;
  ASSERT_EQ(H5FD__onion_history_decode(buf.data(), &out),
            size_t{H5FD_ONION_ENCODED_SIZE_HISTORY});
  EXPECT_EQ(out.n_revisions, uint64_t{0});
  EXPECT_EQ(out.checksum, checksum);

  // Second decode call with 0 revisions still succeeds with null record_locs.
  ASSERT_EQ(H5FD__onion_history_decode(buf.data(), &out),
            size_t{H5FD_ONION_ENCODED_SIZE_HISTORY});
  EXPECT_EQ(out.n_revisions, uint64_t{0});
}

TEST(H5FDonionHistoryTest, EncodeAndDecodeMultipleRevisionsTwoPassRoundTrip) {
  auto locs_in = std::to_array<H5FD_onion_record_loc_t>({
      {/*.phys_addr=*/64u, /*.record_size=*/96u, /*.checksum=*/0x11223344u},
      {/*.phys_addr=*/4096u, /*.record_size=*/128u, /*.checksum=*/0xAABBCCDDu},
      {/*.phys_addr=*/0x123456789ABCDEF0ULL,
       /*.record_size=*/0x0000000100000100ULL,
       /*.checksum=*/0xCAFEBABEu},
  });

  H5FD_onion_history_t in{};
  in.version = H5FD_ONION_HISTORY_VERSION_CURR;
  in.n_revisions = locs_in.size();
  in.record_locs = locs_in.data();

  constexpr size_t kExpectedSize =
      H5FD_ONION_ENCODED_SIZE_HISTORY +
      H5FD_ONION_ENCODED_SIZE_RECORD_POINTER * locs_in.size();
  std::vector<unsigned char> buf(kExpectedSize, 0);
  uint32_t checksum = 0;

  ASSERT_EQ(H5FD__onion_history_encode(&in, buf.data(), &checksum),
            kExpectedSize);
  EXPECT_EQ(checksum, H5_checksum_fletcher32(buf.data(), kExpectedSize - 4));

  // Pass 1: n_revisions == 0 populates revision count and checksum.
  H5FD_onion_history_t out{};
  out.version = H5FD_ONION_HISTORY_VERSION_CURR;
  ASSERT_EQ(H5FD__onion_history_decode(buf.data(), &out), kExpectedSize);
  ASSERT_EQ(out.n_revisions, locs_in.size());
  EXPECT_EQ(out.checksum, checksum);

  // Pass 2: with allocated record_locs, decodes all revision pointers.
  std::vector<H5FD_onion_record_loc_t> locs_out(out.n_revisions);
  out.record_locs = locs_out.data();
  ASSERT_EQ(H5FD__onion_history_decode(buf.data(), &out), kExpectedSize);

  for (size_t i = 0; i < locs_in.size(); ++i) {
    EXPECT_EQ(locs_out[i].phys_addr, locs_in[i].phys_addr);
    EXPECT_EQ(locs_out[i].record_size, locs_in[i].record_size);
    EXPECT_EQ(locs_out[i].checksum, locs_in[i].checksum);
  }
}

TEST(H5FDonionHistoryTest, DecodeRejectsInvalidInputsAndMismatches) {
  auto locs = std::to_array<H5FD_onion_record_loc_t>({
      {/*.phys_addr=*/100u, /*.record_size=*/64u, /*.checksum=*/0x12345678u},
      {/*.phys_addr=*/200u, /*.record_size=*/64u, /*.checksum=*/0x87654321u},
  });

  H5FD_onion_history_t src{};
  src.version = H5FD_ONION_HISTORY_VERSION_CURR;
  src.n_revisions = locs.size();
  src.record_locs = locs.data();

  std::vector<unsigned char> valid_buf(H5FD_ONION_ENCODED_SIZE_HISTORY +
                                       H5FD_ONION_ENCODED_SIZE_RECORD_POINTER *
                                           locs.size());
  uint32_t checksum = 0;
  ASSERT_EQ(H5FD__onion_history_encode(&src, valid_buf.data(), &checksum),
            valid_buf.size());

  ScopedErrorSilence silence;

  // 1. Invalid signature.
  {
    auto buf = valid_buf;
    buf[0] = 'X';
    H5FD_onion_history_t out{};
    out.version = H5FD_ONION_HISTORY_VERSION_CURR;
    EXPECT_EQ(H5FD__onion_history_decode(buf.data(), &out), size_t{0});
  }

  // 2. Invalid version byte.
  {
    auto buf = valid_buf;
    buf[4] = H5FD_ONION_HISTORY_VERSION_CURR + 1;
    H5FD_onion_history_t out{};
    out.version = H5FD_ONION_HISTORY_VERSION_CURR;
    EXPECT_EQ(H5FD__onion_history_decode(buf.data(), &out), size_t{0});
  }

  // 3. Corrupted checksum.
  {
    auto buf = valid_buf;
    buf.back() ^= 0x11u;
    H5FD_onion_history_t out{};
    out.version = H5FD_ONION_HISTORY_VERSION_CURR;
    EXPECT_EQ(H5FD__onion_history_decode(buf.data(), &out), size_t{0});
  }

  // 4. Pass 2 mismatch: caller n_revisions differs from encoded count.
  {
    auto buf = valid_buf;
    H5FD_onion_history_t out{};
    out.version = H5FD_ONION_HISTORY_VERSION_CURR;
    out.n_revisions = locs.size() + 1;
    out.record_locs = locs.data();
    EXPECT_EQ(H5FD__onion_history_decode(buf.data(), &out), size_t{0});
  }

  // 5. Pass 2 error: caller record_locs is nullptr when n_revisions > 0.
  {
    auto buf = valid_buf;
    H5FD_onion_history_t out{};
    out.version = H5FD_ONION_HISTORY_VERSION_CURR;
    out.n_revisions = locs.size();
    out.record_locs = nullptr;
    EXPECT_EQ(H5FD__onion_history_decode(buf.data(), &out), size_t{0});
  }
}

TEST(H5FDonionHistoryTest, WriteAndIngestHistoryViaCoreVfdSucceeds) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFdFile fd(H5FDopen("onion_history_core.h5",
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(fd.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Zero-revision history written at offset 0 (triggers H5FD_set_eoa).
  {
    H5FD_onion_history_t in{};
    in.version = H5FD_ONION_HISTORY_VERSION_CURR;
    in.n_revisions = 0;

    const uint64_t written =
        H5FD__onion_write_history(&in, fd.get(), /*off_start=*/0,
                                  /*filesize_curr=*/0);
    ASSERT_EQ(written, uint64_t{H5FD_ONION_ENCODED_SIZE_HISTORY});

    H5FD_onion_history_t out{};
    out.version = H5FD_ONION_HISTORY_VERSION_CURR;
    ASSERT_GE(H5FD__onion_ingest_history(&out, fd.get(), 0, written), 0);
    EXPECT_EQ(out.n_revisions, uint64_t{0});
    EXPECT_EQ(out.record_locs, nullptr);
  }

  // 2. Multi-revision history written at non-zero offset with pre-sized EOA
  //    (exercises filesize_curr >= off_start + size skip of H5FD_set_eoa).
  {
    auto locs_in = std::to_array<H5FD_onion_record_loc_t>({
        {/*.phys_addr=*/128u, /*.record_size=*/80u, /*.checksum=*/0x10203040u},
        {/*.phys_addr=*/512u, /*.record_size=*/140u, /*.checksum=*/0x50607080u},
    });
    H5FD_onion_history_t in{};
    in.version = H5FD_ONION_HISTORY_VERSION_CURR;
    in.n_revisions = locs_in.size();
    in.record_locs = locs_in.data();

    constexpr haddr_t kOffset = 64;
    constexpr haddr_t kExpectedSize =
        H5FD_ONION_ENCODED_SIZE_HISTORY +
        H5FD_ONION_ENCODED_SIZE_RECORD_POINTER * locs_in.size();
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, kOffset + kExpectedSize), 0);

    const uint64_t written =
        H5FD__onion_write_history(&in, fd.get(), kOffset,
                                  /*filesize_curr=*/kOffset + kExpectedSize);
    ASSERT_EQ(written, kExpectedSize);

    H5FD_onion_history_t out{};
    out.version = H5FD_ONION_HISTORY_VERSION_CURR;
    ASSERT_GE(H5FD__onion_ingest_history(&out, fd.get(), kOffset, written), 0);
    absl::Cleanup free_locs = [&out] { H5MM_xfree(out.record_locs); };

    ASSERT_EQ(out.n_revisions, locs_in.size());
    ASSERT_NE(out.record_locs, nullptr);
    for (size_t i = 0; i < locs_in.size(); ++i) {
      EXPECT_EQ(out.record_locs[i].phys_addr, locs_in[i].phys_addr);
      EXPECT_EQ(out.record_locs[i].record_size, locs_in[i].record_size);
      EXPECT_EQ(out.record_locs[i].checksum, locs_in[i].checksum);
    }
  }
}

TEST(H5FDonionHistoryTest, IngestAndWriteHistoryErrorPathsViaMockVfd) {
  MockHistoryVfdState state;
  ScopedMockState state_guard(&state);

  H5FD_class_t cls = MakeMockHistoryVfdClass();
  ScopedVfdDriver driver(H5FDregister(&cls));
  ASSERT_TRUE(driver.valid());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_driver(fapl.get(), driver.get(), nullptr), 0);

  ScopedFdFile fd(H5FDopen("mock_onion_hist", H5F_ACC_RDWR | H5F_ACC_CREAT,
                           fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(fd.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  auto loc = H5FD_onion_record_loc_t{/*.phys_addr=*/40u, /*.record_size=*/68u,
                                     /*.checksum=*/0x11112222u};
  H5FD_onion_history_t hist{};
  hist.version = H5FD_ONION_HISTORY_VERSION_CURR;
  hist.n_revisions = 1;
  hist.record_locs = &loc;

  constexpr haddr_t kHistSize =
      H5FD_ONION_ENCODED_SIZE_HISTORY + H5FD_ONION_ENCODED_SIZE_RECORD_POINTER;

  // 1. Write fails when set_eoa fails while extending past filesize_curr.
  state.fail_set_eoa = true;
  EXPECT_EQ(H5FD__onion_write_history(&hist, fd.get(), 0, 0), uint64_t{0});
  state.fail_set_eoa = false;

  // 2. Write fails when driver write fails.
  state.fail_write = true;
  EXPECT_EQ(H5FD__onion_write_history(&hist, fd.get(), 0, 0), uint64_t{0});
  state.fail_write = false;

  // Write valid history for ingest failure checks.
  ASSERT_EQ(H5FD__onion_write_history(&hist, fd.get(), 0, 0), kHistSize);

  // 3. Ingest fails when EOF < addr + size.
  H5FD_onion_history_t out{};
  out.version = H5FD_ONION_HISTORY_VERSION_CURR;
  EXPECT_LT(H5FD__onion_ingest_history(&out, fd.get(), 0, kHistSize + 1), 0);
  EXPECT_EQ(out.record_locs, nullptr);

  // 4. Ingest fails when set_eoa fails.
  state.fail_set_eoa = true;
  out = {};
  out.version = H5FD_ONION_HISTORY_VERSION_CURR;
  EXPECT_LT(H5FD__onion_ingest_history(&out, fd.get(), 0, kHistSize), 0);
  state.fail_set_eoa = false;

  // 5. Ingest fails when read fails.
  state.fail_read = true;
  out = {};
  out.version = H5FD_ONION_HISTORY_VERSION_CURR;
  EXPECT_LT(H5FD__onion_ingest_history(&out, fd.get(), 0, kHistSize), 0);
  state.fail_read = false;

  // 6. Ingest fails when decoded size != requested size (extra bytes in file).
  state.data.push_back(0);
  state.data.push_back(0);
  out = {};
  out.version = H5FD_ONION_HISTORY_VERSION_CURR;
  EXPECT_LT(H5FD__onion_ingest_history(&out, fd.get(), 0, kHistSize + 2), 0);

  // 7. Ingest fails when stored bytes fail decode (corrupted signature).
  state.data[0] = 'X';
  out = {};
  out.version = H5FD_ONION_HISTORY_VERSION_CURR;
  EXPECT_LT(H5FD__onion_ingest_history(&out, fd.get(), 0, kHistSize), 0);
}

}  // namespace
}  // namespace hdf5
