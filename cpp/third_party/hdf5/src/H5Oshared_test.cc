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
// Tests Shared Object Header Message operations (v1/v2/v3 decoding, encoding,
// sizing, set_shared, link/delete adjustment for committed & SOHM messages,
// self-referential object headers, cross-file copy/post-copy, and debug
// formatting) in third_party/hdf5/src/H5Oshared.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oshared.c

#include <stdio.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is compiled with -DNDEBUG, omitting debug fields from cache/object
// header structs. Define NDEBUG around internal headers so struct offsets match.
#ifndef NDEBUG
#define H5OSHARED_TEST_RESTORE_UNDEF_NDEBUG 1
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
#define H5S_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5SLprivate.h"
#include "third_party/hdf5/src/H5SMprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5OSHARED_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OSHARED_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__IGNORE_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (tag != H5AC__IGNORE_TAG) {
        H5AC_tag(tag, &prev_tag_);
        tagged_ = true;
      }
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

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void EncodeLittleEndian(haddr_t val, size_t width, uint8_t* out) {
  for (size_t i = 0; i < width; ++i) {
    out[i] = static_cast<uint8_t>(val & 0xffU);
    val >>= 8;
  }
}

std::string CaptureSharedDebug(const H5O_shared_t* sh) {
  char* buf = nullptr;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  if (stream == nullptr) {
    return "";
  }
  absl::Cleanup free_buf = [&] { std::free(buf); };
  const herr_t rc = H5O__shared_debug(sh, stream, /*indent=*/2, /*fwidth=*/24);
  std::fclose(stream);
  if (rc < 0) {
    return "";
  }
  return std::string(buf != nullptr ? buf : "", len);
}

TEST(H5OsharedTest, DebugAllShareTypes) {
  ScopedFile file = CreateInMemoryFile("shared_debug.h5");
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5O_shared_t msg{};

  msg.type = H5O_SHARE_TYPE_UNSHARED;
  EXPECT_THAT(CaptureSharedDebug(&msg), HasSubstr("Unshared"));

  msg.type = H5O_SHARE_TYPE_COMMITTED;
  msg.u.loc.oh_addr = 12345;
  {
    const std::string out = CaptureSharedDebug(&msg);
    EXPECT_THAT(out, HasSubstr("Obj Hdr"));
    EXPECT_THAT(out, HasSubstr("Object address:"));
    EXPECT_THAT(out, HasSubstr("12345"));
  }

  msg.type = H5O_SHARE_TYPE_SOHM;
  msg.u.heap_id.val = 0x0102030405060708ULL;
  {
    const std::string out = CaptureSharedDebug(&msg);
    EXPECT_THAT(out, HasSubstr("SOHM"));
    EXPECT_THAT(out, HasSubstr("Heap ID:"));
  }

  msg.type = H5O_SHARE_TYPE_HERE;
  EXPECT_THAT(CaptureSharedDebug(&msg), HasSubstr("Here"));

  msg.type = 99;
  EXPECT_THAT(CaptureSharedDebug(&msg), HasSubstr("Unknown (99)"));
}

TEST(H5OsharedTest, EncodeSizeSetSharedAndDecodeV1V2V3AndErrors) {
  for (const size_t addr_sz : {size_t{4}, size_t{8}}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), addr_sz, addr_sz), 0);
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(
                  fcpl.get(), 0, H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_SDSPACE_FLAG,
                  1),
              0);

    ScopedFile file = CreateInMemoryFile("shared_codec.h5", H5F_ACC_TRUNC,
                                           fcpl.get());
    ASSERT_TRUE(file.valid());

    // Create a committed datatype /dt and a SOHM-shared dataspace on /ds1,/ds2.
    ScopedType dt(H5Tcopy(H5T_NATIVE_INT));
    ASSERT_TRUE(dt.valid());
    ASSERT_GE(H5Tcommit2(file.get(), "/dt", dt.get(), H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT),
              0);

    const std::array<hsize_t, 3> dims = {2, 3, 4};
    ScopedSpace space(H5Screate_simple(3, dims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset ds1(H5Dcreate2(file.get(), "/ds1", dt.get(), space.get(),
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset ds2(H5Dcreate2(file.get(), "/ds2", dt.get(), space.get(),
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds1.valid());
    ASSERT_TRUE(ds2.valid());

    // Extract committed datatype oh_addr and SOHM dataspace shared metadata.
    // The second dataset reusing `space` receives H5O_SHARE_TYPE_SOHM (the
    // first gets H5O_SHARE_TYPE_HERE).
    H5O_loc_t* dt_oloc = H5O_get_loc(dt.get());
    H5O_loc_t* ds2_oloc = H5O_get_loc(ds2.get());
    ASSERT_NE(dt_oloc, nullptr);
    ASSERT_NE(ds2_oloc, nullptr);
    H5F_t* f = dt_oloc->file;
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    auto* decoded_ds_ext = static_cast<H5S_extent_t*>(
        H5O_msg_read(ds2_oloc, H5O_SDSPACE_ID, nullptr));
    ASSERT_NE(decoded_ds_ext, nullptr);
    absl::Cleanup free_ext = [&] {
      H5O_msg_free(H5O_SDSPACE_ID, decoded_ds_ext);
    };
    const H5O_shared_t real_sohm = decoded_ds_ext->sh_loc;
    ASSERT_EQ(real_sohm.type, static_cast<unsigned>(H5O_SHARE_TYPE_SOHM));

    // H5O__shared_size & H5O__shared_encode & H5O_set_shared for COMMITTED.
    H5O_shared_t sh_comm{};
    sh_comm.type = H5O_SHARE_TYPE_COMMITTED;
    sh_comm.file = f;
    sh_comm.msg_type_id = H5O_DTYPE_ID;
    sh_comm.u.loc.oh_addr = dt_oloc->addr;

    H5O_shared_t copied_sh{};
    ASSERT_GE(H5O_set_shared(&copied_sh, &sh_comm), 0);
    EXPECT_EQ(copied_sh.type, sh_comm.type);
    EXPECT_EQ(copied_sh.u.loc.oh_addr, sh_comm.u.loc.oh_addr);

    EXPECT_EQ(H5O__shared_size(f, &sh_comm), 2 + addr_sz);
    std::vector<uint8_t> enc_v2(H5O__shared_size(f, &sh_comm), 0);
    ASSERT_GE(H5O__shared_encode(f, enc_v2.data(), &sh_comm), 0);
    EXPECT_EQ(enc_v2[0], 2u);
    EXPECT_EQ(enc_v2[1], static_cast<uint8_t>(H5O_SHARE_TYPE_COMMITTED));

    // Decode v2 committed datatype -> resolves via H5O__shared_read(COMMITTED).
    unsigned ioflags = 0;
    void* dec_v2 = H5O__shared_decode(f, nullptr, &ioflags, enc_v2.size(),
                                      enc_v2.data(), H5O_MSG_DTYPE);
    ASSERT_NE(dec_v2, nullptr);
    H5O_msg_free(H5O_DTYPE_ID, dec_v2);

    // Decode v3 committed datatype directly.
    std::vector<uint8_t> enc_v3_comm = enc_v2;
    enc_v3_comm[0] = 3u;
    void* dec_v3_comm = H5O__shared_decode(
        f, nullptr, &ioflags, enc_v3_comm.size(), enc_v3_comm.data(),
        H5O_MSG_DTYPE);
    ASSERT_NE(dec_v3_comm, nullptr);
    H5O_msg_free(H5O_DTYPE_ID, dec_v3_comm);

    // Decode v1 committed datatype: [ver=1, unused=0, 6 reserved, size_sz lheap,
    // addr_sz oh_addr].
    std::vector<uint8_t> enc_v1(8 + 2 * addr_sz, 0);
    enc_v1[0] = 1u;
    enc_v1[1] = 0x5au;  // Unused in v1; ignored and set to COMMITTED.
    EncodeLittleEndian(dt_oloc->addr, addr_sz, enc_v1.data() + 8 + addr_sz);
    void* dec_v1 = H5O__shared_decode(f, nullptr, &ioflags, enc_v1.size(),
                                      enc_v1.data(), H5O_MSG_DTYPE);
    ASSERT_NE(dec_v1, nullptr);
    H5O_msg_free(H5O_DTYPE_ID, dec_v1);

    // H5O__shared_size & H5O__shared_encode & H5O__shared_decode for SOHM (v3).
    EXPECT_EQ(H5O__shared_size(f, &real_sohm), 2u + H5O_FHEAP_ID_LEN);
    std::vector<uint8_t> enc_sohm(H5O__shared_size(f, &real_sohm), 0);
    ASSERT_GE(H5O__shared_encode(f, enc_sohm.data(), &real_sohm), 0);
    EXPECT_EQ(enc_sohm[0], 3u);
    EXPECT_EQ(enc_sohm[1], static_cast<uint8_t>(H5O_SHARE_TYPE_SOHM));

    void* dec_sohm = H5O__shared_decode(f, nullptr, &ioflags, enc_sohm.size(),
                                        enc_sohm.data(), H5O_MSG_SDSPACE);
    ASSERT_NE(dec_sohm, nullptr);
    H5O_msg_free(H5O_SDSPACE_ID, dec_sohm);

    // Error paths inside H5O__shared_decode (overflows & bad versions).
    ScopedErrorSilence silence;
    std::array<uint8_t, 2> dummy{0u, 0u};
    EXPECT_EQ(
        H5O__shared_decode(f, nullptr, &ioflags, 0, dummy.data() + 1,
                           H5O_MSG_DTYPE),
        nullptr);

    for (const uint8_t bad_ver : {uint8_t{0}, uint8_t{4}}) {
      std::array<uint8_t, 12> bad = {bad_ver};
      EXPECT_EQ(H5O__shared_decode(f, nullptr, &ioflags, bad.size(), bad.data(),
                                   H5O_MSG_DTYPE),
                nullptr);
    }

    // Truncated before type byte (p_size == 1).
    EXPECT_EQ(H5O__shared_decode(f, nullptr, &ioflags, 1, enc_v2.data(),
                                 H5O_MSG_DTYPE),
              nullptr);

    // V1 truncations: 6 reserved bytes (< 8), lheap addr (< 8 + addr_sz),
    // and oh_addr (< 8 + 2 * addr_sz).
    for (const size_t len : {size_t{2}, size_t{7}, size_t{8}, 8 + addr_sz}) {
      EXPECT_EQ(H5O__shared_decode(f, nullptr, &ioflags, len, enc_v1.data(),
                                   H5O_MSG_DTYPE),
                nullptr);
    }

    // V2 committed truncated in oh_addr (< 2 + addr_sz).
    EXPECT_EQ(H5O__shared_decode(f, nullptr, &ioflags, 2, enc_v2.data(),
                                 H5O_MSG_DTYPE),
              nullptr);

    // V3 SOHM truncated in heap_id (< 2 + H5O_FHEAP_ID_LEN).
    EXPECT_EQ(H5O__shared_decode(f, nullptr, &ioflags, 2 + H5O_FHEAP_ID_LEN - 1,
                                 enc_sohm.data(), H5O_MSG_SDSPACE),
              nullptr);
  }
}

TEST(H5OsharedTest, SelfReferentialCommittedDatatypeAndSharedReadErrors) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
  // Put DTYPE and SDSPACE in the same fractal heap so cross-type SOHM
  // decoding triggers type->decode failure cleanly.
  ASSERT_GE(H5Pset_shared_mesg_index(
                fcpl.get(), 0, H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_SDSPACE_FLAG,
                1),
            0);

  ScopedFile file =
      CreateInMemoryFile("shared_self_ref.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  // 1. Committed datatype with an attribute whose datatype IS itself ->
  // exercises open_oh != NULL && oloc.addr == H5O_OH_GET_ADDR(open_oh) in
  // H5O__shared_read and H5O__shared_link_adj on flush/reopen/modify/delete.
  ScopedType dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/self_dt", dt.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr self_attr(H5Acreate2(dt.get(), "self_attr", dt.get(), scalar.get(),
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(self_attr.valid());
  const int v = 42;
  ASSERT_GE(H5Awrite(self_attr.get(), H5T_NATIVE_INT, &v), 0);
  self_attr.reset();

  // Also create a group (no DTYPE message) for committed read error paths.
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/empty_grp", H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // Large compound type (> 128B stack buf in H5O__shared_read) + 3D dataspace.
  ScopedType big_dt(H5Tcreate(H5T_COMPOUND, 128 * sizeof(int)));
  ASSERT_TRUE(big_dt.valid());
  for (int i = 0; i < 32; ++i) {
    const std::string field = "f_" + std::to_string(i);
    ASSERT_GE(H5Tinsert(big_dt.get(), field.c_str(),
                        static_cast<size_t>(i) * 4 * sizeof(int),
                        H5T_NATIVE_INT),
              0);
  }
  const std::array<hsize_t, 2> ddims = {4, 5};
  ScopedSpace dspace(H5Screate_simple(2, ddims.data(), nullptr));
  ASSERT_TRUE(dspace.valid());
  ScopedDataset ds_big(H5Dcreate2(file.get(), "/ds_big", big_dt.get(),
                                  dspace.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ScopedDataset ds_big2(H5Dcreate2(file.get(), "/ds_big2", big_dt.get(),
                                   dspace.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
  ASSERT_TRUE(ds_big.valid());
  ASSERT_TRUE(ds_big2.valid());

  // Snapshot and reopen read-only to decode both committed self-ref attribute
  // and >128B SOHM datatype from disk.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t img_len = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(img_len, 0);
  std::vector<uint8_t> img(static_cast<size_t>(img_len));
  ASSERT_EQ(H5Fget_file_image(file.get(), img.data(), img.size()), img_len);

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), img.data(), img.size()), 0);
  ScopedFile ro(H5Fopen("self_ref_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro.valid());

  ScopedType ro_dt(H5Topen2(ro.get(), "/self_dt", H5P_DEFAULT));
  ASSERT_TRUE(ro_dt.valid());
  ScopedAttr ro_attr(H5Aopen(ro_dt.get(), "self_attr", H5P_DEFAULT));
  ASSERT_TRUE(ro_attr.valid());
  int got_v = 0;
  ASSERT_GE(H5Aread(ro_attr.get(), H5T_NATIVE_INT, &got_v), 0);
  EXPECT_EQ(got_v, v);
  ro_attr.reset();
  ro_dt.reset();

  ScopedDataset ro_ds_big(H5Dopen2(ro.get(), "/ds_big2", H5P_DEFAULT));
  ASSERT_TRUE(ro_ds_big.valid());
  ScopedType ro_big_dt(H5Dget_type(ro_ds_big.get()));
  ASSERT_TRUE(ro_big_dt.valid());
  EXPECT_EQ(H5Tget_nmembers(ro_big_dt.get()), 32);

  // Direct H5O__shared_read success with matching open_oh AND error paths.
  H5O_loc_t* dt_oloc = H5O_get_loc(dt.get());
  H5O_loc_t* grp_oloc = H5O_get_loc(grp.get());
  H5O_loc_t* ds_big2_oloc = H5O_get_loc(ds_big2.get());
  ASSERT_NE(dt_oloc, nullptr);
  ASSERT_NE(grp_oloc, nullptr);
  ASSERT_NE(ds_big2_oloc, nullptr);
  H5F_t* f = dt_oloc->file;

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5T_t* big_dt_native =
      static_cast<H5T_t*>(H5O_msg_read(ds_big2_oloc, H5O_DTYPE_ID, nullptr));
  ASSERT_NE(big_dt_native, nullptr);
  absl::Cleanup free_big_dt = [&] { H5O_msg_free(H5O_DTYPE_ID, big_dt_native); };
  const H5O_shared_t big_sohm_sh =
      *reinterpret_cast<const H5O_shared_t*>(big_dt_native);
  ASSERT_EQ(big_sohm_sh.type, static_cast<unsigned>(H5O_SHARE_TYPE_SOHM));

  unsigned ioflags = 0;
  std::vector<uint8_t> enc_buf(2 + std::max<size_t>(H5F_SIZEOF_ADDR(f),
                                                    H5O_FHEAP_ID_LEN),
                               0);

  // Protect /self_dt oh -> open_oh matches dt_oloc->addr in H5O__shared_read.
  {
    H5O_t* dt_oh = H5O_protect(dt_oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(dt_oh, nullptr);
    absl::Cleanup unprot = [&] {
      H5O_unprotect(dt_oloc, dt_oh, H5AC__NO_FLAGS_SET);
    };
    H5O_shared_t self_sh{};
    self_sh.type = H5O_SHARE_TYPE_COMMITTED;
    self_sh.u.loc.oh_addr = dt_oloc->addr;
    ASSERT_GE(H5O__shared_encode(f, enc_buf.data(), &self_sh), 0);
    void* dec_self = H5O__shared_decode(
        f, dt_oh, &ioflags, H5O__shared_size(f, &self_sh), enc_buf.data(),
        H5O_MSG_DTYPE);
    ASSERT_NE(dec_self, nullptr);
    H5O_msg_free(H5O_DTYPE_ID, dec_self);
  }

  ScopedErrorSilence silence;

  // Committed read failure when open_oh matches group lacking DTYPE message.
  {
    H5O_t* grp_oh = H5O_protect(grp_oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(grp_oh, nullptr);
    absl::Cleanup unprot = [&] {
      H5O_unprotect(grp_oloc, grp_oh, H5AC__NO_FLAGS_SET);
    };
    H5O_shared_t grp_sh{};
    grp_sh.type = H5O_SHARE_TYPE_COMMITTED;
    grp_sh.u.loc.oh_addr = grp_oloc->addr;
    ASSERT_GE(H5O__shared_encode(f, enc_buf.data(), &grp_sh), 0);
    EXPECT_EQ(H5O__shared_decode(f, grp_oh, &ioflags,
                                 H5O__shared_size(f, &grp_sh), enc_buf.data(),
                                 H5O_MSG_DTYPE),
              nullptr);
  }

  // Committed read failure with open_oh == nullptr & bogus oh_addr.
  {
    H5O_shared_t bad_comm{};
    bad_comm.type = H5O_SHARE_TYPE_COMMITTED;
    bad_comm.u.loc.oh_addr = 0xdeadbeef00ULL;
    ASSERT_GE(H5O__shared_encode(f, enc_buf.data(), &bad_comm), 0);
    EXPECT_EQ(
        H5O__shared_decode(f, nullptr, &ioflags, H5O__shared_size(f, &bad_comm),
                           enc_buf.data(), H5O_MSG_DTYPE),
        nullptr);
  }

  // SOHM decode error paths:
  // (a) Type not in SOHM table -> H5SM_get_fheap_addr < 0.
  ASSERT_GE(H5O__shared_encode(f, enc_buf.data(), &big_sohm_sh), 0);
  EXPECT_EQ(
      H5O__shared_decode(f, nullptr, &ioflags, H5O__shared_size(f, &big_sohm_sh),
                         enc_buf.data(), H5O_MSG_PLINE),
      nullptr);

  // (b) Cross-type heap_id (datatype bytes decoded with H5O_MSG_SDSPACE) ->
  // type->decode returns NULL cleanly.
  EXPECT_EQ(
      H5O__shared_decode(f, nullptr, &ioflags, H5O__shared_size(f, &big_sohm_sh),
                         enc_buf.data(), H5O_MSG_SDSPACE),
      nullptr);

  // (c) Bogus heap ID flags -> H5HF_get_obj_len fails.
  H5O_shared_t bad_ver_sohm = big_sohm_sh;
  bad_ver_sohm.u.heap_id.id[0] = 0xffu;
  ASSERT_GE(H5O__shared_encode(f, enc_buf.data(), &bad_ver_sohm), 0);
  EXPECT_EQ(H5O__shared_decode(f, nullptr, &ioflags,
                               H5O__shared_size(f, &bad_ver_sohm),
                               enc_buf.data(), H5O_MSG_DTYPE),
            nullptr);

  // (d) Valid heap ID flags/length, huge bogus offset -> H5HF_read fails.
  H5O_shared_t bad_off_sohm = big_sohm_sh;
  bad_off_sohm.u.heap_id.id[1] = 0xffu;
  bad_off_sohm.u.heap_id.id[2] = 0xffu;
  bad_off_sohm.u.heap_id.id[3] = 0xffu;
  ASSERT_GE(H5O__shared_encode(f, enc_buf.data(), &bad_off_sohm), 0);
  EXPECT_EQ(H5O__shared_decode(f, nullptr, &ioflags,
                               H5O__shared_size(f, &bad_off_sohm),
                               enc_buf.data(), H5O_MSG_DTYPE),
            nullptr);

  // (e) Valid SOHM file before heap creation -> fheap_addr == HADDR_UNDEF ->
  // H5HF_open fails.
  ScopedPlist empty_sohm_fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(empty_sohm_fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(empty_sohm_fcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(empty_sohm_fcpl.get(), 0,
                                     H5O_SHMESG_ATTR_FLAG, 1),
            0);
  ScopedFile empty_sohm_file = CreateInMemoryFile(
      "empty_sohm.h5", H5F_ACC_TRUNC, empty_sohm_fcpl.get());
  ASSERT_TRUE(empty_sohm_file.valid());
  H5F_t* f_empty_sohm = UnwrapFile(empty_sohm_file.get());
  ASSERT_NE(f_empty_sohm, nullptr);
  ASSERT_GE(H5O__shared_encode(f_empty_sohm, enc_buf.data(), &big_sohm_sh), 0);
  EXPECT_EQ(H5O__shared_decode(f_empty_sohm, nullptr, &ioflags,
                               H5O__shared_size(f_empty_sohm, &big_sohm_sh),
                               enc_buf.data(), H5O_MSG_ATTR),
            nullptr);

  // Delete self-referential attribute to run self-referential delete path.
  self_attr.reset();
  dt.reset();
  ASSERT_GE(H5Lcreate_hard(file.get(), "/self_dt", file.get(), "/self_dt_link",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedType dt_rw(H5Topen2(file.get(), "/self_dt", H5P_DEFAULT));
  ASSERT_TRUE(dt_rw.valid());
  ASSERT_GE(H5Adelete(dt_rw.get(), "self_attr"), 0);
}

TEST(H5OsharedTest, LinkAndDeleteAdjustmentsAndCopyFileBetweenFiles) {
  ScopedPlist sohm_fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(sohm_fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(sohm_fcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(
                sohm_fcpl.get(), 0, H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_SDSPACE_FLAG,
                1),
            0);

  ScopedFile src_file =
      CreateInMemoryFile("shared_link_src.h5", H5F_ACC_TRUNC, sohm_fcpl.get());
  ScopedFile dst_sohm = CreateInMemoryFile("shared_link_dst_s.h5",
                                           H5F_ACC_TRUNC, sohm_fcpl.get());
  ScopedFile dst_plain = CreateInMemoryFile("shared_link_dst_p.h5");
  ASSERT_TRUE(src_file.valid());
  ASSERT_TRUE(dst_sohm.valid());
  ASSERT_TRUE(dst_plain.valid());

  // Committed datatype linked by extra hard link so open_oh delete tests keep
  // nlink >= 1 cleanly.
  ScopedType comm_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(comm_dt.valid());
  ASSERT_GE(H5Tcommit2(src_file.get(), "/comm_dt", comm_dt.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_hard(src_file.get(), "/comm_dt", src_file.get(),
                           "/comm_dt2", H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Datasets using committed datatype and SOHM dataspace.
  const std::array<hsize_t, 2> dims = {3, 4};
  ScopedSpace sp(H5Screate_simple(2, dims.data(), nullptr));
  ASSERT_TRUE(sp.valid());
  ScopedDataset ds(H5Dcreate2(src_file.get(), "/ds", comm_dt.get(), sp.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds.valid());
  // Add an attribute on /ds with the committed datatype.
  ScopedAttr ds_attr(H5Acreate2(ds.get(), "a_dt", comm_dt.get(), sp.get(),
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds_attr.valid());
  ds_attr.reset();
  ds.reset();

  // End-to-end H5Ocopy of dataset with committed datatype & SOHM dataspace
  // both to a SOHM-enabled destination and to a non-SOHM destination.
  ASSERT_GE(H5Ocopy(src_file.get(), "/ds", dst_sohm.get(), "/ds_copied",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/ds", dst_plain.get(), "/ds_copied",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Copy again with the same committed datatype path to test H5O_copy_header_map
  // cache hit & destination SOHM dedup.
  ASSERT_GE(H5Ocopy(src_file.get(), "/ds", dst_sohm.get(), "/ds_copied2",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  H5O_loc_t* dt_oloc = H5O_get_loc(comm_dt.get());
  ASSERT_NE(dt_oloc, nullptr);
  H5F_t* f_src = dt_oloc->file;
  H5F_t* f_dst = UnwrapFile(dst_sohm.get());
  ASSERT_NE(f_src, nullptr);
  ASSERT_NE(f_dst, nullptr);

  ScopedApiContext ctx(H5AC__IGNORE_TAG);
  ASSERT_TRUE(ctx.valid());

  // Direct H5O__shared_link and H5O__shared_delete for COMMITTED (both
  // open_oh == nullptr and matching open_oh).
  H5O_shared_t sh_comm{};
  sh_comm.type = H5O_SHARE_TYPE_COMMITTED;
  sh_comm.file = f_src;
  sh_comm.msg_type_id = H5O_DTYPE_ID;
  sh_comm.u.loc.oh_addr = dt_oloc->addr;

  EXPECT_GE(H5O__shared_link(f_src, nullptr, H5O_MSG_DTYPE, &sh_comm), 0);
  EXPECT_GE(H5O__shared_delete(f_src, nullptr, H5O_MSG_DTYPE, &sh_comm), 0);

  {
    H5O_t* dt_oh = H5O_protect(dt_oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(dt_oh, nullptr);
    absl::Cleanup unprot = [&] {
      H5O_unprotect(dt_oloc, dt_oh, H5AC__NO_FLAGS_SET);
    };
    EXPECT_GE(H5O__shared_link(f_src, dt_oh, H5O_MSG_DTYPE, &sh_comm), 0);
    EXPECT_GE(H5O__shared_delete(f_src, dt_oh, H5O_MSG_DTYPE, &sh_comm), 0);
  }

  // Error paths in link/delete/copy_file/post_copy_file.
  ScopedErrorSilence silence;

  // 1. Committed link & delete failures when oh_addr is invalid (open_oh ==
  //    nullptr -> H5O_link fails).
  H5O_shared_t bad_comm = sh_comm;
  bad_comm.u.loc.oh_addr = 0xdeadbeef00ULL;
  EXPECT_LT(H5O__shared_link(f_src, nullptr, H5O_MSG_DTYPE, &bad_comm), 0);
  EXPECT_LT(H5O__shared_delete(f_src, nullptr, H5O_MSG_DTYPE, &bad_comm), 0);

  // 2. Committed H5O__link_oh failure when open_oh has nlink == 0 and adjust <
  //    0: temporarily drop link count to 0 cleanly via H5O_link(-rc), then
  //    call H5O__shared_delete, then restore link count before cleanup.
  {
    unsigned rc_before = 0;
    H5O_type_t otype = H5O_TYPE_UNKNOWN;
    ASSERT_GE(H5O_get_rc_and_type(dt_oloc, &rc_before, &otype), 0);
    ASSERT_GT(rc_before, 0u);
    ASSERT_GE(H5O_link(dt_oloc, -static_cast<int>(rc_before)), 0);
    H5O_t* dt_oh = H5O_protect(dt_oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(dt_oh, nullptr);
    EXPECT_LT(H5O__shared_delete(f_src, dt_oh, H5O_MSG_DTYPE, &sh_comm), 0);
    ASSERT_GE(H5O_unprotect(dt_oloc, dt_oh, H5AC__NO_FLAGS_SET), 0);
    ASSERT_GE(H5O_link(dt_oloc, static_cast<int>(rc_before)), 0);
  }

  // 3. Committed post_copy_file failure when H5O_copy_header_map fails on
  //    invalid src oh_addr.
  {
    H5O_shared_t dst_sh{};
    unsigned mflags = H5O_MSG_FLAG_SHARED;
    H5O_copy_t cpy_info{};
    cpy_info.file_dst = f_dst;
    cpy_info.map_list = H5SL_create(H5SL_TYPE_OBJ, nullptr);
    ASSERT_NE(cpy_info.map_list, nullptr);
    absl::Cleanup destroy_map = [&] {
      H5SL_destroy(cpy_info.map_list, nullptr, nullptr);
    };
    EXPECT_LT(H5O__shared_post_copy_file(f_dst, H5O_MSG_DTYPE, &bad_comm,
                                         &dst_sh, &mflags, &cpy_info),
              0);
  }

  // 4. SOHM errors triggered cleanly by pointing f_dst's SOHM master table
  //    address at an unreachable offset and restoring it on exit.
  {
    ScopedDataset ds_ro_src(
        H5Dopen2(dst_sohm.get(), "/ds_copied2", H5P_DEFAULT));
    ASSERT_TRUE(ds_ro_src.valid());
    H5O_loc_t* ds_copied_oloc = H5O_get_loc(ds_ro_src.get());
    ASSERT_NE(ds_copied_oloc, nullptr);
    H5S_extent_t* ext_msg = static_cast<H5S_extent_t*>(
        H5O_msg_read(ds_copied_oloc, H5O_SDSPACE_ID, nullptr));
    ASSERT_NE(ext_msg, nullptr);
    absl::Cleanup free_ext_msg = [&] { H5O_msg_free(H5O_SDSPACE_ID, ext_msg); };

    H5O_shared_t valid_sohm_sh = ext_msg->sh_loc;
    ASSERT_EQ(valid_sohm_sh.type, static_cast<unsigned>(H5O_SHARE_TYPE_SOHM));

    // Delete with bogus heap_id fails H5SM_delete.
    H5O_shared_t bad_sohm_sh = valid_sohm_sh;
    bad_sohm_sh.u.heap_id.val ^= 0xffffffff00000001ULL;
    EXPECT_LT(
        H5O__shared_delete(f_dst, nullptr, H5O_MSG_SDSPACE, &bad_sohm_sh), 0);

    // Point SOHM master table addr to invalid offset temporarily so
    // H5SM_try_share fails in link(SOHM), copy_file(SOHM), post_copy_file.
    const haddr_t saved_sohm_addr = H5F_SOHM_ADDR(f_dst);
    H5F_SET_SOHM_ADDR(f_dst, 0xdeadbeef00ULL);
    absl::Cleanup restore_sohm = [&] {
      H5F_SET_SOHM_ADDR(f_dst, saved_sohm_addr);
    };

    EXPECT_LT(
        H5O__shared_link(f_dst, nullptr, H5O_MSG_SDSPACE, &valid_sohm_sh), 0);

    bool recompute = false;
    unsigned mflags = 0;
    H5O_copy_t cpy_info{};
    cpy_info.file_dst = f_dst;
    cpy_info.map_list = H5SL_create(H5SL_TYPE_OBJ, nullptr);
    ASSERT_NE(cpy_info.map_list, nullptr);
    absl::Cleanup destroy_map = [&] {
      H5SL_destroy(cpy_info.map_list, nullptr, nullptr);
    };
    H5S_extent_t* ext_copy = static_cast<H5S_extent_t*>(
        H5O_msg_copy(H5O_SDSPACE_ID, ext_msg, nullptr));
    ASSERT_NE(ext_copy, nullptr);
    absl::Cleanup free_ext_copy = [&] {
      H5O_msg_free(H5O_SDSPACE_ID, ext_copy);
    };

    H5O_shared_t unshared_src{};
    unshared_src.type = H5O_SHARE_TYPE_UNSHARED;
    EXPECT_LT(H5O__shared_copy_file(f_src, f_dst, H5O_MSG_SDSPACE,
                                    &unshared_src, ext_copy, &recompute,
                                    &mflags, &cpy_info, nullptr),
              0);

    ext_copy->sh_loc.type = H5O_SHARE_TYPE_HERE;
    EXPECT_LT(H5O__shared_post_copy_file(f_dst, H5O_MSG_SDSPACE, &unshared_src,
                                         &ext_copy->sh_loc, &mflags, &cpy_info),
              0);
  }
}

}  // namespace
}  // namespace hdf5
