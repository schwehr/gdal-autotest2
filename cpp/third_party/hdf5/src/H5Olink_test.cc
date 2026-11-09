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
// Tests link object header messages: encode/decode across name-length and flag
// variants, malformed decode rejection, native copy/reset/free, hard/soft/
// external/user-defined link deletion, H5Ocopy shallow/deep/failure hooks,
// and debug output (third_party/hdf5/src/H5Olink.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Olink.c

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields in cache headers and
// object headers pulled in via `H5Opkg.h`.
#ifndef NDEBUG
#define H5O_LINK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5L_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Lpkg.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5O_LINK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5O_LINK_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

constexpr H5L_type_t kCustomUdTypeSucceed = static_cast<H5L_type_t>(65);
constexpr H5L_type_t kCustomUdTypeFail = static_cast<H5L_type_t>(66);
constexpr H5L_type_t kCustomUdTypeNoDel = static_cast<H5L_type_t>(67);
constexpr H5L_type_t kUnregisteredUdType = static_cast<H5L_type_t>(68);
constexpr H5L_type_t kUnknownBuiltinType = static_cast<H5L_type_t>(2);

int g_ud_delete_calls = 0;

herr_t UdDeleteSucceed(const char* /*link_name*/, hid_t /*file*/,
                       const void* /*lnkdata*/, size_t /*lnkdata_size*/) {
  ++g_ud_delete_calls;
  return 0;
}

herr_t UdDeleteFail(const char* /*link_name*/, hid_t /*file*/,
                    const void* /*lnkdata*/, size_t /*lnkdata_size*/) {
  ++g_ud_delete_calls;
  return -1;
}

hid_t UdTraverseNoop(const char* /*link_name*/, hid_t /*cur_group*/,
                     const void* /*lnkdata*/, size_t /*lnkdata_size*/,
                     hid_t /*lapl_id*/, hid_t /*dxpl_id*/) {
  return -1;
}

herr_t UdCopySucceed(const char* /*new_name*/, hid_t /*new_loc*/,
                     const void* /*lnkdata*/, size_t /*lnkdata_size*/) {
  return 0;
}

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__COPIED_TAG) {
    std::memset(&node_, 0, sizeof(node_));
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
  H5CX_node_t node_;
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

template <typename T>
T ZeroStruct() {
  T val;
  std::memset(&val, 0, sizeof(val));
  return val;
}

H5F_t* GetNativeFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void AppendUint16Le(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void AppendUint32Le(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

void AppendUint64Le(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

std::string ReadWholeFile(const TempFile& file) {
  std::FILE* fp = std::fopen(file.c_str(), "rb");
  if (fp == nullptr) {
    return "";
  }
  absl::Cleanup close_fp = [fp] { std::fclose(fp); };
  std::string out;
  char buf[512];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
    out.append(buf, n);
  }
  return out;
}

TEST(H5OlinkTest, MessageClassMetadataMatches) {
  EXPECT_EQ(H5O_MSG_LINK[0].id, H5O_LINK_ID);
  EXPECT_STREQ(H5O_MSG_LINK[0].name, "link");
  EXPECT_EQ(H5O_MSG_LINK[0].native_size, sizeof(H5O_link_t));
}

TEST(H5OlinkTest, EncodeDecodeCopySizeAndResetRoundTrips) {
  ScopedFile file = CreateInMemoryFile("link_roundtrip.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1-byte name, 2-byte name (> 255), and 4-byte name (> 65535).
  const std::string name_short = "alpha";
  const std::string name_2byte(300, 'b');
  const std::string name_4byte(66000, 'c');

  std::string soft_target = "/target/dataset";
  std::string ext_payload("other.h5\0/ext_dset", 18);
  std::vector<uint8_t> ud_payload = {0x11, 0x22, 0x33, 0x44, 0x55};

  struct Case {
    H5L_type_t type;
    bool corder_valid;
    int64_t corder;
    H5T_cset_t cset;
    const std::string* name;
    haddr_t hard_addr;
    const std::string* soft_name;
    const void* udata;
    size_t ud_size;
  };
  const Case cases[] = {
      {H5L_TYPE_HARD, false, 0, H5T_CSET_ASCII, &name_short, 0x1200, nullptr,
       nullptr, 0},
      {H5L_TYPE_SOFT, true, 42, H5T_CSET_UTF8, &name_2byte, HADDR_UNDEF,
       &soft_target, nullptr, 0},
      {H5L_TYPE_EXTERNAL, true, -7, H5T_CSET_UTF8, &name_4byte, HADDR_UNDEF,
       nullptr, ext_payload.data(), ext_payload.size()},
      {kCustomUdTypeSucceed, false, 0, H5T_CSET_ASCII, &name_short, HADDR_UNDEF,
       nullptr, ud_payload.data(), ud_payload.size()},
      {kCustomUdTypeSucceed, true, 99, H5T_CSET_ASCII, &name_short, HADDR_UNDEF,
       nullptr, nullptr, 0},
  };

  for (const auto& tc : cases) {
    H5O_link_t src = ZeroStruct<H5O_link_t>();
    src.type = tc.type;
    src.corder_valid = tc.corder_valid;
    src.corder = tc.corder;
    src.cset = tc.cset;
    src.name = const_cast<char*>(tc.name->c_str());
    if (tc.type == H5L_TYPE_HARD) {
      src.u.hard.addr = tc.hard_addr;
    } else if (tc.type == H5L_TYPE_SOFT) {
      src.u.soft.name = const_cast<char*>(tc.soft_name->c_str());
    } else {
      src.u.ud.udata = const_cast<void*>(tc.udata);
      src.u.ud.size = tc.ud_size;
    }

    const size_t raw_size = H5O_msg_raw_size(f, H5O_LINK_ID, false, &src);
    ASSERT_GT(raw_size, 0u);

    std::vector<uint8_t> encoded(raw_size, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_LINK_ID, false, encoded.data(), &src), 0);

    H5O_link_t* decoded = static_cast<H5O_link_t*>(
        H5O_msg_decode(f, nullptr, H5O_LINK_ID, encoded.size(), encoded.data()));
    ASSERT_NE(decoded, nullptr);
    absl::Cleanup free_decoded = [decoded] {
      H5O_msg_free(H5O_LINK_ID, decoded);
    };

    EXPECT_EQ(decoded->type, tc.type);
    EXPECT_EQ(decoded->corder_valid, tc.corder_valid);
    EXPECT_EQ(decoded->corder, tc.corder_valid ? tc.corder : 0);
    EXPECT_EQ(decoded->cset, tc.cset);
    EXPECT_STREQ(decoded->name, tc.name->c_str());
    if (tc.type == H5L_TYPE_HARD) {
      EXPECT_EQ(decoded->u.hard.addr, tc.hard_addr);
    } else if (tc.type == H5L_TYPE_SOFT) {
      EXPECT_STREQ(decoded->u.soft.name, tc.soft_name->c_str());
    } else {
      EXPECT_EQ(decoded->u.ud.size, tc.ud_size);
      if (tc.ud_size > 0) {
        ASSERT_NE(decoded->u.ud.udata, nullptr);
        EXPECT_EQ(std::memcmp(decoded->u.ud.udata, tc.udata, tc.ud_size), 0);
      } else {
        EXPECT_EQ(decoded->u.ud.udata, nullptr);
      }
    }

    H5O_link_t* copied_alloc =
        static_cast<H5O_link_t*>(H5O_msg_copy(H5O_LINK_ID, decoded, nullptr));
    ASSERT_NE(copied_alloc, nullptr);
    absl::Cleanup free_copied_alloc = [copied_alloc] {
      H5O_msg_free(H5O_LINK_ID, copied_alloc);
    };
    EXPECT_STREQ(copied_alloc->name, decoded->name);
    EXPECT_NE(copied_alloc->name, decoded->name);

    H5O_link_t copied_inplace = ZeroStruct<H5O_link_t>();
    ASSERT_EQ(H5O_msg_copy(H5O_LINK_ID, decoded, &copied_inplace),
              &copied_inplace);
    absl::Cleanup reset_copied_inplace = [&copied_inplace] {
      H5O_msg_reset(H5O_LINK_ID, &copied_inplace);
    };
    EXPECT_STREQ(copied_inplace.name, decoded->name);
  }

  // Null reset is a clean no-op.
  EXPECT_GE(H5O_msg_reset(H5O_LINK_ID, nullptr), 0);

  // Decode an explicit 8-byte name-length field (flags & 0x03 == 3) without
  // allocating >4 GiB in RAM.
  std::vector<uint8_t> raw_name8;
  raw_name8.push_back(1);     // version
  raw_name8.push_back(0x03);  // 8-byte name length
  AppendUint64Le(raw_name8, 4);
  raw_name8.insert(raw_name8.end(), {'n', 'a', 'm', 'e'});
  AppendUint64Le(raw_name8, 0x2000);
  H5O_link_t* decoded_name8 = static_cast<H5O_link_t*>(
      H5O_msg_decode(f, nullptr, H5O_LINK_ID, raw_name8.size(), raw_name8.data()));
  ASSERT_NE(decoded_name8, nullptr);
  absl::Cleanup free_name8 = [decoded_name8] {
    H5O_msg_free(H5O_LINK_ID, decoded_name8);
  };
  EXPECT_STREQ(decoded_name8->name, "name");
  EXPECT_EQ(decoded_name8->u.hard.addr, 0x2000u);
}

TEST(H5OlinkTest, DecodeRejectsMalformedAndTruncatedBuffers) {
  ScopedFile file = CreateInMemoryFile("link_bad_decode.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // 0-byte buffer: pass in-bounds end sentinel so p_end = buf + 0 - 1 is valid.
  uint8_t tiny[2] = {0, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, 0, tiny + 1), nullptr);

  // Bad version number.
  const std::vector<uint8_t> bad_version = {2, 0, 1, 'a'};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, bad_version.size(),
                           bad_version.data()),
            nullptr);

  // Truncated before flags.
  const std::vector<uint8_t> no_flags = {1};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, no_flags.size(),
                           no_flags.data()),
            nullptr);

  // Unsupported flag bits.
  const std::vector<uint8_t> bad_flags = {1, 0x20, 1, 'a'};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, bad_flags.size(),
                           bad_flags.data()),
            nullptr);

  // Store-link-type set, truncated before link type byte.
  const std::vector<uint8_t> trunc_type = {1, 0x08};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_type.size(),
                           trunc_type.data()),
            nullptr);

  // Store-corder set, truncated 8-byte creation order.
  const std::vector<uint8_t> trunc_corder = {1, 0x04, 1, 2, 3, 4};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_corder.size(),
                           trunc_corder.data()),
            nullptr);

  // Store-cset set, truncated before cset byte.
  const std::vector<uint8_t> trunc_cset = {1, 0x10};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_cset.size(),
                           trunc_cset.data()),
            nullptr);

  // Invalid character set value (> H5T_CSET_UTF8).
  const std::vector<uint8_t> bad_cset = {
      1, 0x10, static_cast<uint8_t>(H5T_CSET_RESERVED_2), 1, 'a'};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, bad_cset.size(),
                           bad_cset.data()),
            nullptr);

  // Truncated 1/2/4/8-byte name lengths.
  const std::vector<uint8_t> trunc_len1 = {1, 0x00};
  const std::vector<uint8_t> trunc_len2 = {1, 0x01, 1};
  const std::vector<uint8_t> trunc_len4 = {1, 0x02, 1, 0};
  const std::vector<uint8_t> trunc_len8 = {1, 0x03, 1, 0, 0, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_len1.size(),
                           trunc_len1.data()),
            nullptr);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_len2.size(),
                           trunc_len2.data()),
            nullptr);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_len4.size(),
                           trunc_len4.data()),
            nullptr);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_len8.size(),
                           trunc_len8.data()),
            nullptr);

  // Zero-length name.
  const std::vector<uint8_t> zero_name = {1, 0x00, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, zero_name.size(),
                           zero_name.data()),
            nullptr);

  // Truncated link name bytes.
  const std::vector<uint8_t> trunc_name = {1, 0x00, 5, 'a', 'b'};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_name.size(),
                           trunc_name.data()),
            nullptr);

  // Hard link truncated before target object header address.
  const std::vector<uint8_t> trunc_hard = {1, 0x00, 1, 'a', 0, 1};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_hard.size(),
                           trunc_hard.data()),
            nullptr);

  // Soft link: truncated 2-byte length, zero length, truncated target value.
  const std::vector<uint8_t> trunc_soft_len = {1, 0x08, H5L_TYPE_SOFT, 1, 's',
                                               1};
  const std::vector<uint8_t> zero_soft_len = {1, 0x08, H5L_TYPE_SOFT, 1, 's',
                                              0, 0};
  const std::vector<uint8_t> trunc_soft_val = {1, 0x08, H5L_TYPE_SOFT, 1, 's',
                                               4, 0, 'x'};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_soft_len.size(),
                           trunc_soft_len.data()),
            nullptr);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, zero_soft_len.size(),
                           zero_soft_len.data()),
            nullptr);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_soft_val.size(),
                           trunc_soft_val.data()),
            nullptr);

  // Unrecognized built-in link type (2..63).
  const std::vector<uint8_t> bad_type = {
      1, 0x08, static_cast<uint8_t>(kUnknownBuiltinType), 1, 'x', 0, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, bad_type.size(),
                           bad_type.data()),
            nullptr);

  // User-defined / external link: truncated 2-byte len, external len < 3,
  // truncated UD payload.
  const std::vector<uint8_t> trunc_ud_len = {
      1, 0x08, static_cast<uint8_t>(kCustomUdTypeSucceed), 1, 'u', 1};
  const std::vector<uint8_t> short_ext = {
      1, 0x08, static_cast<uint8_t>(H5L_TYPE_EXTERNAL), 1, 'e', 2, 0, 'a', 'b'};
  const std::vector<uint8_t> trunc_ud_data = {
      1, 0x08, static_cast<uint8_t>(kCustomUdTypeSucceed), 1, 'u', 5, 0, 'a'};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_ud_len.size(),
                           trunc_ud_len.data()),
            nullptr);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, short_ext.size(),
                           short_ext.data()),
            nullptr);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, trunc_ud_data.size(),
                           trunc_ud_data.data()),
            nullptr);

  // Truncated 4-byte name decode path with overflow inside 4-byte length field
  // is already checked; also validate 4-byte decoded length overflow.
  std::vector<uint8_t> overflow_name4 = {1, 0x02};
  AppendUint32Le(overflow_name4, 100);
  overflow_name4.push_back('z');
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINK_ID, overflow_name4.size(),
                           overflow_name4.data()),
            nullptr);
}

TEST(H5OlinkTest, DeleteHandlesHardSoftAndUserDefinedLinkClasses) {
  g_ud_delete_calls = 0;

  H5L_class_t cls_succeed{};
  cls_succeed.version = H5L_LINK_CLASS_T_VERS;
  cls_succeed.id = kCustomUdTypeSucceed;
  cls_succeed.comment = "ud_succeed";
  cls_succeed.trav_func = UdTraverseNoop;
  cls_succeed.del_func = UdDeleteSucceed;
  cls_succeed.copy_func = UdCopySucceed;
  ASSERT_GE(H5Lregister(&cls_succeed), 0);
  absl::Cleanup unreg_succeed = [] { H5Lunregister(kCustomUdTypeSucceed); };

  H5L_class_t cls_fail{};
  cls_fail.version = H5L_LINK_CLASS_T_VERS;
  cls_fail.id = kCustomUdTypeFail;
  cls_fail.comment = "ud_fail";
  cls_fail.trav_func = UdTraverseNoop;
  cls_fail.del_func = UdDeleteFail;
  ASSERT_GE(H5Lregister(&cls_fail), 0);
  absl::Cleanup unreg_fail = [] { H5Lunregister(kCustomUdTypeFail); };

  H5L_class_t cls_no_del{};
  cls_no_del.version = H5L_LINK_CLASS_T_VERS;
  cls_no_del.id = kCustomUdTypeNoDel;
  cls_no_del.comment = "ud_no_del";
  cls_no_del.trav_func = UdTraverseNoop;
  ASSERT_GE(H5Lregister(&cls_no_del), 0);
  absl::Cleanup unreg_no_del = [] { H5Lunregister(kCustomUdTypeNoDel); };

  ScopedFile file = CreateInMemoryFile("link_delete.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/target", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  H5O_loc_t* grp_loc = H5O_get_loc(grp.get());
  ASSERT_NE(grp_loc, nullptr);
  ASSERT_GE(H5Oincr_refcount(grp.get()), 0);

  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Valid hard link deletion decrements target refcount cleanly.
  H5O_link_t hard_ok = ZeroStruct<H5O_link_t>();
  hard_ok.type = H5L_TYPE_HARD;
  hard_ok.name = const_cast<char*>("hard_ok");
  hard_ok.u.hard.addr = grp_loc->addr;
  EXPECT_GE(H5O_link_delete(f, nullptr, &hard_ok), 0);

  // Soft link deletion is a no-op success.
  H5O_link_t soft_ok = ZeroStruct<H5O_link_t>();
  soft_ok.type = H5L_TYPE_SOFT;
  soft_ok.name = const_cast<char*>("soft_ok");
  soft_ok.u.soft.name = const_cast<char*>("/target");
  EXPECT_GE(H5O_link_delete(f, nullptr, &soft_ok), 0);

  // UD link without del_func and with succeeding del_func.
  char payload[] = "abc";
  H5O_link_t ud_no_del = ZeroStruct<H5O_link_t>();
  ud_no_del.type = kCustomUdTypeNoDel;
  ud_no_del.name = const_cast<char*>("ud_no_del");
  ud_no_del.u.ud.udata = payload;
  ud_no_del.u.ud.size = sizeof(payload);
  EXPECT_GE(H5O_link_delete(f, nullptr, &ud_no_del), 0);

  H5O_link_t ud_ok = ZeroStruct<H5O_link_t>();
  ud_ok.type = kCustomUdTypeSucceed;
  ud_ok.name = const_cast<char*>("ud_ok");
  ud_ok.u.ud.udata = payload;
  ud_ok.u.ud.size = sizeof(payload);
  EXPECT_GE(H5O_link_delete(f, nullptr, &ud_ok), 0);
  EXPECT_EQ(g_ud_delete_calls, 1);

  ScopedErrorSilence silence;

  // Hard link pointing beyond EOA fails cleanly in H5O_link.
  H5O_link_t hard_bad = ZeroStruct<H5O_link_t>();
  hard_bad.type = H5L_TYPE_HARD;
  hard_bad.name = const_cast<char*>("hard_bad");
  hard_bad.u.hard.addr = 0xdeadbeef00ULL;
  EXPECT_LT(H5O_link_delete(f, nullptr, &hard_bad), 0);

  // Unregistered UD link type fails with H5E_NOTREGISTERED.
  H5O_link_t ud_unreg = ZeroStruct<H5O_link_t>();
  ud_unreg.type = kUnregisteredUdType;
  ud_unreg.name = const_cast<char*>("ud_unreg");
  EXPECT_LT(H5O_link_delete(f, nullptr, &ud_unreg), 0);

  // Registered UD link class whose del_func returns FAIL.
  H5O_link_t ud_fail = ZeroStruct<H5O_link_t>();
  ud_fail.type = kCustomUdTypeFail;
  ud_fail.name = const_cast<char*>("ud_fail");
  ud_fail.u.ud.udata = payload;
  ud_fail.u.ud.size = sizeof(payload);
  EXPECT_LT(H5O_link_delete(f, nullptr, &ud_fail), 0);
  EXPECT_EQ(g_ud_delete_calls, 2);
}

TEST(H5OlinkTest, CopyCallbacksAndPublicShallowAndDeepCopyWork) {
  H5L_class_t cls_succeed{};
  cls_succeed.version = H5L_LINK_CLASS_T_VERS;
  cls_succeed.id = kCustomUdTypeSucceed;
  cls_succeed.comment = "ud_copy_succeed";
  cls_succeed.trav_func = UdTraverseNoop;
  cls_succeed.copy_func = UdCopySucceed;
  ASSERT_GE(H5Lregister(&cls_succeed), 0);
  absl::Cleanup unreg_succeed = [] { H5Lunregister(kCustomUdTypeSucceed); };

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile src(
      H5Fcreate("link_copy_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ScopedFile dst(
      H5Fcreate("link_copy_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  ScopedPlist lcpl_utf8(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl_utf8.valid());
  ASSERT_GE(H5Pset_char_encoding(lcpl_utf8.get(), H5T_CSET_UTF8), 0);

  {
    ScopedGroup parent(
        H5Gcreate2(src.get(), "/parent", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(parent.valid());
    ScopedGroup child(H5Gcreate2(parent.get(), "child", H5P_DEFAULT, gcpl.get(),
                                 H5P_DEFAULT));
    ASSERT_TRUE(child.valid());
    ScopedGroup grandchild(H5Gcreate2(child.get(), "grandchild", H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grandchild.valid());
    ASSERT_GE(H5Lcreate_soft("/parent/child/grandchild", parent.get(),
                             "soft_utf8", lcpl_utf8.get(), H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_external("ext.h5", "/ext_obj", parent.get(), "ext",
                                 H5P_DEFAULT, H5P_DEFAULT),
              0);
    const char ud_bytes[] = "ud_payload";
    ASSERT_GE(H5Lcreate_ud(parent.get(), "ud_link", kCustomUdTypeSucceed,
                           ud_bytes, sizeof(ud_bytes), H5P_DEFAULT,
                           H5P_DEFAULT),
              0);
  }

  // Deep copy exercises H5O__link_{pre_copy_file,copy_file,post_copy_file}.
  ASSERT_GE(H5Ocopy(src.get(), "/parent", dst.get(), "/deep", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(dst.get(), "/deep/child/grandchild", H5P_DEFAULT), 0);
  EXPECT_GT(H5Lexists(dst.get(), "/deep/ud_link", H5P_DEFAULT), 0);

  // Shallow hierarchy copy hits curr_depth >= max_depth inside /parent/child,
  // setting *deleted = true in H5O__link_pre_copy_file.
  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_SHALLOW_HIERARCHY_FLAG),
            0);
  ASSERT_GE(H5Ocopy(src.get(), "/parent", dst.get(), "/shallow", ocpypl.get(),
                    H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(dst.get(), "/shallow/child", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(dst.get(), "/shallow/child/grandchild", H5P_DEFAULT), 0);

  // Direct copy_file error check for invalid built-in link type, and public
  // H5Ocopy failure when expanding a self-referential soft link hits
  // H5O__link_post_copy_file's error return.
  H5F_t* f_src = GetNativeFile(src.get());
  H5F_t* f_dst = GetNativeFile(dst.get());
  ASSERT_NE(f_src, nullptr);
  ASSERT_NE(f_dst, nullptr);

  {
    ScopedGroup loop_grp(H5Gcreate2(src.get(), "/loop_grp", H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(loop_grp.valid());
    ASSERT_GE(H5Lcreate_soft("/loop_grp/self_loop", loop_grp.get(), "self_loop",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
  ScopedPlist ocpypl_expand(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl_expand.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl_expand.get(),
                               H5O_COPY_EXPAND_SOFT_LINK_FLAG),
            0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  EXPECT_LT(H5Ocopy(src.get(), "/loop_grp", dst.get(), "/loop_copy",
                    ocpypl_expand.get(), H5P_DEFAULT),
            0);

  H5O_copy_t cpy_info = ZeroStruct<H5O_copy_t>();
  cpy_info.max_depth = -1;
  cpy_info.file_dst = f_dst;

  bool deleted = false;
  bool recompute_size = false;
  unsigned mesg_flags = 0;

  H5O_link_t bad_builtin = ZeroStruct<H5O_link_t>();
  bad_builtin.type = kUnknownBuiltinType;
  bad_builtin.name = const_cast<char*>("bad_builtin");
  EXPECT_GE(H5O_MSG_LINK[0].pre_copy_file(f_src, &bad_builtin, &deleted,
                                           &cpy_info, nullptr),
            0);
  EXPECT_FALSE(deleted);
  EXPECT_EQ(H5O_MSG_LINK[0].copy_file(f_src, &bad_builtin, f_dst,
                                       &recompute_size, &mesg_flags, &cpy_info,
                                       nullptr),
            nullptr);
}

TEST(H5OlinkTest, DebugPrintsAllLinkVariantsAndRejectsUnknownType) {
  ScopedFile file = CreateInMemoryFile("link_debug.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  TempFile dbg_file("link_debug.txt");
  std::FILE* fp = std::fopen(dbg_file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);

  {
    absl::Cleanup close_fp = [&fp] {
      if (fp != nullptr) {
        std::fclose(fp);
      }
    };

    H5O_link_t hard = ZeroStruct<H5O_link_t>();
    hard.type = H5L_TYPE_HARD;
    hard.cset = H5T_CSET_ASCII;
    hard.name = const_cast<char*>("hard_link");
    hard.u.hard.addr = 0x1000;
    EXPECT_GE(H5O_debug_id(H5O_LINK_ID, f, &hard, fp, 0, 40), 0);

    H5O_link_t soft = ZeroStruct<H5O_link_t>();
    soft.type = H5L_TYPE_SOFT;
    soft.corder_valid = true;
    soft.corder = 17;
    soft.cset = H5T_CSET_UTF8;
    soft.name = const_cast<char*>("soft_link");
    soft.u.soft.name = const_cast<char*>("/target/obj");
    EXPECT_GE(H5O_debug_id(H5O_LINK_ID, f, &soft, fp, 2, 40), 0);

    char ext_data[] = "ext_file.h5\0/ext_dataset";
    H5O_link_t ext = ZeroStruct<H5O_link_t>();
    ext.type = H5L_TYPE_EXTERNAL;
    ext.cset = H5T_CSET_ASCII;
    ext.name = const_cast<char*>("ext_link");
    ext.u.ud.udata = ext_data;
    ext.u.ud.size = sizeof(ext_data);
    EXPECT_GE(H5O_debug_id(H5O_LINK_ID, f, &ext, fp, 0, 40), 0);

    char ud_data[] = "opaque";
    H5O_link_t ud = ZeroStruct<H5O_link_t>();
    ud.type = kCustomUdTypeSucceed;
    ud.cset = H5T_CSET_RESERVED_2;
    ud.name = const_cast<char*>("ud_link");
    ud.u.ud.udata = ud_data;
    ud.u.ud.size = sizeof(ud_data);
    EXPECT_GE(H5O_debug_id(H5O_LINK_ID, f, &ud, fp, 0, 40), 0);

    ScopedErrorSilence silence;
    H5O_link_t unknown = ZeroStruct<H5O_link_t>();
    unknown.type = kUnknownBuiltinType;
    unknown.cset = H5T_CSET_ASCII;
    unknown.name = const_cast<char*>("unknown_link");
    EXPECT_LT(H5O_debug_id(H5O_LINK_ID, f, &unknown, fp, 0, 40), 0);

    std::fclose(fp);
    fp = nullptr;
  }

  const std::string output = ReadWholeFile(dbg_file);
  EXPECT_THAT(output, HasSubstr("Hard"));
  EXPECT_THAT(output, HasSubstr("Soft"));
  EXPECT_THAT(output, HasSubstr("External"));
  EXPECT_THAT(output, HasSubstr("User-defined"));
  EXPECT_THAT(output, HasSubstr("Unknown"));
  EXPECT_THAT(output, HasSubstr("ASCII"));
  EXPECT_THAT(output, HasSubstr("UTF-8"));
  EXPECT_THAT(output, HasSubstr("Creation Order:"));
  EXPECT_THAT(output, HasSubstr("ext_file.h5"));
  EXPECT_THAT(output, HasSubstr("/ext_dataset"));
  EXPECT_THAT(output, HasSubstr("User-Defined Link Size:"));
}

}  // namespace
}  // namespace hdf5
