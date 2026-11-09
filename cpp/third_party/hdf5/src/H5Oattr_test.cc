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
// Tests attribute object header message encoding, decoding, corrupt-buffer
// bounds checks, copying, file copy callbacks, creation indices, shared
// datatypes/dataspaces, and debug formatting (third_party/hdf5/src/H5Oattr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oattr.c

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match internal struct layouts across C++.
#ifndef NDEBUG
#define H5OATTR_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma push_macro("try")
#undef try
#define try h5_param_try
extern "C" {
#define H5O_FRIEND
#define H5A_FRIEND
#define H5S_FRIEND
#include "third_party/hdf5/src/H5Apkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#undef try
#pragma pop_macro("try")
#ifdef H5OATTR_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OATTR_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

// Pushes/pops an HDF5 API context and metadata cache tag for internal calls.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000) {
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

// RAII owner for native message structs allocated via H5O_msg_decode / copy.
class ScopedMsg {
 public:
  ScopedMsg() = default;
  ScopedMsg(unsigned type_id, void* msg) : type_id_(type_id), msg_(msg) {}
  ~ScopedMsg() { reset(); }

  ScopedMsg(const ScopedMsg&) = delete;
  ScopedMsg& operator=(const ScopedMsg&) = delete;

  void reset(unsigned type_id = 0, void* next = nullptr) {
    if (msg_ != nullptr) {
      H5O_msg_free(type_id_, msg_);
    }
    type_id_ = type_id;
    msg_ = next;
  }

  void* get() const { return msg_; }

 private:
  unsigned type_id_ = 0;
  void* msg_ = nullptr;
};

ScopedFile CreateCoreFileWithBounds(const char* name, H5F_libver_t low,
                                    H5F_libver_t high,
                                    hid_t fcpl = H5P_DEFAULT) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl.get(), low, high) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, fcpl, fapl.get()));
}

H5F_t* GetInternalFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5A_t* GetInternalAttr(hid_t attr_id) {
  return static_cast<H5A_t*>(H5VL_object_verify(attr_id, H5I_ATTR));
}

std::string CaptureMemstream(const std::function<void(FILE*)>& fn) {
  char* buf = nullptr;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  if (stream == nullptr) {
    return "";
  }
  auto cleanup = absl::MakeCleanup([&]() {
    if (stream != nullptr) {
      std::fclose(stream);
    }
    std::free(buf);
  });
  fn(stream);
  std::fflush(stream);
  std::fclose(stream);
  stream = nullptr;
  return (buf != nullptr) ? std::string(buf, len) : std::string();
}

TEST(H5OattrTest, Version1EncodeDecodeAndSize) {
  ScopedFile file = CreateCoreFileWithBounds(
      "v1_attr.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // 5-char name => name_len = 6 => padded to 8 bytes in v1.
  // Unwritten int32 attribute exercises zero-fill encode branch (data == nullptr).
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr unwritten(H5Acreate2(grp.get(), "alpha", H5T_STD_I32LE,
                                  scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(unwritten.valid());

  // NULL dataspace exercises data_size == 0 path in decode and encode.
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());
  ScopedAttr null_attr(H5Acreate2(grp.get(), "null_a", H5T_STD_I32LE,
                                  null_space.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(null_attr.valid());

  // Written attribute with 2 elements and unaligned name length.
  hsize_t dims[1] = {2};
  ScopedSpace vec_space(H5Screate_simple(1, dims, nullptr));
  ASSERT_TRUE(vec_space.valid());
  ScopedAttr written(H5Acreate2(grp.get(), "beta", H5T_STD_I32LE,
                                vec_space.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(written.valid());
  std::array<int32_t, 2> vals = {123, -456};
  EXPECT_GE(H5Awrite(written.get(), H5T_NATIVE_INT32, vals.data()), 0);

  H5F_t* f = GetInternalFile(file.get());
  ASSERT_NE(f, nullptr);
  H5A_t* a_unwritten = GetInternalAttr(unwritten.get());
  H5A_t* a_null = GetInternalAttr(null_attr.get());
  H5A_t* a_written = GetInternalAttr(written.get());
  ASSERT_NE(a_unwritten, nullptr);
  ASSERT_NE(a_null, nullptr);
  ASSERT_NE(a_written, nullptr);
  EXPECT_EQ(a_unwritten->shared->version, H5O_ATTR_VERSION_1);
  EXPECT_EQ(a_null->shared->version, H5O_ATTR_VERSION_1);
  EXPECT_EQ(a_written->shared->version, H5O_ATTR_VERSION_1);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  for (H5A_t* a : {a_unwritten, a_null, a_written}) {
    size_t sz = H5O_msg_raw_size(f, H5O_ATTR_ID, false, a);
    ASSERT_GT(sz, 0u);
    std::vector<uint8_t> raw(sz, 0xaa);
    ASSERT_GE(H5O_msg_encode(f, H5O_ATTR_ID, false, raw.data(), a), 0);

    ScopedMsg decoded(
        H5O_ATTR_ID,
        H5O_msg_decode(f, nullptr, H5O_ATTR_ID, raw.size(), raw.data()));
    ASSERT_NE(decoded.get(), nullptr);
    const H5A_t* da = static_cast<const H5A_t*>(decoded.get());
    EXPECT_EQ(da->shared->version, H5O_ATTR_VERSION_1);
    EXPECT_STREQ(da->shared->name, a->shared->name);
    EXPECT_EQ(da->shared->data_size, a->shared->data_size);
    if (a == a_unwritten) {
      ASSERT_NE(da->shared->data, nullptr);
      int32_t zero_val = 99;
      std::memcpy(&zero_val, da->shared->data, sizeof(zero_val));
      EXPECT_EQ(zero_val, 0);
    } else if (a == a_null) {
      EXPECT_EQ(da->shared->data_size, 0u);
    } else {
      ASSERT_NE(da->shared->data, nullptr);
      std::array<int32_t, 2> out_vals = {};
      std::memcpy(out_vals.data(), da->shared->data, sizeof(out_vals));
      EXPECT_EQ(out_vals[0], 123);
      EXPECT_EQ(out_vals[1], -456);
    }
  }
}

TEST(H5OattrTest, Version2CommittedDatatypeAndSohmSharedMessages) {
  TempFile tmp("v2_shared_attr.h5");
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(
                  fcpl.get(), 0,
                  H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_SDSPACE_FLAG |
                      H5O_SHMESG_ATTR_FLAG,
                  1),
              0);
    ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 2, 1), 0);

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
        0);

    ScopedFile file(
        H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());

    // Committed datatype + ASCII name forces H5O_ATTR_VERSION_2 in earliest
    // libver bounds.
    ScopedType ctype(H5Tcopy(H5T_STD_I32LE));
    ASSERT_TRUE(ctype.valid());
    ASSERT_GE(H5Tcommit2(file.get(), "/shared_i32", ctype.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);

    hsize_t dims[2] = {2, 3};
    ScopedSpace space(H5Screate_simple(2, dims, nullptr));
    ASSERT_TRUE(space.valid());

    ScopedPlist ocpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(ocpl.valid());
    // Force dense storage on g1 after 1 attribute to test link/delete on dense.
    ASSERT_GE(H5Pset_attr_phase_change(ocpl.get(), 1, 1), 0);
    ScopedGroup g1(
        H5Gcreate2(file.get(), "/g1", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g1.valid());

    std::array<int32_t, 6> data = {1, 2, 3, 4, 5, 6};
    for (const char* name : {"a1", "a2", "a3"}) {
      ScopedAttr a(H5Acreate2(g1.get(), name, ctype.get(), space.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
      EXPECT_GE(H5Awrite(a.get(), H5T_NATIVE_INT32, data.data()), 0);

      H5F_t* f = GetInternalFile(file.get());
      H5A_t* ia = GetInternalAttr(a.get());
      ASSERT_NE(ia, nullptr);
      EXPECT_EQ(ia->shared->version, H5O_ATTR_VERSION_2);

      // Test raw size, encode, decode (with disable_shared = true & false).
      ScopedApiContext ctx;
      size_t sz = H5O_msg_raw_size(f, H5O_ATTR_ID, true, ia);
      ASSERT_GT(sz, 0u);
      std::vector<uint8_t> raw(sz);
      ASSERT_GE(H5O_msg_encode(f, H5O_ATTR_ID, true, raw.data(), ia), 0);
      ScopedMsg dec(H5O_ATTR_ID,
                    H5O_msg_decode(f, nullptr, H5O_ATTR_ID, raw.size(),
                                   raw.data()));
      ASSERT_NE(dec.get(), nullptr);
    }

    // Also create a second object sharing exact same attribute to trigger SOHM.
    ScopedGroup g2(
        H5Gcreate2(file.get(), "/g2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g2.valid());
    for (hid_t obj : {g1.get(), g2.get()}) {
      ScopedAttr dup(H5Acreate2(obj, "dup_shared_attr", ctype.get(),
                                space.get(), H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(dup.valid());
      EXPECT_GE(H5Awrite(dup.get(), H5T_NATIVE_INT32, data.data()), 0);
    }

    // Delete attributes to exercise H5O__attr_delete and SOHM decrements.
    EXPECT_GE(H5Adelete(g1.get(), "a2"), 0);
    EXPECT_GE(H5Adelete(g2.get(), "dup_shared_attr"), 0);
  }

  // Reopen from disk to decode persisted v2 / shared attributes fresh.
  ScopedFile reopened(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  ScopedGroup g1(H5Gopen2(reopened.get(), "/g1", H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ScopedAttr a1(H5Aopen(g1.get(), "a1", H5P_DEFAULT));
  ASSERT_TRUE(a1.valid());
  std::array<int32_t, 6> read_back = {};
  EXPECT_GE(H5Aread(a1.get(), H5T_NATIVE_INT32, read_back.data()), 0);
  EXPECT_EQ(read_back[5], 6);

  // Re-open SOHM-shared duplicate attribute and verify its raw encoding/link/del.
  ScopedAttr dup(H5Aopen(g1.get(), "dup_shared_attr", H5P_DEFAULT));
  ASSERT_TRUE(dup.valid());
  H5F_t* f = GetInternalFile(reopened.get());
  H5A_t* idup = GetInternalAttr(dup.get());
  ASSERT_NE(idup, nullptr);
  {
    ScopedApiContext ctx;
    ScopedMsg copy_msg(H5O_ATTR_ID, H5O_msg_copy(H5O_ATTR_ID, idup, nullptr));
    ASSERT_NE(copy_msg.get(), nullptr);
    EXPECT_GE(H5O__attr_link(f, nullptr, copy_msg.get()), 0);
    EXPECT_GE(H5O__attr_delete(f, nullptr, copy_msg.get()), 0);
  }
}

TEST(H5OattrTest, Version3Utf8AndBoundsUpgrade) {
  ScopedFile file = CreateCoreFileWithBounds(
      "v3_utf8.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedPlist acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(acpl.valid());
  ASSERT_GE(H5Pset_char_encoding(acpl.get(), H5T_CSET_UTF8), 0);
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr utf8_attr(H5Acreate2(grp.get(), "utf8_\xc3\xa9", H5T_STD_I32LE,
                                  scalar.get(), acpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(utf8_attr.valid());
  int32_t v = 77;
  EXPECT_GE(H5Awrite(utf8_attr.get(), H5T_NATIVE_INT32, &v), 0);

  H5F_t* f = GetInternalFile(file.get());
  H5A_t* ia = GetInternalAttr(utf8_attr.get());
  ASSERT_NE(ia, nullptr);
  EXPECT_EQ(ia->shared->version, H5O_ATTR_VERSION_3);
  EXPECT_EQ(ia->shared->encoding, H5T_CSET_UTF8);

  ScopedApiContext ctx;
  size_t sz = H5O_msg_raw_size(f, H5O_ATTR_ID, false, ia);
  ASSERT_GT(sz, 0u);
  std::vector<uint8_t> raw(sz);
  ASSERT_GE(H5O_msg_encode(f, H5O_ATTR_ID, false, raw.data(), ia), 0);

  ScopedMsg dec(H5O_ATTR_ID,
                H5O_msg_decode(f, nullptr, H5O_ATTR_ID, raw.size(), raw.data()));
  ASSERT_NE(dec.get(), nullptr);
  const H5A_t* da = static_cast<const H5A_t*>(dec.get());
  EXPECT_EQ(da->shared->version, H5O_ATTR_VERSION_3);
  EXPECT_EQ(da->shared->encoding, H5T_CSET_UTF8);
  EXPECT_STREQ(da->shared->name, "utf8_\xc3\xa9");
}

TEST(H5OattrTest, DecodeHandlesTruncatedAndCorruptBuffers) {
  // Use 4-byte length sizes in fcpl so a v1 1D simple dataspace has
  // unaligned ds_size = 12 (which pads to 16 in v1!), allowing us to test the
  // v1 dataspace alignment overflow branch deterministically.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_sizes(fcpl.get(), 4, 4), 0);

  ScopedFile file = CreateCoreFileWithBounds(
      "corrupt_decode.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST, fcpl.get());
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // 1) Build v1 encoded reference message (unaligned name_len=3, dt_size=12,
  //    ds_size=12, 1D simple length=2 -> data_size=8).
  hsize_t dim1 = 2;
  ScopedSpace s1(H5Screate_simple(1, &dim1, nullptr));
  ASSERT_TRUE(s1.valid());
  ScopedAttr a1(H5Acreate2(grp.get(), "ab", H5T_STD_I32LE, s1.get(),
                           H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(a1.valid());
  std::array<int32_t, 2> vals = {10, 20};
  EXPECT_GE(H5Awrite(a1.get(), H5T_NATIVE_INT32, vals.data()), 0);

  // 2) Build v2 encoded reference message (committed dtype + 1D simple space).
  ScopedType ctype(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(ctype.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/ct", ctype.get(), H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            0);
  ScopedAttr a2(H5Acreate2(grp.get(), "ac", ctype.get(), s1.get(),
                           H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(a2.valid());
  EXPECT_GE(H5Awrite(a2.get(), H5T_NATIVE_INT32, vals.data()), 0);

  // 3) Build v3 encoded reference message (UTF-8).
  ScopedPlist acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(acpl.valid());
  ASSERT_GE(H5Pset_char_encoding(acpl.get(), H5T_CSET_UTF8), 0);
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr a3(H5Acreate2(grp.get(), "cd", H5T_STD_I32LE, scalar.get(),
                           acpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(a3.valid());
  int32_t scalar_val = 42;
  EXPECT_GE(H5Awrite(a3.get(), H5T_NATIVE_INT32, &scalar_val), 0);

  H5F_t* f = GetInternalFile(file.get());
  H5A_t* ia1 = GetInternalAttr(a1.get());
  H5A_t* ia2 = GetInternalAttr(a2.get());
  H5A_t* ia3 = GetInternalAttr(a3.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ScopedErrorSilence silence;

  auto encode_raw = [&](H5A_t* a, bool disable_shared) {
    size_t sz = H5O_msg_raw_size(f, H5O_ATTR_ID, disable_shared, a);
    std::vector<uint8_t> buf(sz);
    EXPECT_GE(H5O_msg_encode(f, H5O_ATTR_ID, disable_shared, buf.data(), a), 0);
    return buf;
  };

  std::vector<uint8_t> v1_buf = encode_raw(ia1, false);
  // Force unshared native dtype+dspace encoding inside v2 layout by copying and
  // clearing shared info or mutating v3->v2 buffer. Note that for ia2,
  // disable_shared=true keeps dtype shared flag in H5O__attr_encode because
  // H5O__attr_encode checks H5O_msg_is_shared(dt). Using a v3-like unshared
  // layout or patching raw header gives exact unshared v2 too.
  std::vector<uint8_t> v2_buf = encode_raw(ia2, true);
  std::vector<uint8_t> v3_buf = encode_raw(ia3, false);

  auto expect_decode_fail = [&](const std::vector<uint8_t>& buf, size_t len) {
    static const uint8_t dummy = 0;
    const uint8_t* ptr = buf.empty() ? &dummy : buf.data();
    ScopedMsg m(H5O_ATTR_ID,
                H5O_msg_decode(f, nullptr, H5O_ATTR_ID, len, ptr));
    EXPECT_EQ(m.get(), nullptr) << "Expected failure for len=" << len;
  };

  // Truncations in common prefix (0..7) and v3 encoding (8).
  for (size_t trunc : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{4},
                       size_t{5}, size_t{6}, size_t{7}}) {
    expect_decode_fail(v1_buf, trunc);
  }
  expect_decode_fail(v3_buf, 8);  // Before v3 encoding byte.

  // Bad version values (0 and H5O_ATTR_VERSION_LATEST + 1).
  {
    auto bad = v1_buf;
    bad[0] = 0;
    expect_decode_fail(bad, bad.size());
    bad[0] = H5O_ATTR_VERSION_LATEST + 1;
    expect_decode_fail(bad, bad.size());
  }

  // Unknown flags in v2 (>= version 2).
  {
    auto bad = v2_buf;
    bad[1] = 0x04;
    expect_decode_fail(bad, bad.size());
  }

  // Invalid decoded name_len (0 and 1 -> <= 1).
  {
    auto bad = v1_buf;
    bad[2] = 0;
    bad[3] = 0;
    expect_decode_fail(bad, bad.size());
    bad[2] = 1;
    bad[3] = 0;
    expect_decode_fail(bad, bad.size());
  }

  // Name truncated & v1 name 8-byte alignment padding truncation.
  // v1 header is 8 bytes; name is "ab\0" (3 bytes, aligned to 8 bytes).
  expect_decode_fail(v1_buf, 8 + 2);  // Cut before name_len (3).
  expect_decode_fail(v1_buf, 8 + 4);  // Cut before v1 aligned name end (8).

  // Embedded NUL inside name so name_len != strnlen + 1.
  {
    auto bad = v1_buf;
    bad[8] = 'a';
    bad[9] = '\0';
    bad[10] = 'b';
    expect_decode_fail(bad, bad.size());
  }

  // Datatype checks (offset 16 in v1_buf; dt_size = ia1->shared->dt_size = 12).
  const size_t v1_dt_off = 8 + 8;
  expect_decode_fail(v1_buf, v1_dt_off + 2);  // Cut inside dt_size.
  {
    auto bad = v1_buf;
    bad[v1_dt_off] = 0xff;  // Invalid datatype class/version.
    expect_decode_fail(bad, bad.size());
  }
  if (ia1->shared->dt_size < 16) {
    // Passes DTYPE->decode, then triggers v1 H5O_ALIGN_OLD(dt_size) overflow.
    expect_decode_fail(v1_buf, v1_dt_off + ia1->shared->dt_size);
  }

  // Dataspace checks (v1_ds_off = v1_dt_off + H5O_ALIGN_OLD(dt_size)).
  const size_t v1_ds_off =
      v1_dt_off + 8 * ((ia1->shared->dt_size + 7) / 8);
  expect_decode_fail(v1_buf, v1_ds_off + 2);  // Cut inside ds_size.
  {
    auto bad = v1_buf;
    bad[v1_ds_off] = 0xff;  // Invalid dataspace version.
    expect_decode_fail(bad, bad.size());
  }
  if (ia1->shared->ds_size % 8 != 0) {
    // Passes SDSPACE->decode, then triggers v1 H5O_ALIGN_OLD(ds_size) overflow.
    expect_decode_fail(v1_buf, v1_ds_off + ia1->shared->ds_size);
  }

  // Truncation inside attribute data payload.
  const size_t v1_data_off =
      v1_ds_off + 8 * ((ia1->shared->ds_size + 7) / 8);
  expect_decode_fail(v1_buf, v1_data_off + ia1->shared->data_size - 1);

  // Also test v1 decode -> convert in-place to unshared v2-compatible layout
  // with 64-bit size_t to trigger data_size multiplication overflow
  // (ds_size * dt_size wraps uint64_t).
  {
    ScopedFile f64 = CreateCoreFileWithBounds(
        "overflow64.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(f64.valid());
    ScopedGroup g64(H5Gcreate2(f64.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(g64.valid());
    hsize_t d = 1;
    ScopedSpace s64(H5Screate_simple(1, &d, nullptr));
    ASSERT_TRUE(s64.valid());
    ScopedPlist acpl64(H5Pcreate(H5P_ATTRIBUTE_CREATE));
    ASSERT_TRUE(acpl64.valid());
    ASSERT_GE(H5Pset_char_encoding(acpl64.get(), H5T_CSET_UTF8), 0);
    ScopedAttr a64(H5Acreate2(g64.get(), "ov", H5T_STD_I32LE, s64.get(),
                              acpl64.get(), H5P_DEFAULT));
    ASSERT_TRUE(a64.valid());
    H5F_t* fi64 = GetInternalFile(f64.get());
    H5A_t* ia64 = GetInternalAttr(a64.get());
    ASSERT_EQ(ia64->shared->version, H5O_ATTR_VERSION_3);
    ASSERT_EQ(ia64->shared->ds_size, 20u);
    size_t sz64 = H5O_msg_raw_size(fi64, H5O_ATTR_ID, false, ia64);
    std::vector<uint8_t> buf64(sz64);
    ASSERT_GE(H5O_msg_encode(fi64, H5O_ATTR_ID, false, buf64.data(), ia64), 0);

    // Offset to dimension[0] and maxdim[0] inside v3 + v2-sdspace buffer:
    // prefix(9) + name("ov\0"=3) + dt_size + 4-byte ds header.
    const size_t ds_off = 9 + 3 + ia64->shared->dt_size;
    ASSERT_EQ(buf64[ds_off], 2u);      // Version 2 dataspace
    ASSERT_EQ(buf64[ds_off + 1], 1u);  // Rank 1
    // Patch both size[0] (+4) and max[0] (+12) to (1ULL << 62) + 1 so
    // H5O__sdspace_decode succeeds (size <= max) and size * 4 overflows 64-bit.
    const uint64_t bad_dim = (uint64_t{1} << 62) + 1;
    for (size_t field_off : {ds_off + 4, ds_off + 12}) {
      for (size_t i = 0; i < 8; ++i) {
        buf64[field_off + i] =
            static_cast<uint8_t>((bad_dim >> (8 * i)) & 0xff);
      }
    }
    ScopedMsg m(
        H5O_ATTR_ID,
        H5O_msg_decode(fi64, nullptr, H5O_ATTR_ID, buf64.size(), buf64.data()));
    EXPECT_EQ(m.get(), nullptr);
  }
}

TEST(H5OattrTest, CopyResetFreeAndCreationIndex) {
  ScopedFile file = CreateCoreFileWithBounds(
      "copy_crt.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());

  ScopedPlist ocpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(ocpl.valid());
  ASSERT_GE(H5Pset_attr_creation_order(
                ocpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr a1(H5Acreate2(grp.get(), "first", H5T_STD_I32LE, scalar.get(),
                           H5P_DEFAULT, H5P_DEFAULT));
  ScopedAttr a2(H5Acreate2(grp.get(), "second", H5T_STD_I32LE, scalar.get(),
                           H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(a1.valid());
  ASSERT_TRUE(a2.valid());

  H5A_t* ia1 = GetInternalAttr(a1.get());
  H5A_t* ia2 = GetInternalAttr(a2.get());
  ASSERT_NE(ia1, nullptr);
  ASSERT_NE(ia2, nullptr);

  ScopedApiContext ctx;
  H5O_msg_crt_idx_t idx1 = 999, idx2 = 999;
  EXPECT_GE(H5O_msg_get_crt_index(H5O_ATTR_ID, ia1, &idx1), 0);
  EXPECT_GE(H5O_msg_get_crt_index(H5O_ATTR_ID, ia2, &idx2), 0);
  EXPECT_EQ(idx1, 0u);
  EXPECT_EQ(idx2, 1u);

  // Test H5O_msg_copy with both nullptr dest and preallocated (uninitialized)
  // non-null dest, plus H5O_msg_reset / H5O__attr_reset / set_crt_index / free.
  ScopedMsg copy1(H5O_ATTR_ID, H5O_msg_copy(H5O_ATTR_ID, ia1, nullptr));
  ASSERT_NE(copy1.get(), nullptr);

  H5A_t prealloc = {};
  void* copied_into = H5O_msg_copy(H5O_ATTR_ID, ia2, &prealloc);
  EXPECT_EQ(copied_into, &prealloc);
  EXPECT_STREQ(prealloc.shared->name, "second");
  EXPECT_GT(prealloc.shared->nrefs, 1u);
  prealloc.shared->nrefs--;
  EXPECT_GE(H5G_name_free(&prealloc.path), 0);

  EXPECT_GE((H5O_MSG_ATTR->set_crt_index)(copy1.get(), 77), 0);
  H5O_msg_crt_idx_t out_idx = 0;
  EXPECT_GE(H5O_msg_get_crt_index(H5O_ATTR_ID, copy1.get(), &out_idx), 0);
  EXPECT_EQ(out_idx, 77u);

  EXPECT_GE(H5O__attr_reset(copy1.get()), 0);
  EXPECT_GE(H5O_msg_reset(H5O_ATTR_ID, copy1.get()), 0);
}

TEST(H5OattrTest, FileCopyPreCopyPostCopyAndWithoutAttrFlag) {
  ScopedFile src = CreateCoreFileWithBounds(
      "copy_src.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(src.valid());
  ScopedGroup src_grp(
      H5Gcreate2(src.get(), "/src_obj", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(src_grp.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr v1_attr(H5Acreate2(src_grp.get(), "plain", H5T_STD_I32LE,
                                scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(v1_attr.valid());
  int32_t v = 99;
  EXPECT_GE(H5Awrite(v1_attr.get(), H5T_NATIVE_INT32, &v), 0);

  // Object reference attribute pointing to src_grp itself exercises
  // H5O__attr_post_copy_file.
  hobj_ref_t self_ref = {};
  ASSERT_GE(H5Rcreate(&self_ref, src.get(), "/src_obj", H5R_OBJECT, -1), 0);
  ScopedAttr ref_attr(H5Acreate2(src_grp.get(), "self_ref", H5T_STD_REF_OBJ,
                                 scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ref_attr.valid());
  EXPECT_GE(H5Awrite(ref_attr.get(), H5T_STD_REF_OBJ, &self_ref), 0);

  // UTF-8 (v3) attribute so earliest-bound dest rejects version out of bounds.
  ScopedPlist acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(acpl.valid());
  ASSERT_GE(H5Pset_char_encoding(acpl.get(), H5T_CSET_UTF8), 0);
  ScopedAttr v3_attr(H5Acreate2(src_grp.get(), "utf8_name", H5T_STD_I32LE,
                                scalar.get(), acpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(v3_attr.valid());
  EXPECT_GE(H5Awrite(v3_attr.get(), H5T_NATIVE_INT32, &v), 0);

  // Destination 1: modern bounds, full copy + expand object references.
  ScopedFile dst = CreateCoreFileWithBounds("copy_dst.h5", H5F_LIBVER_V18,
                                            H5F_LIBVER_LATEST);
  ASSERT_TRUE(dst.valid());
  EXPECT_GE(H5Ocopy(src.get(), "/src_obj", dst.get(), "/copied_default",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedPlist ocpy_expand(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy_expand.valid());
  ASSERT_GE(H5Pset_copy_object(ocpy_expand.get(), H5O_COPY_EXPAND_REFERENCE_FLAG),
            0);
  EXPECT_GE(H5Ocopy(src.get(), "/src_obj", dst.get(), "/copied_expand",
                    ocpy_expand.get(), H5P_DEFAULT),
            0);

  // Destination 2: H5O_COPY_WITHOUT_ATTR_FLAG sets *deleted = true in pre_copy.
  ScopedPlist ocpy_no_attr(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy_no_attr.valid());
  ASSERT_GE(
      H5Pset_copy_object(ocpy_no_attr.get(), H5O_COPY_WITHOUT_ATTR_FLAG), 0);
  EXPECT_GE(H5Ocopy(src.get(), "/src_obj", dst.get(), "/copied_no_attr",
                    ocpy_no_attr.get(), H5P_DEFAULT),
            0);

  ScopedGroup no_attr_grp(H5Gopen2(dst.get(), "/copied_no_attr", H5P_DEFAULT));
  ASSERT_TRUE(no_attr_grp.valid());
  H5O_info2_t oinfo = {};
  ASSERT_GE(H5Oget_info3(no_attr_grp.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
  EXPECT_EQ(oinfo.num_attrs, 0u);

  // Verify out-of-bounds attribute message version rejection inside
  // H5O__attr_pre_copy_file using a copied message above destination bounds.
  H5F_t* f_dst = GetInternalFile(dst.get());
  H5A_t* ia_v3 = GetInternalAttr(v3_attr.get());
  ASSERT_NE(f_dst, nullptr);
  ASSERT_NE(ia_v3, nullptr);
  {
    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    ScopedMsg copy_msg(H5O_ATTR_ID, H5O_msg_copy(H5O_ATTR_ID, ia_v3, nullptr));
    ASSERT_NE(copy_msg.get(), nullptr);
    H5A_t* bad_ver = static_cast<H5A_t*>(copy_msg.get());
    bad_ver->shared->version = H5O_ATTR_VERSION_LATEST + 1;
    bool deleted = false;
    H5O_copy_t cpy_info = {};
    cpy_info.file_dst = f_dst;
    EXPECT_LT((H5O_MSG_ATTR->pre_copy_file)(nullptr, bad_ver, &deleted,
                                            &cpy_info, nullptr),
              0);
  }
}

TEST(H5OattrTest, DebugFormattingAllCharacterSetsAndStates) {
  ScopedFile file = CreateCoreFileWithBounds(
      "debug_attr.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedPlist ocpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(ocpl.valid());
  ASSERT_GE(H5Pset_attr_creation_order(ocpl.get(), H5P_CRT_ORDER_TRACKED), 0);

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr attr(H5Acreate2(grp.get(), "dbg", H5T_STD_I32LE, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  int32_t v = 5;
  EXPECT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &v), 0);

  H5F_t* f = GetInternalFile(file.get());
  H5A_t* ia = GetInternalAttr(attr.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(ia, nullptr);

  ScopedApiContext ctx;
  ScopedMsg copy_msg(H5O_ATTR_ID, H5O_msg_copy(H5O_ATTR_ID, ia, nullptr));
  ASSERT_NE(copy_msg.get(), nullptr);
  H5A_t* m = static_cast<H5A_t*>(copy_msg.get());

  // 1) Opened attribute with crt_idx set.
  std::string out_open = CaptureMemstream([&](FILE* s) {
    EXPECT_GE(H5O_debug_id(H5O_ATTR_ID, f, ia, s, 0, 20), 0);
  });
  EXPECT_THAT(out_open, HasSubstr("ASCII"));
  EXPECT_THAT(out_open, HasSubstr("Object opened:       TRUE"));
  EXPECT_THAT(out_open, HasSubstr("Creation Index:"));

  // 2) Copied/unopened attribute with UTF-8, RESERVED_2..15, ERROR/invalid,
  //    and H5O_MAX_CRT_ORDER_IDX.
  m->obj_opened = false;
  m->shared->crt_idx = H5O_MAX_CRT_ORDER_IDX;
  m->shared->encoding = H5T_CSET_UTF8;
  std::string out_utf8 = CaptureMemstream([&](FILE* s) {
    EXPECT_GE(H5O_debug_id(H5O_ATTR_ID, f, m, s, 2, 20), 0);
  });
  EXPECT_THAT(out_utf8, HasSubstr("UTF-8"));
  EXPECT_THAT(out_utf8, HasSubstr("FALSE"));
  EXPECT_THAT(out_utf8, ::testing::Not(HasSubstr("Creation Index:")));

  m->shared->encoding = H5T_CSET_RESERVED_2;
  std::string out_res = CaptureMemstream([&](FILE* s) {
    EXPECT_GE(H5O_debug_id(H5O_ATTR_ID, f, m, s, 0, 20), 0);
  });
  EXPECT_THAT(out_res, HasSubstr("H5T_CSET_RESERVED_2"));

  m->shared->encoding = H5T_CSET_ERROR;
  std::string out_err = CaptureMemstream([&](FILE* s) {
    EXPECT_GE(H5O_debug_id(H5O_ATTR_ID, f, m, s, 0, 20), 0);
  });
  EXPECT_THAT(out_err, HasSubstr("Unknown character set"));
}

}  // namespace
}  // namespace hdf5
