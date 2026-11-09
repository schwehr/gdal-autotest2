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
// Tests the Onion Virtual File Driver (VFD) archival index, revision index
// hash table, index merging, and revision record encoding/decoding/ingest in
// third_party/hdf5/src/H5FDonion_index.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDonion_index.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDONION_INDEX_TEST_RESTORE_UNDEF_NDEBUG 1
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

#ifdef H5FDONION_INDEX_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDONION_INDEX_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
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
// calls inside H5FD__onion_ingest_revision_record.
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

struct RevisionIndexDeleter {
  void operator()(H5FD_onion_revision_index_t* rix) const {
    if (rix != nullptr) {
      H5FD__onion_revision_index_destroy(rix);
    }
  }
};

using ScopedRevisionIndex =
    std::unique_ptr<H5FD_onion_revision_index_t, RevisionIndexDeleter>;

// RAII wrapper for H5FD_onion_revision_record_t populated by
// H5FD__onion_ingest_revision_record(). Note that on failure, ingest frees its
// own allocations internally without zeroing the pointers, so `owns_allocs`
// must only be enabled when ingest succeeds.
class ScopedIngestedRevisionRecord {
 public:
  ScopedIngestedRevisionRecord() { InitHeaders(); }
  ~ScopedIngestedRevisionRecord() { Reset(); }

  ScopedIngestedRevisionRecord(const ScopedIngestedRevisionRecord&) = delete;
  ScopedIngestedRevisionRecord& operator=(const ScopedIngestedRevisionRecord&) =
      delete;

  herr_t Ingest(H5FD_t* file, const H5FD_onion_history_t* history,
                uint64_t revision_num) {
    // ingest frees any pre-existing comment / list on entry and frees newly
    // allocated ones if it returns FAIL, but expects n_entries and comment_size
    // to be zeroed by the caller.
    rec_.comment_size = 0;
    rec_.archival_index.n_entries = 0;
    owns_allocs_ = false;
    const herr_t rc =
        H5FD__onion_ingest_revision_record(&rec_, file, history, revision_num);
    if (rc >= 0) {
      owns_allocs_ = true;
    } else {
      std::memset(&rec_, 0, sizeof(rec_));
      InitHeaders();
    }
    return rc;
  }

  const H5FD_onion_revision_record_t& get() const { return rec_; }

 private:
  void InitHeaders() {
    rec_.version = H5FD_ONION_REVISION_RECORD_VERSION_CURR;
    rec_.archival_index.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
  }

  void Reset() {
    if (owns_allocs_) {
      H5MM_xfree(rec_.comment);
      H5MM_xfree(rec_.archival_index.list);
    }
    std::memset(&rec_, 0, sizeof(rec_));
    InitHeaders();
    owns_allocs_ = false;
  }

  H5FD_onion_revision_record_t rec_{};
  bool owns_allocs_ = false;
};

// In-memory fault-injecting VFD for deterministic revision record ingest error
// testing without disk I/O.
struct MockIndexVfdState {
  std::vector<uint8_t> data;
  haddr_t eoa = 0;
  bool fail_set_eoa = false;
  int read_calls = 0;
  int fail_read_on_call = 0;
};

struct MockIndexVfdFile {
  H5FD_t pub{};
  MockIndexVfdState* state = nullptr;
};

static_assert(std::is_standard_layout_v<MockIndexVfdFile>);
static_assert(offsetof(MockIndexVfdFile, pub) == 0);

MockIndexVfdState* g_mock_state = nullptr;

class ScopedMockState {
 public:
  explicit ScopedMockState(MockIndexVfdState* state)
      : prev_(std::exchange(g_mock_state, state)) {}
  ~ScopedMockState() { g_mock_state = prev_; }

  ScopedMockState(const ScopedMockState&) = delete;
  ScopedMockState& operator=(const ScopedMockState&) = delete;

 private:
  MockIndexVfdState* prev_;
};

H5FD_t* MockOpen(const char* /*name*/, unsigned /*flags*/, hid_t /*fapl_id*/,
                 haddr_t /*maxaddr*/) {
  if (g_mock_state == nullptr) {
    return nullptr;
  }
  auto* file = new MockIndexVfdFile();
  file->state = g_mock_state;
  return &file->pub;
}

herr_t MockClose(H5FD_t* _file) {
  delete reinterpret_cast<MockIndexVfdFile*>(_file);
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
  return reinterpret_cast<const MockIndexVfdFile*>(_file)->state->eoa;
}

herr_t MockSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<MockIndexVfdFile*>(_file);
  if (file->state->fail_set_eoa) {
    return -1;
  }
  file->state->eoa = addr;
  return 0;
}

haddr_t MockGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return static_cast<haddr_t>(
      reinterpret_cast<const MockIndexVfdFile*>(_file)->state->data.size());
}

herr_t MockRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, void* buf) {
  auto* file = reinterpret_cast<MockIndexVfdFile*>(_file);
  ++file->state->read_calls;
  if (file->state->fail_read_on_call == file->state->read_calls) {
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
  auto* file = reinterpret_cast<MockIndexVfdFile*>(_file);
  const size_t end = static_cast<size_t>(addr) + size;
  if (end > file->state->data.size()) {
    file->state->data.resize(end, 0);
  }
  if (size > 0) {
    std::memcpy(file->state->data.data() + addr, buf, size);
  }
  return 0;
}

H5FD_class_t MakeMockIndexVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = 263;
  cls.name = "mock_onion_index_vfd";
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

std::vector<unsigned char> EncodeRevisionRecord(
    uint64_t rev_num, uint64_t parent_rev, uint64_t logical_eof,
    uint32_t page_size_log2, std::vector<H5FD_onion_index_entry_t> entries,
    std::string comment, uint32_t* checksum_out = nullptr) {
  H5FD_onion_revision_record_t rec{};
  rec.version = H5FD_ONION_REVISION_RECORD_VERSION_CURR;
  rec.revision_num = rev_num;
  rec.parent_revision_num = parent_rev;
  constexpr char kTimestamp[16] = {'2', '0', '2', '6', '1', '1', '0', '8',
                                   'T', '1', '2', '0', '0', '0', '0', 'Z'};
  std::memcpy(rec.time_of_creation, kTimestamp, sizeof(kTimestamp));
  rec.logical_eof = logical_eof;
  rec.archival_index.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
  rec.archival_index.page_size_log2 = page_size_log2;
  rec.archival_index.n_entries = entries.size();
  rec.archival_index.list = entries.empty() ? nullptr : entries.data();
  if (!comment.empty()) {
    rec.comment_size = static_cast<uint32_t>(comment.size() + 1);
    rec.comment = comment.data();
  }

  const size_t cap =
      H5FD_ONION_ENCODED_SIZE_REVISION_RECORD + rec.comment_size +
      H5FD_ONION_ENCODED_SIZE_INDEX_ENTRY * rec.archival_index.n_entries;
  std::vector<unsigned char> buf(cap, 0);
  uint32_t sum = 0;
  const size_t nbytes =
      H5FD__onion_revision_record_encode(&rec, buf.data(), &sum);
  buf.resize(nbytes);
  if (checksum_out != nullptr) {
    *checksum_out = sum;
  }
  return buf;
}

TEST(H5FDonionIndexTest, ArchivalIndexIsValidChecksVersionListAndSorting) {
  auto sorted_entries = std::to_array<H5FD_onion_index_entry_t>({
      {/*.logical_page=*/1u, /*.phys_addr=*/100u},
      {/*.logical_page=*/4u, /*.phys_addr=*/200u},
      {/*.logical_page=*/7u, /*.phys_addr=*/300u},
      {/*.logical_page=*/12u, /*.phys_addr=*/400u},
  });

  H5FD_onion_archival_index_t aix{};
  aix.version = 0;
  aix.list = sorted_entries.data();
  EXPECT_FALSE(H5FD__onion_archival_index_is_valid(&aix));

  aix.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
  aix.list = nullptr;
  EXPECT_FALSE(H5FD__onion_archival_index_is_valid(&aix));

  aix.list = sorted_entries.data();
  aix.n_entries = 0;
  EXPECT_TRUE(H5FD__onion_archival_index_is_valid(&aix));

  aix.n_entries = 1;
  EXPECT_TRUE(H5FD__onion_archival_index_is_valid(&aix));

  aix.n_entries = sorted_entries.size();
  EXPECT_TRUE(H5FD__onion_archival_index_is_valid(&aix));

  // Upstream loop compares adjacent pairs starting at index 1..n_entries-2.
  auto unsorted_entries = sorted_entries;
  unsorted_entries[2].logical_page = 3u;
  aix.list = unsorted_entries.data();
  EXPECT_FALSE(H5FD__onion_archival_index_is_valid(&aix));

  auto duplicate_entries = sorted_entries;
  duplicate_entries[2].logical_page = duplicate_entries[1].logical_page;
  aix.list = duplicate_entries.data();
  EXPECT_FALSE(H5FD__onion_archival_index_is_valid(&aix));
}

TEST(H5FDonionIndexTest, ArchivalIndexFindCoversBoundaryAndInteriorCases) {
  // 1. Empty archival index returns 0 without modifying entry_out.
  {
    H5FD_onion_archival_index_t empty_aix{};
    empty_aix.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
    const H5FD_onion_index_entry_t* found = nullptr;
    EXPECT_EQ(H5FD__onion_archival_index_find(&empty_aix, 0u, &found), 0);
    EXPECT_EQ(found, nullptr);
  }

  // 2. Single-entry archival index: document upstream binary search quirk where
  //    (n != low || n != high) evaluates to false when n == low == high == 0.
  {
    H5FD_onion_index_entry_t single{/*.logical_page=*/10u, /*.phys_addr=*/512u};
    H5FD_onion_archival_index_t one_aix{};
    one_aix.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
    one_aix.n_entries = 1;
    one_aix.list = &single;
    const H5FD_onion_index_entry_t* found = nullptr;
    EXPECT_EQ(H5FD__onion_archival_index_find(&one_aix, 10u, &found), 0);
    EXPECT_EQ(found, nullptr);
  }

  // 3. Multi-entry sorted archival index covers exact hits (start, middle, end)
  //    and misses (below min, above max, and interior gaps).
  auto entries = std::to_array<H5FD_onion_index_entry_t>({
      {/*.logical_page=*/10u, /*.phys_addr=*/1000u},
      {/*.logical_page=*/20u, /*.phys_addr=*/2000u},
      {/*.logical_page=*/30u, /*.phys_addr=*/3000u},
      {/*.logical_page=*/40u, /*.phys_addr=*/4000u},
      {/*.logical_page=*/50u, /*.phys_addr=*/5000u},
  });
  H5FD_onion_archival_index_t aix{};
  aix.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
  aix.n_entries = entries.size();
  aix.list = entries.data();

  for (const auto& entry : entries) {
    const H5FD_onion_index_entry_t* found = nullptr;
    ASSERT_EQ(H5FD__onion_archival_index_find(&aix, entry.logical_page, &found),
              1)
        << "logical_page=" << entry.logical_page;
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->logical_page, entry.logical_page);
    EXPECT_EQ(found->phys_addr, entry.phys_addr);
  }

  for (const uint64_t missing : {5u, 15u, 25u, 35u, 45u, 55u}) {
    const H5FD_onion_index_entry_t* found = nullptr;
    EXPECT_EQ(H5FD__onion_archival_index_find(&aix, missing, &found), 0)
        << "missing=" << missing;
    EXPECT_EQ(found, nullptr);
  }

  // 4. Two-entry index covers n=0 -> low=1 leaf path.
  H5FD_onion_archival_index_t two_aix{};
  two_aix.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
  two_aix.n_entries = 2;
  two_aix.list = entries.data();
  const H5FD_onion_index_entry_t* found = nullptr;
  ASSERT_EQ(H5FD__onion_archival_index_find(&two_aix, 20u, &found), 1);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->phys_addr, 2000u);
}

TEST(H5FDonionIndexTest, RevisionIndexLifecycleInsertUpdateCollideAndFind) {
  ScopedRevisionIndex rix(H5FD__onion_revision_index_init(4096));
  ASSERT_NE(rix, nullptr);
  EXPECT_EQ(rix->version, H5FD_ONION_REVISION_INDEX_VERSION_CURR);
  EXPECT_EQ(rix->n_entries, uint64_t{0});
  EXPECT_EQ(rix->page_size_log2, uint32_t{12});
  EXPECT_EQ(rix->_hash_table_size,
            uint64_t{1} << H5FD_ONION_REVISION_INDEX_STARTING_SIZE_LOG2);

  const H5FD_onion_index_entry_t* found = nullptr;
  EXPECT_EQ(H5FD__onion_revision_index_find(rix.get(), 7u, &found), 0);
  EXPECT_EQ(found, nullptr);

  // 1. Insert initial entry.
  const H5FD_onion_index_entry_t e1{/*.logical_page=*/7u, /*.phys_addr=*/1000u};
  ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &e1), 0);
  EXPECT_EQ(rix->n_entries, uint64_t{1});
  ASSERT_EQ(H5FD__onion_revision_index_find(rix.get(), 7u, &found), 1);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->phys_addr, 1000u);

  // 2. Re-inserting same logical_page and phys_addr updates in place.
  ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &e1), 0);
  EXPECT_EQ(rix->n_entries, uint64_t{1});

  // 3. Re-inserting same logical_page with mismatched phys_addr fails.
  {
    ScopedErrorSilence silence;
    const H5FD_onion_index_entry_t conflict{/*.logical_page=*/7u,
                                            /*.phys_addr=*/2000u};
    EXPECT_LT(H5FD__onion_revision_index_insert(rix.get(), &conflict), 0);
  }

  // 4. Insert distinct pages hashing to the same bucket to build a chain.
  const uint64_t stride = rix->_hash_table_size;
  const H5FD_onion_index_entry_t c1{/*.logical_page=*/7u + stride,
                                    /*.phys_addr=*/3000u};
  const H5FD_onion_index_entry_t c2{/*.logical_page=*/7u + 2 * stride,
                                    /*.phys_addr=*/4000u};
  ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &c1), 0);
  ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &c2), 0);
  EXPECT_EQ(rix->n_entries, uint64_t{3});

  found = nullptr;
  ASSERT_EQ(H5FD__onion_revision_index_find(rix.get(), c2.logical_page, &found),
            1);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->phys_addr, 4000u);

  // Lookup in populated bucket for absent key traverses entire chain.
  found = nullptr;
  EXPECT_EQ(H5FD__onion_revision_index_find(rix.get(), 7u + 3 * stride, &found),
            0);
  EXPECT_EQ(found, nullptr);
}

TEST(H5FDonionIndexTest,
     RevisionIndexResizeRehashesPopulatedAndCollidingBuckets) {
  // 1. Trigger resize via _hash_table_n_keys_populated >= _hash_table_size / 2.
  {
    ScopedRevisionIndex rix(H5FD__onion_revision_index_init(4096));
    ASSERT_NE(rix, nullptr);

    const uint64_t initial_size = rix->_hash_table_size;
    ASSERT_EQ(initial_size, uint64_t{1024});

    // Insert 0 and 2048 so bucket 0 holds two entries that still map to bucket
    // 0 after doubling to 2048 slots, exercising the resize collision branch
    // cleanly.
    const H5FD_onion_index_entry_t p0{/*.logical_page=*/0u,
                                      /*.phys_addr=*/100u};
    const H5FD_onion_index_entry_t p2048{/*.logical_page=*/2048u,
                                         /*.phys_addr=*/200u};
    ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &p0), 0);
    ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &p2048), 0);

    for (uint64_t page = 1; page < initial_size / 2; ++page) {
      const H5FD_onion_index_entry_t e{/*.logical_page=*/page,
                                       /*.phys_addr=*/(page + 10) * 8u};
      ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &e), 0);
    }
    EXPECT_EQ(rix->_hash_table_size, initial_size);

    // 512th distinct bucket already populated -> next insert triggers resize.
    const H5FD_onion_index_entry_t trigger{/*.logical_page=*/512u,
                                           /*.phys_addr=*/9999u};
    ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &trigger), 0);
    EXPECT_EQ(rix->_hash_table_size, initial_size * 2);
    EXPECT_EQ(rix->_hash_table_size_log2,
              uint64_t{H5FD_ONION_REVISION_INDEX_STARTING_SIZE_LOG2 + 1});

    const H5FD_onion_index_entry_t* found = nullptr;
    ASSERT_EQ(H5FD__onion_revision_index_find(rix.get(), 0u, &found), 1);
    EXPECT_EQ(found->phys_addr, 100u);
    ASSERT_EQ(H5FD__onion_revision_index_find(rix.get(), 2048u, &found), 1);
    EXPECT_EQ(found->phys_addr, 200u);
    ASSERT_EQ(H5FD__onion_revision_index_find(rix.get(), 512u, &found), 1);
    EXPECT_EQ(found->phys_addr, 9999u);
  }

  // 2. Trigger resize via n_entries >= _hash_table_size * 2 on a single bucket.
  {
    ScopedRevisionIndex rix(H5FD__onion_revision_index_init(4096));
    ASSERT_NE(rix, nullptr);

    const uint64_t initial_size = rix->_hash_table_size;
    // All multiples of 4096 map to bucket 0 in both 1024- and 2048-slot tables.
    for (uint64_t i = 0; i <= initial_size * 2; ++i) {
      const H5FD_onion_index_entry_t e{/*.logical_page=*/i * 4096u,
                                       /*.phys_addr=*/(i + 1) * 16u};
      ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &e), 0);
    }
    EXPECT_EQ(rix->_hash_table_size, initial_size * 2);
    EXPECT_EQ(rix->n_entries, initial_size * 2 + 1);
  }
}

TEST(H5FDonionIndexTest, MergeRevisionIndexIntoArchivalIndex) {
  ScopedRevisionIndex rix(H5FD__onion_revision_index_init(4096));
  ASSERT_NE(rix, nullptr);

  H5FD_onion_archival_index_t aix{};
  aix.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
  aix.page_size_log2 = rix->page_size_log2;
  absl::Cleanup free_aix = [&aix] { H5MM_xfree(aix.list); };

  // 1. Merging an empty revision index is a no-op.
  ASSERT_GE(
      H5FD__onion_merge_revision_index_into_archival_index(rix.get(), &aix), 0);
  EXPECT_EQ(aix.n_entries, uint64_t{0});
  EXPECT_EQ(aix.list, nullptr);

  // 2. Merge unsorted revision entries into empty archival index.
  for (const auto& e : std::to_array<H5FD_onion_index_entry_t>({
           {/*.logical_page=*/30u, /*.phys_addr=*/300u},
           {/*.logical_page=*/10u, /*.phys_addr=*/100u},
           {/*.logical_page=*/20u, /*.phys_addr=*/200u},
       })) {
    ASSERT_GE(H5FD__onion_revision_index_insert(rix.get(), &e), 0);
  }
  ASSERT_GE(
      H5FD__onion_merge_revision_index_into_archival_index(rix.get(), &aix), 0);
  ASSERT_EQ(aix.n_entries, uint64_t{3});
  EXPECT_EQ(aix.list[0].logical_page, 10u);
  EXPECT_EQ(aix.list[1].logical_page, 20u);
  EXPECT_EQ(aix.list[2].logical_page, 30u);

  // 3. Merge another revision index containing an overlapping page (20) and
  //    new pages (5, 25, 40) while preserving non-overlapping archival pages
  //    (10, 30).
  ScopedRevisionIndex rix2(H5FD__onion_revision_index_init(4096));
  ASSERT_NE(rix2, nullptr);
  for (const auto& e : std::to_array<H5FD_onion_index_entry_t>({
           {/*.logical_page=*/40u, /*.phys_addr=*/4000u},
           {/*.logical_page=*/20u, /*.phys_addr=*/2222u},
           {/*.logical_page=*/5u, /*.phys_addr=*/50u},
           {/*.logical_page=*/25u, /*.phys_addr=*/2500u},
       })) {
    ASSERT_GE(H5FD__onion_revision_index_insert(rix2.get(), &e), 0);
  }
  ASSERT_GE(
      H5FD__onion_merge_revision_index_into_archival_index(rix2.get(), &aix),
      0);
  ASSERT_EQ(aix.n_entries, uint64_t{6});
  EXPECT_THAT(
      std::to_array({aix.list[0].logical_page, aix.list[1].logical_page,
                     aix.list[2].logical_page, aix.list[3].logical_page,
                     aix.list[4].logical_page, aix.list[5].logical_page}),
      ElementsAre(5u, 10u, 20u, 25u, 30u, 40u));
  EXPECT_EQ(aix.list[0].phys_addr, 50u);
  EXPECT_EQ(aix.list[1].phys_addr, 100u);
  EXPECT_EQ(aix.list[2].phys_addr, 2222u);
  EXPECT_EQ(aix.list[3].phys_addr, 2500u);
  EXPECT_EQ(aix.list[4].phys_addr, 300u);
  EXPECT_EQ(aix.list[5].phys_addr, 4000u);

  // 4. Merging overlapping and kept entries including equal kept pages also
  //    exercises the equality return path in the sort comparator.
  ScopedRevisionIndex rix3(H5FD__onion_revision_index_init(4096));
  ASSERT_NE(rix3, nullptr);
  const H5FD_onion_index_entry_t e50{/*.logical_page=*/50u,
                                     /*.phys_addr=*/5000u};
  ASSERT_GE(H5FD__onion_revision_index_insert(rix3.get(), &e50), 0);
  aix.list[0].logical_page = 99u;
  aix.list[1].logical_page = 99u;
  ASSERT_GE(
      H5FD__onion_merge_revision_index_into_archival_index(rix3.get(), &aix),
      0);
  EXPECT_EQ(aix.n_entries, uint64_t{7});
}

TEST(H5FDonionIndexTest, RevisionRecordEncodeAndDecodeTwoPassRoundTrip) {
  // 1. Minimal record: 0 entries and no comment.
  {
    uint32_t sum = 0;
    std::vector<unsigned char> buf =
        EncodeRevisionRecord(/*rev_num=*/0u, /*parent_rev=*/0u,
                             /*logical_eof=*/512u, /*page_size_log2=*/10u,
                             /*entries=*/{}, /*comment=*/"", &sum);
    ASSERT_EQ(buf.size(), size_t{H5FD_ONION_ENCODED_SIZE_REVISION_RECORD});

    H5FD_onion_revision_record_t out{};
    out.version = H5FD_ONION_REVISION_RECORD_VERSION_CURR;
    out.archival_index.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
    ASSERT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), buf.size());
    EXPECT_EQ(out.revision_num, uint64_t{0});
    EXPECT_EQ(out.logical_eof, uint64_t{512});
    EXPECT_EQ(out.archival_index.page_size_log2, uint32_t{10});
    EXPECT_EQ(out.archival_index.n_entries, uint64_t{0});
    EXPECT_EQ(out.comment_size, uint32_t{0});
    EXPECT_EQ(out.checksum, sum);
  }

  // 2. Full record: multiple index entries and non-empty comment.
  {
    std::vector<H5FD_onion_index_entry_t> entries = {
        {/*.logical_page=*/0u, /*.phys_addr=*/1024u},
        {/*.logical_page=*/3u, /*.phys_addr=*/5120u},
        {/*.logical_page=*/9u, /*.phys_addr=*/9216u},
    };
    const std::string comment = "snapshot-comment";
    uint32_t sum = 0;
    std::vector<unsigned char> buf = EncodeRevisionRecord(
        /*rev_num=*/7u, /*parent_rev=*/6u, /*logical_eof=*/65536u,
        /*page_size_log2=*/12u, entries, comment, &sum);

    const size_t expected_size =
        H5FD_ONION_ENCODED_SIZE_REVISION_RECORD +
        H5FD_ONION_ENCODED_SIZE_INDEX_ENTRY * entries.size() +
        (comment.size() + 1);
    ASSERT_EQ(buf.size(), expected_size);

    H5FD_onion_revision_record_t out{};
    out.version = H5FD_ONION_REVISION_RECORD_VERSION_CURR;
    out.archival_index.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
    ASSERT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), buf.size());
    ASSERT_EQ(out.archival_index.n_entries, entries.size());
    ASSERT_EQ(out.comment_size, comment.size() + 1);
    EXPECT_EQ(out.checksum, sum);

    std::vector<H5FD_onion_index_entry_t> decoded_entries(
        out.archival_index.n_entries);
    std::vector<char> decoded_comment(out.comment_size, '\0');
    out.archival_index.list = decoded_entries.data();
    out.comment = decoded_comment.data();

    ASSERT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), buf.size());
    EXPECT_EQ(out.revision_num, uint64_t{7});
    EXPECT_EQ(out.parent_revision_num, uint64_t{6});
    EXPECT_EQ(out.logical_eof, uint64_t{65536});
    EXPECT_EQ(out.archival_index.page_size_log2, uint32_t{12});
    EXPECT_STREQ(decoded_comment.data(), comment.c_str());
    for (size_t i = 0; i < entries.size(); ++i) {
      EXPECT_EQ(decoded_entries[i].logical_page, entries[i].logical_page);
      EXPECT_EQ(decoded_entries[i].phys_addr, entries[i].phys_addr);
    }
  }
}

TEST(H5FDonionIndexTest, RevisionRecordDecodeRejectsCorruptAndInvalidInputs) {
  std::vector<H5FD_onion_index_entry_t> entries = {
      {/*.logical_page=*/2u, /*.phys_addr=*/4096u},
  };
  const std::vector<unsigned char> valid_buf = EncodeRevisionRecord(
      /*rev_num=*/1u, /*parent_rev=*/0u,
      /*logical_eof=*/16384u, /*page_size_log2=*/12u, entries, "abc");

  auto make_empty_record = []() {
    H5FD_onion_revision_record_t r{};
    r.version = H5FD_ONION_REVISION_RECORD_VERSION_CURR;
    r.archival_index.version = H5FD_ONION_ARCHIVAL_INDEX_VERSION_CURR;
    return r;
  };

  ScopedErrorSilence silence;

  // 1. Invalid signature.
  {
    auto buf = valid_buf;
    buf[0] = 'X';
    auto out = make_empty_record();
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 2. Invalid version.
  {
    auto buf = valid_buf;
    buf[4] = H5FD_ONION_REVISION_RECORD_VERSION_CURR + 1;
    auto out = make_empty_record();
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 3. page_size == 0 (page_size sits at bytes 48..51 in encoded record).
  {
    auto buf = valid_buf;
    buf[48] = 0;
    buf[49] = 0;
    buf[50] = 0;
    buf[51] = 0;
    auto out = make_empty_record();
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 4. Non-power-of-two page_size (3).
  {
    auto buf = valid_buf;
    buf[48] = 3;
    buf[49] = 0;
    buf[50] = 0;
    buf[51] = 0;
    auto out = make_empty_record();
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 5. Pass 2 mismatch on n_entries.
  {
    auto buf = valid_buf;
    auto out = make_empty_record();
    out.archival_index.n_entries = 2;
    std::array<H5FD_onion_index_entry_t, 2> list{};
    out.archival_index.list = list.data();
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 6. Pass 2 with null archival_index.list.
  {
    auto buf = valid_buf;
    auto out = make_empty_record();
    out.archival_index.n_entries = 1;
    out.archival_index.list = nullptr;
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 7. Unaligned logical address in pass 2 (index entry begins at offset 64).
  {
    auto buf = valid_buf;
    buf[64] |= 0x01u;
    auto out = make_empty_record();
    out.archival_index.n_entries = 1;
    H5FD_onion_index_entry_t entry{};
    out.archival_index.list = &entry;
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 8. Index entry checksum mismatch in pass 2 (entry checksum at
  // offset 80..83).
  {
    auto buf = valid_buf;
    buf[80] ^= 0xFFu;
    auto out = make_empty_record();
    out.archival_index.n_entries = 1;
    H5FD_onion_index_entry_t entry{};
    out.archival_index.list = &entry;
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 9. Pass 1 with comment pointer already non-null.
  {
    auto buf = valid_buf;
    auto out = make_empty_record();
    char dummy_comment[8] = {};
    out.comment = dummy_comment;
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 10. Pass 2 with comment_size != 0 but comment == nullptr.
  {
    auto buf = valid_buf;
    auto out = make_empty_record();
    out.comment_size = 4;
    out.comment = nullptr;
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }

  // 11. Overall record checksum mismatch.
  {
    auto buf = valid_buf;
    buf.back() ^= 0x42u;
    auto out = make_empty_record();
    EXPECT_EQ(H5FD__onion_revision_record_decode(buf.data(), &out), size_t{0});
  }
}

TEST(H5FDonionIndexTest, IngestRevisionRecordBinarySearchAndCoreVfd) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFdFile fd(H5FDopen("onion_index_core.h5",
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(fd.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Build 5 revision records with sorted revision IDs {10, 20, 30, 40, 50}.
  struct Spec {
    uint64_t rev_num;
    std::vector<H5FD_onion_index_entry_t> entries;
    std::string comment;
  };
  const std::vector<Spec> specs = {
      {10u, {}, ""},
      {20u, {{1u, 1024u}}, "rev20"},
      {30u, {{2u, 2048u}, {5u, 4096u}}, "rev30"},
      {40u, {{3u, 3072u}}, ""},
      {50u, {{4u, 8192u}, {8u, 16384u}}, "rev50"},
  };

  std::vector<H5FD_onion_record_loc_t> locs(specs.size());
  haddr_t cursor = 0;
  for (size_t i = 0; i < specs.size(); ++i) {
    uint32_t sum = 0;
    std::vector<unsigned char> rec_buf = EncodeRevisionRecord(
        specs[i].rev_num, specs[i].rev_num - 10u, (i + 1) * 4096u, 12u,
        specs[i].entries, specs[i].comment, &sum);
    locs[i] = {/*.phys_addr=*/cursor, /*.record_size=*/rec_buf.size(),
               /*.checksum=*/sum};
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, cursor + rec_buf.size()), 0);
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, cursor,
                        rec_buf.size(), rec_buf.data()),
              0);
    cursor += rec_buf.size();
  }

  H5FD_onion_history_t history{};
  history.version = H5FD_ONION_HISTORY_VERSION_CURR;
  history.n_revisions = locs.size();
  history.record_locs = locs.data();

  // Drop EOA below last record to verify recovery-open EOA extension.
  ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, 0), 0);

  // Reuse the same ScopedIngestedRevisionRecord across lookups so prior
  // comment/list allocations are freed on entry.
  ScopedIngestedRevisionRecord ingested;
  for (const Spec& spec : specs) {
    ASSERT_GE(ingested.Ingest(fd.get(), &history, spec.rev_num), 0)
        << "rev_num=" << spec.rev_num;
    EXPECT_EQ(ingested.get().revision_num, spec.rev_num);
    EXPECT_EQ(ingested.get().archival_index.n_entries, spec.entries.size());
    if (spec.comment.empty()) {
      EXPECT_EQ(ingested.get().comment_size, uint32_t{0});
      EXPECT_EQ(ingested.get().comment, nullptr);
    } else {
      ASSERT_NE(ingested.get().comment, nullptr);
      EXPECT_STREQ(ingested.get().comment, spec.comment.c_str());
    }
  }

  // Single-revision history hits leaf branch immediately.
  H5FD_onion_history_t single_hist{};
  single_hist.version = H5FD_ONION_HISTORY_VERSION_CURR;
  single_hist.n_revisions = 1;
  single_hist.record_locs = &locs[0];
  ASSERT_GE(ingested.Ingest(fd.get(), &single_hist, 10u), 0);
  EXPECT_EQ(ingested.get().revision_num, uint64_t{10});

  // Missing revision IDs across low, interior, and high ranges fail cleanly.
  ScopedErrorSilence silence;
  for (const uint64_t missing : {5u, 25u, 55u}) {
    EXPECT_LT(ingested.Ingest(fd.get(), &history, missing), 0)
        << "missing=" << missing;
  }
  EXPECT_LT(ingested.Ingest(fd.get(), &single_hist, 99u), 0);
}

TEST(H5FDonionIndexTest, IngestRevisionRecordErrorPathsViaMockVfd) {
  MockIndexVfdState state;
  ScopedMockState state_guard(&state);

  H5FD_class_t cls = MakeMockIndexVfdClass();
  ScopedVfdDriver driver(H5FDregister(&cls));
  ASSERT_TRUE(driver.valid());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_driver(fapl.get(), driver.get(), nullptr), 0);

  ScopedFdFile fd(H5FDopen("mock_onion_idx", H5F_ACC_RDWR | H5F_ACC_CREAT,
                           fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(fd.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // Prepare 2 records so binary search enters both the `while (range > 0)`
  // loop (reads index 0) and the `if (range == 0)` leaf block (reads index 1).
  std::vector<unsigned char> rec0 =
      EncodeRevisionRecord(10u, 0u, 4096u, 12u, {{1u, 1024u}}, "first");
  std::vector<unsigned char> rec1 =
      EncodeRevisionRecord(20u, 10u, 8192u, 12u, {{2u, 2048u}}, "second");

  state.data = rec0;
  const haddr_t addr1 = state.data.size();
  state.data.insert(state.data.end(), rec1.begin(), rec1.end());
  state.eoa = state.data.size();

  auto locs = std::to_array<H5FD_onion_record_loc_t>({
      {/*.phys_addr=*/0u, /*.record_size=*/rec0.size(), /*.checksum=*/0u},
      {/*.phys_addr=*/addr1, /*.record_size=*/rec1.size(), /*.checksum=*/0u},
  });
  H5FD_onion_history_t history{};
  history.version = H5FD_ONION_HISTORY_VERSION_CURR;
  history.n_revisions = locs.size();
  history.record_locs = locs.data();

  ScopedIngestedRevisionRecord ingested;

  // 1. Fails when EOF < last record end.
  const std::vector<uint8_t> saved_data = state.data;
  state.data.pop_back();
  EXPECT_LT(ingested.Ingest(fd.get(), &history, 10u), 0);
  state.data = saved_data;

  // 2. Fails when EOA < addr + size and set_eoa fails.
  state.eoa = 0;
  state.fail_set_eoa = true;
  EXPECT_LT(ingested.Ingest(fd.get(), &history, 10u), 0);
  state.fail_set_eoa = false;
  state.eoa = state.data.size();

  // 3. Fails when read fails inside the binary search loop (1st read).
  state.read_calls = 0;
  state.fail_read_on_call = 1;
  EXPECT_LT(ingested.Ingest(fd.get(), &history, 20u), 0);

  // 4. Fails when read fails at the binary search leaf (2nd read).
  state.read_calls = 0;
  state.fail_read_on_call = 2;
  EXPECT_LT(ingested.Ingest(fd.get(), &history, 20u), 0);
  state.fail_read_on_call = 0;

  // 5. Fails when initial decode fails at the leaf record (corrupt rec1).
  state.data[addr1] = 'X';
  EXPECT_LT(ingested.Ingest(fd.get(), &history, 20u), 0);
  state.data[addr1] = saved_data[addr1];

  // 6. Fails when initial decode fails inside the search loop (corrupt rec0).
  state.data[0] = 'X';
  EXPECT_LT(ingested.Ingest(fd.get(), &history, 20u), 0);
  state.data[0] = saved_data[0];

  // 7. Fails during pass-2 decode (line 153): corrupt index entry alignment
  //    inside rec0 while recomputing outer checksum so pass 1 succeeds and
  //    ingest allocates comment + archival_index.list before pass 2 rejects it.
  std::vector<uint8_t> bad_pass2 = rec0;
  bad_pass2[64] |= 0x01u;  // Unaligned logical address in index entry 0.
  const uint32_t bad_sum =
      H5_checksum_fletcher32(bad_pass2.data(), bad_pass2.size() - 4);
  bad_pass2[bad_pass2.size() - 4] = static_cast<uint8_t>(bad_sum & 0xFFu);
  bad_pass2[bad_pass2.size() - 3] =
      static_cast<uint8_t>((bad_sum >> 8) & 0xFFu);
  bad_pass2[bad_pass2.size() - 2] =
      static_cast<uint8_t>((bad_sum >> 16) & 0xFFu);
  bad_pass2[bad_pass2.size() - 1] =
      static_cast<uint8_t>((bad_sum >> 24) & 0xFFu);
  std::copy(bad_pass2.begin(), bad_pass2.end(), state.data.begin());
  EXPECT_LT(ingested.Ingest(fd.get(), &history, 10u), 0);
}

}  // namespace
}  // namespace hdf5
