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
// Tests symbol table entry encode/decode, copy/reset, conversion to H5O_link_t,
// and debug output (third_party/hdf5/src/H5Gent.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gent.c

#ifndef NDEBUG
#define H5GENT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5G_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try h5g_test_try_param
#include "third_party/hdf5/src/H5Gpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5HLprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5GENT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GENT_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

H5F_t* UnwrapFile(const ScopedFile& file) {
  return static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
}

TEST(H5GentTest, EncodeAndDecodeSingleAndVectorRoundTrip) {
  // Use 4-byte sizes/addresses so CACHED_STAB takes 8 of 16 scratch bytes and
  // exercises zero-padding of remaining bytes, plus default 8-byte file.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_sizes(fcpl.get(), 4, 4), 0);
  ScopedFile small_file =
      CreateInMemoryFile("ent_small.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(small_file.valid());
  H5F_t* f_small = UnwrapFile(small_file);
  ASSERT_NE(f_small, nullptr);

  ScopedFile std_file = CreateInMemoryFile("ent_std.h5");
  ASSERT_TRUE(std_file.valid());
  H5F_t* f_std = UnwrapFile(std_file);
  ASSERT_NE(f_std, nullptr);

  std::array<H5G_entry_t, 3> src{};
  src[0].type = H5G_NOTHING_CACHED;
  src[0].name_off = 12;
  src[0].header = 0x1020;

  src[1].type = H5G_CACHED_STAB;
  src[1].name_off = 34;
  src[1].header = 0x3040;
  src[1].cache.stab.btree_addr = 0x5060;
  src[1].cache.stab.heap_addr = 0x7080;

  src[2].type = H5G_CACHED_SLINK;
  src[2].name_off = 56;
  src[2].header = HADDR_UNDEF;
  src[2].cache.slink.lval_offset = 99;

  for (H5F_t* f : {f_small, f_std}) {
    const size_t entry_sz = H5G_SIZEOF_ENTRY_FILE(f);
    std::vector<uint8_t> buf(entry_sz * 4, 0xAA);

    uint8_t* enc_p = buf.data();
    ASSERT_GE(H5G__ent_encode_vec(f, &enc_p, src.data(), 3), 0);
    // Also exercise ent == nullptr branch in H5G_ent_encode.
    ASSERT_GE(H5G_ent_encode(f, &enc_p, nullptr), 0);
    EXPECT_EQ(enc_p, buf.data() + entry_sz * 4);

    // Zero-initialize dst before decoding (MSan-safe for line 121 check).
    std::array<H5G_entry_t, 3> dst{};
    const uint8_t* dec_p = buf.data();
    const uint8_t* p_end = buf.data() + buf.size() - 1;
    ASSERT_GE(H5G__ent_decode_vec(f, &dec_p, p_end, dst.data(), 3), 0);

    EXPECT_EQ(dst[0].type, H5G_NOTHING_CACHED);
    EXPECT_EQ(dst[0].name_off, 12u);
    EXPECT_EQ(dst[0].header, 0x1020u);

    EXPECT_EQ(dst[1].type, H5G_CACHED_STAB);
    EXPECT_EQ(dst[1].cache.stab.btree_addr, 0x5060u);
    EXPECT_EQ(dst[1].cache.stab.heap_addr, 0x7080u);

    EXPECT_EQ(dst[2].type, H5G_CACHED_SLINK);
    EXPECT_EQ(dst[2].header, HADDR_UNDEF);
    EXPECT_EQ(dst[2].cache.slink.lval_offset, 99u);

    H5G_entry_t null_dst{};
    ASSERT_GE(H5G_ent_decode(f, &dec_p, &null_dst, p_end), 0);
    EXPECT_EQ(null_dst.type, H5G_NOTHING_CACHED);
    EXPECT_EQ(null_dst.name_off, 0u);
    EXPECT_EQ(null_dst.header, HADDR_UNDEF);
  }
}

TEST(H5GentTest, EncodeDecodeErrorsAndOverflowGuards) {
  ScopedFile file = CreateInMemoryFile("ent_err.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file);
  ASSERT_NE(f, nullptr);

  const size_t entry_sz = H5G_SIZEOF_ENTRY_FILE(f);
  ASSERT_EQ(entry_sz, 40u);
  std::vector<uint8_t> buf(entry_sz * 2, 0);

  // Invalid cache types fail encode and encode_vec.
  ScopedErrorSilence silence;
  for (H5G_cache_type_t bad_type :
       {H5G_CACHED_ERROR, H5G_NCACHED, static_cast<H5G_cache_type_t>(-2)}) {
    H5G_entry_t bad{};
    bad.type = bad_type;
    bad.header = 0x100;
    uint8_t* p = buf.data();
    EXPECT_LT(H5G_ent_encode(f, &p, &bad), 0);
    p = buf.data();
    EXPECT_LT(H5G__ent_encode_vec(f, &p, &bad, 1), 0);
  }

  // Decode overflow at line 121 (when caller supplies non-zero ent.name_off).
  {
    H5G_entry_t ent{};
    ent.name_off = 100;
    const uint8_t* p = buf.data();
    EXPECT_LT(H5G_ent_decode(f, &p, &ent, buf.data() + 10), 0);
  }

  // Decode overflow at line 127 (truncated after 8-byte name_off).
  {
    H5G_entry_t ent{};
    const uint8_t* p = buf.data();
    EXPECT_LT(H5G_ent_decode(f, &p, &ent, buf.data() + 12), 0);
  }

  // Decode overflow at line 134 (truncated right after reserved bytes = 24B).
  {
    H5G_entry_t ent{};
    const uint8_t* p = buf.data();
    EXPECT_LT(H5G_ent_decode(f, &p, &ent, buf.data() + 23), 0);
  }

  // Encode a CACHED_STAB and a CACHED_SLINK, then decode with truncated p_end.
  {
    H5G_entry_t stab{};
    stab.type = H5G_CACHED_STAB;
    stab.header = 0x100;
    uint8_t* ep = buf.data();
    ASSERT_GE(H5G_ent_encode(f, &ep, &stab), 0);

    H5G_entry_t ent{};
    const uint8_t* dp = buf.data();
    EXPECT_LT(H5G_ent_decode(f, &dp, &ent, buf.data() + 30), 0);
  }
  {
    H5G_entry_t slink{};
    slink.type = H5G_CACHED_SLINK;
    slink.header = HADDR_UNDEF;
    slink.cache.slink.lval_offset = 7;
    uint8_t* ep = buf.data();
    ASSERT_GE(H5G_ent_encode(f, &ep, &slink), 0);

    H5G_entry_t ent{};
    const uint8_t* dp = buf.data();
    EXPECT_LT(H5G_ent_decode(f, &dp, &ent, buf.data() + 26), 0);
  }

  // Decode unknown cache type from wire bytes.
  {
    std::array<uint8_t, 40> bad_wire{};
    // With 8-byte size + 8-byte addr, cache_type UINT32 sits at offset 16.
    bad_wire[16] = static_cast<uint8_t>(H5G_NCACHED);
    H5G_entry_t ent{};
    const uint8_t* dp = bad_wire.data();
    EXPECT_LT(H5G_ent_decode(f, &dp, &ent, bad_wire.data() + 39), 0);
    dp = bad_wire.data();
    EXPECT_LT(H5G__ent_decode_vec(f, &dp, bad_wire.data() + 39, &ent, 1), 0);
  }

  // decode_vec ran past end of image buffer (*pp > p_end).
  {
    H5G_entry_t ent{};
    const uint8_t* dp = buf.data() + 5;
    EXPECT_LT(H5G__ent_decode_vec(f, &dp, buf.data() + 3, &ent, 1), 0);
  }
}

TEST(H5GentTest, CopyAndResetBehavior) {
  H5G_entry_t src{};
  src.type = H5G_CACHED_STAB;
  src.name_off = 42;
  src.header = 0xABCD;
  src.cache.stab.btree_addr = 0x111;
  src.cache.stab.heap_addr = 0x222;

  H5G_entry_t dst_deep{};
  H5G__ent_copy(&dst_deep, &src, H5_COPY_DEEP);
  EXPECT_EQ(dst_deep.type, H5G_CACHED_STAB);
  EXPECT_EQ(dst_deep.name_off, 42u);
  EXPECT_EQ(dst_deep.header, 0xABCDu);
  // Source remains intact on deep copy.
  EXPECT_EQ(src.header, 0xABCDu);

  H5G_entry_t dst_shallow{};
  H5G__ent_copy(&dst_shallow, &src, H5_COPY_SHALLOW);
  EXPECT_EQ(dst_shallow.header, 0xABCDu);
  // Source is reset on shallow copy.
  EXPECT_EQ(src.type, H5G_NOTHING_CACHED);
  EXPECT_EQ(src.name_off, 0u);
  EXPECT_EQ(src.header, HADDR_UNDEF);
}

TEST(H5GentTest, EntToLinkAndDebugWithLocalHeap) {
  ScopedFile file = CreateInMemoryFile("ent_heap.h5");
  ASSERT_TRUE(file.valid());
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_ctx = [] { H5CX_pop(false); };
  H5F_t* f = UnwrapFile(file);
  ASSERT_NE(f, nullptr);

  haddr_t heap_addr = HADDR_UNDEF;
  ASSERT_GE(H5HL_create(f, 256, &heap_addr), 0);
  H5HL_t* heap = H5HL_protect(f, heap_addr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(heap, nullptr);
  absl::Cleanup unprotect_heap = [&] { H5HL_unprotect(heap); };

  size_t name_off = 0;
  size_t empty_off = 0;
  size_t val_off = 0;
  constexpr char kLinkName[] = "target_link";
  constexpr char kEmptyName[] = "";
  constexpr char kSoftVal[] = "/dest/obj";
  ASSERT_GE(H5HL_insert(f, heap, sizeof(kLinkName), kLinkName, &name_off), 0);
  ASSERT_GE(H5HL_insert(f, heap, sizeof(kEmptyName), kEmptyName, &empty_off),
            0);
  ASSERT_GE(H5HL_insert(f, heap, sizeof(kSoftVal), kSoftVal, &val_off), 0);

  // 1. Hard link conversion.
  H5G_entry_t hard_ent{};
  hard_ent.type = H5G_NOTHING_CACHED;
  hard_ent.name_off = name_off;
  hard_ent.header = 0x4000;
  H5O_link_t lnk{};
  ASSERT_GE(H5G__ent_to_link(&hard_ent, heap, &lnk), 0);
  EXPECT_EQ(lnk.type, H5L_TYPE_HARD);
  EXPECT_STREQ(lnk.name, kLinkName);
  EXPECT_EQ(lnk.u.hard.addr, 0x4000u);
  ASSERT_GE(H5O_msg_reset(H5O_LINK_ID, &lnk), 0);

  // 2. Soft link conversion.
  H5G_entry_t slink_ent{};
  slink_ent.type = H5G_CACHED_SLINK;
  slink_ent.name_off = name_off;
  slink_ent.header = HADDR_UNDEF;
  slink_ent.cache.slink.lval_offset = val_off;
  ASSERT_GE(H5G__ent_to_link(&slink_ent, heap, &lnk), 0);
  EXPECT_EQ(lnk.type, H5L_TYPE_SOFT);
  EXPECT_STREQ(lnk.name, kLinkName);
  EXPECT_STREQ(lnk.u.soft.name, kSoftVal);
  ASSERT_GE(H5O_msg_reset(H5O_LINK_ID, &lnk), 0);

  // 3. Conversion failure paths (invalid offsets, empty name).
  {
    ScopedErrorSilence silence;
    H5G_entry_t bad_off = hard_ent;
    bad_off.name_off = 99999;
    EXPECT_LT(H5G__ent_to_link(&bad_off, heap, &lnk), 0);

    H5G_entry_t empty_ent = hard_ent;
    empty_ent.name_off = empty_off;
    EXPECT_LT(H5G__ent_to_link(&empty_ent, heap, &lnk), 0);

    H5G_entry_t bad_slink = slink_ent;
    bad_slink.cache.slink.lval_offset = 99999;
    EXPECT_LT(H5G__ent_to_link(&bad_slink, heap, &lnk), 0);
  }

  // 4. H5G__ent_debug across all cache types and heap edge cases.
  char* mem_buf = nullptr;
  size_t mem_sz = 0;
  FILE* stream = open_memstream(&mem_buf, &mem_sz);
  ASSERT_NE(stream, nullptr);
  absl::Cleanup cleanup_stream = [&] {
    if (stream != nullptr) {
      std::fclose(stream);
    }
    std::free(mem_buf);
  };

  H5G_entry_t stab_ent{};
  stab_ent.type = H5G_CACHED_STAB;
  stab_ent.name_off = name_off;
  stab_ent.header = 0x100;
  stab_ent.cache.stab.btree_addr = 0x200;
  stab_ent.cache.stab.heap_addr = 0x300;

  H5G_entry_t bad_slink = slink_ent;
  bad_slink.cache.slink.lval_offset = 99999;

  H5G_entry_t err_ent{};
  err_ent.type = H5G_CACHED_ERROR;
  err_ent.header = HADDR_UNDEF;

  EXPECT_GE(H5G__ent_debug(&hard_ent, stream, 0, 20, heap), 0);
  EXPECT_GE(H5G__ent_debug(&stab_ent, stream, 2, 24, heap), 0);
  EXPECT_GE(H5G__ent_debug(&slink_ent, stream, 0, 20, heap), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_GE(H5G__ent_debug(&bad_slink, stream, 0, 20, heap), 0);
  }
  EXPECT_GE(H5G__ent_debug(&slink_ent, stream, 0, 20, nullptr), 0);
  EXPECT_GE(H5G__ent_debug(&err_ent, stream, 0, 20, heap), 0);

  std::fclose(stream);
  stream = nullptr;
  std::string output(mem_buf, mem_sz);
  EXPECT_THAT(output, HasSubstr("Nothing Cached"));
  EXPECT_THAT(output, HasSubstr("Symbol Table"));
  EXPECT_THAT(output, HasSubstr("Symbolic Link"));
  EXPECT_THAT(output, HasSubstr(kSoftVal));
  EXPECT_THAT(output, HasSubstr("Invalid heap address given"));
  EXPECT_THAT(output, HasSubstr("Unknown symbol type"));
}

}  // namespace
}  // namespace hdf5
