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
// Tests global heap metadata cache callbacks: speculative load sizing,
// header/collection deserialization, serialization, image_len, and free_icr
// (third_party/hdf5/src/H5HGcache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HGcache.c

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug/stats fields from H5AC_info_t
// (the first member of H5HG_heap_t). Match those struct layouts across both
// the prelude and internal headers.
#ifndef NDEBUG
#define H5HGCACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5HG_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5HGpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5HGCACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HGCACHE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__GLOBALHEAP_TAG) {
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

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void WriteLeUint(uint8_t* dst, uint64_t val, size_t nbytes) {
  for (size_t i = 0; i < nbytes; ++i) {
    dst[i] = static_cast<uint8_t>((val >> (i * 8)) & 0xffU);
  }
}

std::vector<uint8_t> MakeHeapImage(size_t buf_len, uint64_t declared_size,
                                   uint8_t version, size_t sizeof_size) {
  std::vector<uint8_t> buf(buf_len, 0);
  if (buf_len >= H5_SIZEOF_MAGIC) {
    std::memcpy(buf.data(), H5HG_MAGIC, H5_SIZEOF_MAGIC);
  }
  if (buf_len >= 5) {
    buf[4] = version;
  }
  if (buf_len >= 8 + sizeof_size) {
    WriteLeUint(buf.data() + 8, declared_size, sizeof_size);
  }
  return buf;
}

void EncodeObjHdr(uint8_t* dst, uint16_t idx, uint16_t nrefs, uint64_t size,
                  size_t sizeof_size) {
  WriteLeUint(dst, idx, 2);
  WriteLeUint(dst + 2, nrefs, 2);
  WriteLeUint(dst + 4, 0, 4);
  WriteLeUint(dst + 8, size, sizeof_size);
}

TEST(H5HGcacheTest, ClassDescriptorMatchesExpectedDefinitions) {
  EXPECT_EQ(H5AC_GHEAP[0].id, H5AC_GHEAP_ID);
  EXPECT_STREQ(H5AC_GHEAP[0].name, "global heap");
  EXPECT_EQ(H5AC_GHEAP[0].mem_type, H5FD_MEM_GHEAP);
  EXPECT_EQ(H5AC_GHEAP[0].flags, H5AC__CLASS_SPECULATIVE_LOAD_FLAG);
  EXPECT_NE(H5AC_GHEAP[0].get_initial_load_size, nullptr);
  EXPECT_NE(H5AC_GHEAP[0].get_final_load_size, nullptr);
  EXPECT_EQ(H5AC_GHEAP[0].verify_chksum, nullptr);
  EXPECT_NE(H5AC_GHEAP[0].deserialize, nullptr);
  EXPECT_NE(H5AC_GHEAP[0].image_len, nullptr);
  EXPECT_EQ(H5AC_GHEAP[0].pre_serialize, nullptr);
  EXPECT_NE(H5AC_GHEAP[0].serialize, nullptr);
  EXPECT_EQ(H5AC_GHEAP[0].notify, nullptr);
  EXPECT_NE(H5AC_GHEAP[0].free_icr, nullptr);
  EXPECT_EQ(H5AC_GHEAP[0].fsf_size, nullptr);
}

TEST(H5HGcacheTest,
     PersistedCollectionsExerciseSpeculativeLoadAndTailFreeSpace) {
  std::vector<uint8_t> image;
  H5HG_t obj_tight{};
  H5HG_t obj_huge{};
  H5HG_t obj_small{};
  H5HG_t obj_large{};

  // 4056 bytes + 16-byte obj hdr = 4072 bytes; in a 4096-byte heap (hdr=16),
  // 8 trailing bytes remain (< H5HG_SIZEOF_OBJHDR), exercising the small-tail
  // free-space branch on deserialize.
  const std::vector<uint8_t> data_tight(4056, 0x5A);
  // > H5HG_MAXSIZE (65536), so CWFS cannot extend Heap 1 and allocates Heap 2
  // with size > H5HG_MINSIZE, exercising speculative load resizing.
  const std::vector<uint8_t> data_huge(70000, 0xA5);
  const std::vector<uint8_t> data_small(128, 0x3C);
  // > H5HG_MINSIZE, forces extension of Heap 3 to > 4096 with remaining free
  // space (idx == 0 header encoded on disk).
  const std::vector<uint8_t> data_large(6000, 0xC3);

  {
    ScopedFile file = CreateInMemoryFile("h5hgcache_persist.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    ASSERT_GE(H5HG_insert(f, data_tight.size(), data_tight.data(), &obj_tight),
              0);
    ASSERT_GE(H5HG_insert(f, data_huge.size(), data_huge.data(), &obj_huge), 0);
    ASSERT_GE(H5HG_insert(f, data_small.size(), data_small.data(), &obj_small),
              0);
    ASSERT_GE(H5HG_insert(f, data_large.size(), data_large.data(), &obj_large),
              0);
    EXPECT_NE(obj_tight.addr, obj_huge.addr);
    EXPECT_EQ(obj_small.addr, obj_large.addr);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_len = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_len, 0);
    image.resize(static_cast<size_t>(image_len));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_len);
  }

  // Reopen read-only from the serialized file image and read all four objects
  // so H5AC drives initial_load_size, final_load_size, deserialize, and
  // free_icr on close.
  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("h5hgcache_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  H5F_t* rf = GetH5File(ro_file.get());
  ASSERT_NE(rf, nullptr);

  ScopedApiContext ro_ctx;
  ASSERT_TRUE(ro_ctx.valid());

  auto verify_read = [rf](H5HG_t* hobj, const std::vector<uint8_t>& expected) {
    std::vector<uint8_t> actual(expected.size(), 0);
    size_t read_sz = 0;
    ASSERT_NE(H5HG_read(rf, hobj, actual.data(), &read_sz), nullptr);
    EXPECT_EQ(read_sz, expected.size());
    EXPECT_EQ(actual, expected);
  };
  verify_read(&obj_tight, data_tight);
  verify_read(&obj_huge, data_huge);
  verify_read(&obj_small, data_small);
  verify_read(&obj_large, data_large);
}

TEST(H5HGcacheTest, PublicVariableLengthStringsPersistAcrossReopen) {
  std::vector<uint8_t> image;
  const std::string long_str(5000, 'H');
  const char* strs[2] = {"short_gheap_str", long_str.c_str()};

  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    ScopedFile file(
        H5Fcreate("h5hgcache_vlen.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedType vls(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(vls.valid());
    ASSERT_GE(H5Tset_size(vls.get(), H5T_VARIABLE), 0);
    const hsize_t dims[1] = {2};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "strings", vls.get(), space.get(),
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(
        H5Dwrite(dset.get(), vls.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT, strs),
        0);

    dset.reset();
    space.reset();
    vls.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_len = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_len, 0);
    image.resize(static_cast<size_t>(image_len));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_len);
  }

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(
      H5Fopen("h5hgcache_vlen_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());

  ScopedDataset dset(H5Dopen2(ro_file.get(), "strings", H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedType dtype(H5Dget_type(dset.get()));
  ASSERT_TRUE(dtype.valid());
  ScopedSpace space(H5Dget_space(dset.get()));
  ASSERT_TRUE(space.valid());

  char* read_strs[2] = {nullptr, nullptr};
  absl::Cleanup reclaim = [&] {
    H5Dvlen_reclaim(dtype.get(), space.get(), H5P_DEFAULT, read_strs);
  };
  ASSERT_GE(H5Dread(dset.get(), dtype.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_strs),
            0);
  EXPECT_STREQ(read_strs[0], strs[0]);
  EXPECT_STREQ(read_strs[1], strs[1]);
}

TEST(H5HGcacheTest, GetInitialAndFinalLoadSizeValidateAndDecodeHeaders) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_sizes(fcpl.get(), 4, 4), 0);
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(
      H5Fcreate("h5hgcache_hdr.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(H5F_SIZEOF_SIZE(f), uint8_t{4});

  // 1. Initial load size is always H5HG_MINSIZE (4096).
  size_t init_len = 0;
  ASSERT_GE(H5AC_GHEAP[0].get_initial_load_size(f, &init_len), 0);
  EXPECT_EQ(init_len, size_t{H5HG_MINSIZE});

  // 2. Valid 4096-byte header decoding a 16384-byte heap.
  {
    std::vector<uint8_t> buf =
        MakeHeapImage(H5HG_MINSIZE, 16384, H5HG_VERSION, 4);
    size_t actual_len = H5HG_MINSIZE;
    ASSERT_GE(H5AC_GHEAP[0].get_final_load_size(buf.data(), buf.size(), f,
                                                &actual_len),
              0);
    EXPECT_EQ(actual_len, 16384u);
  }

  // 3. Reject truncated and corrupt headers across every H5HG__hdr_deserialize
  //    validation check.
  ScopedErrorSilence silence;
  auto expect_final_fail = [&](const std::vector<uint8_t>& buf) {
    size_t actual_len = buf.size();
    EXPECT_LT(H5AC_GHEAP[0].get_final_load_size(buf.data(), buf.size(), f,
                                                &actual_len),
              0);
  };

  // Truncated in magic (< 4 bytes).
  expect_final_fail({0x47, 0x43, 0x4f});
  // Truncated before version (len == 4).
  expect_final_fail({'G', 'C', 'O', 'L'});
  // Truncated in reserved bytes (len == 5).
  expect_final_fail({'G', 'C', 'O', 'L', H5HG_VERSION});
  // Truncated in length field (len == 9 with sizeof_size == 4).
  expect_final_fail({'G', 'C', 'O', 'L', H5HG_VERSION, 0, 0, 0, 0});

  // Bad magic in full 4096-byte buffer.
  {
    std::vector<uint8_t> bad_magic =
        MakeHeapImage(H5HG_MINSIZE, H5HG_MINSIZE, H5HG_VERSION, 4);
    std::memcpy(bad_magic.data(), "BAD!", H5_SIZEOF_MAGIC);
    expect_final_fail(bad_magic);
  }
  // Unsupported version (!= 1).
  expect_final_fail(MakeHeapImage(H5HG_MINSIZE, H5HG_MINSIZE, 2, 4));
  // Declared heap size < H5HG_MINSIZE (4096).
  expect_final_fail(MakeHeapImage(H5HG_MINSIZE, 4088, H5HG_VERSION, 4));
}

TEST(H5HGcacheTest, DeserializeImageLenSerializeAndFreeIcrDirectRoundTrip) {
  ScopedFile file = CreateInMemoryFile("h5hgcache_direct.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  const size_t sz_len = H5F_SIZEOF_SIZE(f);
  const size_t hdr_len = H5HG_SIZEOF_HDR(f);
  const size_t obj_hdr_len = H5HG_SIZEOF_OBJHDR(f);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Empty heap containing only free space object idx==0 -> nused == 1.
  {
    std::vector<uint8_t> raw =
        MakeHeapImage(H5HG_MINSIZE, H5HG_MINSIZE, H5HG_VERSION, sz_len);
    EncodeObjHdr(raw.data() + hdr_len, /*idx=*/0, /*nrefs=*/0,
                 H5HG_MINSIZE - hdr_len, sz_len);

    bool dirty = false;
    auto* heap = static_cast<H5HG_heap_t*>(
        H5AC_GHEAP[0].deserialize(raw.data(), raw.size(), f, &dirty));
    ASSERT_NE(heap, nullptr);
    heap->cache_info.type = H5AC_GHEAP;
    absl::Cleanup free_heap = [&] {
      if (heap != nullptr) {
        heap->cache_info.type = H5AC_GHEAP;
        EXPECT_GE(H5AC_GHEAP[0].free_icr(heap), 0);
      }
    };

    EXPECT_FALSE(dirty);
    EXPECT_EQ(heap->nused, 1u);
    EXPECT_EQ(heap->obj[0].size, H5HG_MINSIZE - hdr_len);

    size_t reported_len = 0;
    ASSERT_GE(H5AC_GHEAP[0].image_len(heap, &reported_len), 0);
    EXPECT_EQ(reported_len, size_t{H5HG_MINSIZE});

    std::vector<uint8_t> out(H5HG_MINSIZE, 0xEE);
    ASSERT_GE(H5AC_GHEAP[0].serialize(f, out.data(), out.size(), heap), 0);
    EXPECT_EQ(out, raw);

    ASSERT_GE(H5AC_GHEAP[0].free_icr(heap), 0);
    heap = nullptr;
  }

  // 2. Non-contiguous high object index (idx == 300 > initial nalloc == 257)
  //    forces H5HG_obj_t array reallocation during deserialize.
  {
    std::vector<uint8_t> raw =
        MakeHeapImage(H5HG_MINSIZE, H5HG_MINSIZE, H5HG_VERSION, sz_len);
    const uint16_t kHighIdx = 300;
    const size_t obj_data_len = 8;
    const size_t obj_total = obj_hdr_len + obj_data_len;
    EncodeObjHdr(raw.data() + hdr_len, kHighIdx, /*nrefs=*/7, obj_data_len,
                 sz_len);
    for (size_t i = 0; i < obj_data_len; ++i) {
      raw[hdr_len + obj_hdr_len + i] = static_cast<uint8_t>(0x80 + i);
    }
    const size_t free_size = H5HG_MINSIZE - hdr_len - obj_total;
    EncodeObjHdr(raw.data() + hdr_len + obj_total, /*idx=*/0, /*nrefs=*/0,
                 free_size, sz_len);

    bool dirty = false;
    auto* heap = static_cast<H5HG_heap_t*>(
        H5AC_GHEAP[0].deserialize(raw.data(), raw.size(), f, &dirty));
    ASSERT_NE(heap, nullptr);
    heap->cache_info.type = H5AC_GHEAP;
    absl::Cleanup free_heap = [&] {
      if (heap != nullptr) {
        heap->cache_info.type = H5AC_GHEAP;
        EXPECT_GE(H5AC_GHEAP[0].free_icr(heap), 0);
      }
    };

    EXPECT_FALSE(dirty);
    EXPECT_GT(heap->nalloc, kHighIdx);
    EXPECT_EQ(heap->nused, static_cast<size_t>(kHighIdx + 1));
    EXPECT_EQ(heap->obj[kHighIdx].nrefs, 7);
    EXPECT_EQ(heap->obj[kHighIdx].size, obj_data_len);
    ASSERT_NE(heap->obj[kHighIdx].begin, nullptr);
    EXPECT_THAT(
        std::vector<uint8_t>(heap->obj[kHighIdx].begin + obj_hdr_len,
                             heap->obj[kHighIdx].begin + obj_hdr_len + 4),
        ElementsAre(0x80, 0x81, 0x82, 0x83));
    EXPECT_EQ(heap->obj[0].size, free_size);

    std::vector<uint8_t> out(H5HG_MINSIZE, 0);
    ASSERT_GE(H5AC_GHEAP[0].serialize(f, out.data(), out.size(), heap), 0);
    EXPECT_EQ(out, raw);

    ASSERT_GE(H5AC_GHEAP[0].free_icr(heap), 0);
    heap = nullptr;
  }
}

TEST(H5HGcacheTest, DeserializeRejectsTruncatedAndCorruptBuffers) {
  ScopedFile file = CreateInMemoryFile("h5hgcache_corrupt.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  const size_t sz_len = H5F_SIZEOF_SIZE(f);
  const size_t hdr_len = H5HG_SIZEOF_HDR(f);
  const size_t obj_hdr_len = H5HG_SIZEOF_OBJHDR(f);
  ASSERT_EQ(hdr_len, 16u);
  ASSERT_EQ(obj_hdr_len, 16u);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;
  bool dirty = false;

  // 1. Buffer shorter than H5HG_SIZEOF_HDR(f).
  {
    std::vector<uint8_t> short_buf(8, 0);
    EXPECT_EQ(H5AC_GHEAP[0].deserialize(short_buf.data(), short_buf.size(), f,
                                        &dirty),
              nullptr);
  }

  // 2. Bad magic in full buffer -> H5HG__hdr_deserialize fails.
  {
    std::vector<uint8_t> bad =
        MakeHeapImage(H5HG_MINSIZE, H5HG_MINSIZE, H5HG_VERSION, sz_len);
    std::memcpy(bad.data(), "NOPE", H5_SIZEOF_MAGIC);
    EXPECT_EQ(H5AC_GHEAP[0].deserialize(bad.data(), bad.size(), f, &dirty),
              nullptr);
  }

  // 3. Duplicate object 0: explicit idx==0 object followed by 8 trailing bytes
  //    (< obj_hdr_len) hits "object 0 should not be set".
  {
    std::vector<uint8_t> dup0 =
        MakeHeapImage(H5HG_MINSIZE, H5HG_MINSIZE, H5HG_VERSION, sz_len);
    EncodeObjHdr(dup0.data() + hdr_len, /*idx=*/0, /*nrefs=*/0,
                 H5HG_MINSIZE - hdr_len - 8, sz_len);
    EXPECT_EQ(H5AC_GHEAP[0].deserialize(dup0.data(), dup0.size(), f, &dirty),
              nullptr);
  }

  // 4. Declared size 8192 while buffer len is 4096 -> overflow at idx
  // (p==4096).
  {
    std::vector<uint8_t> buf =
        MakeHeapImage(H5HG_MINSIZE, 8192, H5HG_VERSION, sz_len);
    EncodeObjHdr(buf.data() + hdr_len, /*idx=*/1, /*nrefs=*/1,
                 H5HG_MINSIZE - hdr_len - obj_hdr_len, sz_len);
    EXPECT_EQ(H5AC_GHEAP[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 5-7. Declared size 8192 with an entry starting at offset 4088 and
  //      truncated buffers hitting nrefs, reserved, and object-length checks.
  auto make_truncated_entry_buf = [&](size_t actual_buf_len) {
    std::vector<uint8_t> buf =
        MakeHeapImage(actual_buf_len, 8192, H5HG_VERSION, sz_len);
    // First object fills [16..4088): data size 4056 -> need 4072.
    EncodeObjHdr(buf.data() + hdr_len, /*idx=*/1, /*nrefs=*/1, 4056, sz_len);
    // Start second entry at 4088 with idx=2 (bytes 4088..4089).
    WriteLeUint(buf.data() + 4088, 2, 2);
    return buf;
  };
  {
    // Truncated in nrefs (4091 bytes: 4088..4089 idx ok, 4090..4091 overflows).
    auto b_nrefs = make_truncated_entry_buf(4091);
    EXPECT_EQ(
        H5AC_GHEAP[0].deserialize(b_nrefs.data(), b_nrefs.size(), f, &dirty),
        nullptr);

    // Truncated in reserved (4094 bytes: idx + nrefs ok, 4 reserved overflows).
    auto b_res = make_truncated_entry_buf(4094);
    EXPECT_EQ(H5AC_GHEAP[0].deserialize(b_res.data(), b_res.size(), f, &dirty),
              nullptr);

    // Truncated in size (4096 bytes: idx + nrefs + reserved ok, 8-byte len
    // overflows).
    auto b_len = make_truncated_entry_buf(4096);
    EXPECT_EQ(H5AC_GHEAP[0].deserialize(b_len.data(), b_len.size(), f, &dirty),
              nullptr);
  }

  // 8. Declared size 8192, len 4096, object declares size 5000 -> payload
  //    overflows input buffer.
  {
    std::vector<uint8_t> buf =
        MakeHeapImage(H5HG_MINSIZE, 8192, H5HG_VERSION, sz_len);
    EncodeObjHdr(buf.data() + hdr_len, /*idx=*/1, /*nrefs=*/1, 5000, sz_len);
    EXPECT_EQ(H5AC_GHEAP[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 9. Buffer len 8192, declared heap size 4096, object advances p to 4104
  //    (!= chunk + 4096) -> "partially decoded global heap".
  {
    std::vector<uint8_t> buf =
        MakeHeapImage(8192, H5HG_MINSIZE, H5HG_VERSION, sz_len);
    EncodeObjHdr(buf.data() + hdr_len, /*idx=*/1, /*nrefs=*/1, 4072, sz_len);
    EXPECT_EQ(H5AC_GHEAP[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 10. Unaligned declared heap size (4097) leaves 9 unaligned tail bytes ->
  //     "decoded global heap is not aligned".
  {
    std::vector<uint8_t> buf = MakeHeapImage(4097, 4097, H5HG_VERSION, sz_len);
    EncodeObjHdr(buf.data() + hdr_len, /*idx=*/1, /*nrefs=*/1, 4056, sz_len);
    EXPECT_EQ(H5AC_GHEAP[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }
}

}  // namespace
}  // namespace hdf5
