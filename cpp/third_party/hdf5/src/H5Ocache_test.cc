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
// Tests object header (`H5AC_OHDR`) and continuation chunk (`H5AC_OHDR_CHK`)
// metadata cache callbacks, v1/v2 header serialization and deserialization,
// multi-chunk continuations, null-message merging, gaps, SWMR flush
// dependencies, unknown-message flags, and corruption error paths
// (third_party/hdf5/src/H5Ocache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ocache.c

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields in H5C_cache_entry_t
// and H5O_t. Define NDEBUG=1 around internal headers to match library ABI.
#ifndef NDEBUG
#define H5OCACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#define H5O_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5OCACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OCACHE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

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

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

std::vector<uint8_t> ExtractFileImage(hid_t file_id) {
  if (H5Fflush(file_id, H5F_SCOPE_GLOBAL) < 0) return {};
  const ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) return {};
  std::vector<uint8_t> image(static_cast<size_t>(size));
  if (H5Fget_file_image(file_id, image.data(), image.size()) != size) {
    return {};
  }
  return image;
}

ScopedFile OpenMemoryImage(const std::vector<uint8_t>& image, unsigned flags) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedFile();
  if (H5Pset_file_image(fapl.get(), const_cast<uint8_t*>(image.data()),
                        image.size()) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fopen("mem_reopen.h5", flags, fapl.get()));
}

void AppendLe16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
}

void AppendLe32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
}

void AppendLe64(std::vector<uint8_t>* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xffu));
  }
}

void AppendV2Checksum(std::vector<uint8_t>* buf) {
  const uint32_t csum = H5_checksum_metadata(buf->data(), buf->size(), 0);
  AppendLe32(buf, csum);
}

// Builds a raw serialized v2 object header prefix + chunk 0 payload + checksum.
std::vector<uint8_t> BuildRawV2Header(uint8_t version, uint8_t flags,
                                      uint64_t chunk0_size,
                                      const std::vector<uint8_t>& body,
                                      uint16_t max_compact = 8,
                                      uint16_t min_dense = 6,
                                      bool append_chksum = true) {
  std::vector<uint8_t> out = {'O', 'H', 'D', 'R', version, flags};
  if (flags & H5O_HDR_STORE_TIMES) {
    for (int i = 0; i < 4; ++i) AppendLe32(&out, 1000u + i);
  }
  if (flags & H5O_HDR_ATTR_STORE_PHASE_CHANGE) {
    AppendLe16(&out, max_compact);
    AppendLe16(&out, min_dense);
  }
  switch (flags & H5O_HDR_CHUNK0_SIZE) {
    case 0:
      out.push_back(static_cast<uint8_t>(chunk0_size));
      break;
    case 1:
      AppendLe16(&out, static_cast<uint16_t>(chunk0_size));
      break;
    case 2:
      AppendLe32(&out, static_cast<uint32_t>(chunk0_size));
      break;
    case 3:
      AppendLe64(&out, chunk0_size);
      break;
  }
  out.insert(out.end(), body.begin(), body.end());
  if (append_chksum) {
    AppendV2Checksum(&out);
  }
  return out;
}

std::vector<uint8_t> BuildRawV1Header(uint8_t version, uint16_t nmesgs,
                                      uint32_t chunk0_size,
                                      const std::vector<uint8_t>& body) {
  std::vector<uint8_t> out;
  out.push_back(version);
  out.push_back(0);  // reserved
  AppendLe16(&out, nmesgs);
  AppendLe32(&out, 1);  // nlink
  AppendLe32(&out, chunk0_size);
  AppendLe32(&out, 0);  // 4 reserved bytes -> 16-byte v1 prefix
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

TEST(H5OcacheTest, V1MultiChunkReloadNullMergingAndExpunge) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
      0);

  ScopedFile file(H5Fcreate("v1_chunks.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "d", H5T_NATIVE_INT, scalar.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // Force many v1 continuation chunks (> H5O_NCHUNKS) so H5O__add_cont_msg
  // grows cont_msg_info->msgs and oh->chunk grows beyond initial capacity.
  // Interleave small dummy datasets + flush so chunks cannot extend in place.
  for (int i = 0; i < 60; ++i) {
    const std::string name = absl::StrCat("attr_long_name_", i);
    ScopedAttr attr(H5Acreate2(dset.get(), name.c_str(), H5T_NATIVE_INT,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &i), 0);
    if (i % 12 == 11) {
      const std::string pad_name = absl::StrCat("pad_", i);
      ScopedDataset pad(H5Dcreate2(file.get(), pad_name.c_str(), H5T_NATIVE_INT,
                                   scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
      ASSERT_TRUE(pad.valid());
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
    }
  }
  dset.reset();

  const std::vector<uint8_t> image1 = ExtractFileImage(file.get());
  ASSERT_FALSE(image1.empty());
  ASSERT_GE(H5Fclose(file.release()), 0);

  // Reopen RDWR, verify attributes, and delete adjacent attributes so multiple
  // NULL messages appear sequentially in the chunks.
  ScopedFile r2 = OpenMemoryImage(image1, H5F_ACC_RDWR);
  ASSERT_TRUE(r2.valid());
  ScopedDataset d2(H5Dopen2(r2.get(), "d", H5P_DEFAULT));
  ASSERT_TRUE(d2.valid());
  for (int i = 1; i <= 6; ++i) {
    const std::string name = absl::StrCat("attr_long_name_", i);
    ASSERT_GE(H5Adelete(d2.get(), name.c_str()), 0);
  }
  d2.reset();

  const std::vector<uint8_t> image2 = ExtractFileImage(r2.get());
  ASSERT_FALSE(image2.empty());
  ASSERT_GE(H5Fclose(r2.release()), 0);

  // Reopen RDWR again to trigger NULL-message merging and cache expunge/reload.
  ScopedFile r3 = OpenMemoryImage(image2, H5F_ACC_RDWR);
  ASSERT_TRUE(r3.valid());
  ScopedDataset d3(H5Dopen2(r3.get(), "d", H5P_DEFAULT));
  ASSERT_TRUE(d3.valid());
  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(d3.get(), &oinfo, H5O_INFO_BASIC), 0);
  H5O_native_info_t ninfo{};
  ASSERT_GE(H5Oget_native_info(d3.get(), &ninfo, H5O_NATIVE_INFO_HDR), 0);
  EXPECT_GT(ninfo.hdr.nchunks, 2u);

  {
    ScopedApiContext ctx;
    H5O_loc_t* oloc = H5O_get_loc(d3.get());
    ASSERT_NE(oloc, nullptr);
    ASSERT_GE(H5O__expunge_chunks_test(oloc), 0);
  }
  ScopedAttr check(H5Aopen(d3.get(), "attr_long_name_50", H5P_DEFAULT));
  ASSERT_TRUE(check.valid());
  int v = 0;
  ASSERT_GE(H5Aread(check.get(), H5T_NATIVE_INT, &v), 0);
  EXPECT_EQ(v, 50);
  check.reset();
  d3.reset();
  ASSERT_GE(H5Fclose(r3.release()), 0);
}

TEST(H5OcacheTest, V2ChunkSizesFlagsAndGaps) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

  ScopedFile file(H5Fcreate("v2_variants.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  // 1) chunk0 size encoding case 0 (1-byte), track times OFF.
  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_obj_track_times(gcpl.get(), false), 0);
    ScopedGroup g(
        H5Gcreate2(file.get(), "no_times", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }

  // 2) Phase change + creation order tracked + multi-chunk + gap creation.
  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 65, 65), 0);
    ASSERT_GE(
        H5Pset_attr_creation_order(gcpl.get(), H5P_CRT_ORDER_TRACKED), 0);
    ScopedGroup g(
        H5Gcreate2(file.get(), "multi_v2", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());

    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    for (int i = 0; i < 45; ++i) {
      const std::string name = absl::StrCat("v2_", i, (i % 3 == 0 ? "_xx" : ""));
      ScopedAttr a(H5Acreate2(g.get(), name.c_str(), H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
      ASSERT_GE(H5Awrite(a.get(), H5T_NATIVE_INT, &i), 0);
      if (i % 15 == 14) {
        const std::string pad_name = absl::StrCat("v2_pad_", i);
        ScopedDataset pad(H5Dcreate2(file.get(), pad_name.c_str(),
                                     H5T_NATIVE_INT, scalar.get(), H5P_DEFAULT,
                                     H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(pad.valid());
        ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
      }
    }
    // Delete a few attributes and add different-sized ones to encourage chunk
    // gaps and NULL messages.
    for (int i = 5; i < 15; i += 2) {
      const std::string name = absl::StrCat("v2_", i, (i % 3 == 0 ? "_xx" : ""));
      ASSERT_GE(H5Adelete(g.get(), name.c_str()), 0);
    }
    for (int i = 0; i < 4; ++i) {
      const std::string name = absl::StrCat("gap_", i);
      ScopedAttr a(H5Acreate2(g.get(), name.c_str(), H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
    }
  }

  // 3) Compact dataset >= 256 bytes -> chunk0 size encoding case 1 (2-byte),
  // and compact dataset + large fill value >= 65536 bytes -> case 2 (4-byte).
  {
    const hsize_t dim2[1] = {512};
    ScopedSpace sp2(H5Screate_simple(1, dim2, nullptr));
    ScopedPlist dcpl2(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_layout(dcpl2.get(), H5D_COMPACT), 0);
    ScopedDataset d2(H5Dcreate2(file.get(), "compact_2b", H5T_NATIVE_UINT8,
                                sp2.get(), H5P_DEFAULT, dcpl2.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(d2.valid());

    const hsize_t dim4[1] = {40000};
    ScopedSpace sp4(H5Screate_simple(1, dim4, nullptr));
    ScopedPlist dcpl4(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_layout(dcpl4.get(), H5D_COMPACT), 0);
    ScopedDataset d4(H5Dcreate2(file.get(), "compact_4b_base", H5T_NATIVE_UINT8,
                                sp4.get(), H5P_DEFAULT, dcpl4.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(d4.valid());
    const hsize_t adim[1] = {30000};
    ScopedSpace aspace(H5Screate_simple(1, adim, nullptr));
    ScopedAttr big_a(H5Acreate2(d4.get(), "big_a", H5T_NATIVE_UINT8,
                                aspace.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(big_a.valid());
  }

  const std::vector<uint8_t> image = ExtractFileImage(file.get());
  ASSERT_FALSE(image.empty());
  ASSERT_GE(H5Fclose(file.release()), 0);

  ScopedFile reopened = OpenMemoryImage(image, H5F_ACC_RDWR);
  ASSERT_TRUE(reopened.valid());

  ScopedGroup no_times(H5Gopen2(reopened.get(), "no_times", H5P_DEFAULT));
  ASSERT_TRUE(no_times.valid());
  ScopedGroup multi_v2(H5Gopen2(reopened.get(), "multi_v2", H5P_DEFAULT));
  ASSERT_TRUE(multi_v2.valid());

  H5O_native_info_t ninfo{};
  ASSERT_GE(H5Oget_native_info(multi_v2.get(), &ninfo, H5O_NATIVE_INFO_HDR), 0);
  EXPECT_GE(ninfo.hdr.nchunks, 2u);

  multi_v2.reset();
  no_times.reset();
  ASSERT_GE(H5Fflush(reopened.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5Fclose(reopened.release()), 0);
}

TEST(H5OcacheTest, SwmrWriteFlushDependenciesAndEviction) {
  // SWMR requires a real filesystem driver (Sec2 via TempFile).
  TempFile swmr_file("ohdr_swmr.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
      0);

  ScopedFile file(H5Fcreate(swmr_file.c_str(),
                            H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_attr_phase_change(dcpl.get(), 65, 65), 0);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "swmr_d", H5T_NATIVE_INT,
                                scalar.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // Enough compact attributes to span >= 3 object header chunks so both oh->chk
  // and chk->chk flush dependencies are registered and torn down. Interleave
  // small dummy datasets + flush so chunks cannot extend in place.
  for (int i = 0; i < 60; ++i) {
    const std::string name = absl::StrCat("swmr_a_", i);
    ScopedAttr a(H5Acreate2(dset.get(), name.c_str(), H5T_NATIVE_INT,
                            scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a.valid());
    if (i % 12 == 11) {
      const std::string pad_name = absl::StrCat("swmr_pad_", i);
      ScopedDataset pad(H5Dcreate2(file.get(), pad_name.c_str(),
                                   H5T_NATIVE_INT, scalar.get(), H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(pad.valid());
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
    }
  }
  dset.reset();
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  file.reset();

  // Reopen in SWMR write mode so cache loads `oh` and chunks with
  // H5F_ACC_SWMR_WRITE active, then close to test flush dependency teardown.
  ScopedFile reopened(
      H5Fopen(swmr_file.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
              fapl.get()));
  ASSERT_TRUE(reopened.valid());
  ScopedDataset d_re(H5Dopen2(reopened.get(), "swmr_d", H5P_DEFAULT));
  ASSERT_TRUE(d_re.valid());
  H5O_native_info_t ninfo{};
  ASSERT_GE(H5Oget_native_info(d_re.get(), &ninfo, H5O_NATIVE_INFO_HDR), 0);
  EXPECT_GT(ninfo.hdr.nchunks, 2u);
  ScopedAttr a_re(H5Aopen(d_re.get(), "swmr_a_55", H5P_DEFAULT));
  ASSERT_TRUE(a_re.valid());
  int v = 77;
  ASSERT_GE(H5Awrite(a_re.get(), H5T_NATIVE_INT, &v), 0);
  a_re.reset();
  d_re.reset();
  ASSERT_GE(H5Fclose(reopened.release()), 0);
}

TEST(H5OcacheTest, DirectOhdrCallbacksValidV2VariantsAndNotifyActions) {
  ScopedFile file = CreateInMemoryFile("direct_ohdr_valid.h5");
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;

  // Test get_initial_load_size & get_final_load_size.
  size_t init_len = 0;
  ASSERT_GE(H5AC_OHDR->get_initial_load_size(nullptr, &init_len), 0);
  EXPECT_EQ(init_len, static_cast<size_t>(H5O_SPEC_READ_SIZE));

  // Build valid minimal v2 header with 8-byte chunk0_size (flags case 3) and
  // zero messages to exercise 8-byte chunk0 decode + serialize + image_len.
  const std::vector<uint8_t> raw8 =
      BuildRawV2Header(/*version=*/2, /*flags=*/0x03, /*chunk0_size=*/0, {});

  H5O_cont_msgs_t cont_info{};
  H5O_cache_ud_t udata{};
  udata.common.f = f;
  udata.common.file_intent = H5F_ACC_RDWR;
  udata.common.cont_msg_info = &cont_info;
  udata.common.addr = 0x2000;

  size_t actual_len = raw8.size();
  ASSERT_GE(H5AC_OHDR->get_final_load_size(raw8.data(), raw8.size(), &udata,
                                           &actual_len),
            0);
  EXPECT_EQ(actual_len, raw8.size());
  EXPECT_GT(H5AC_OHDR->verify_chksum(raw8.data(), raw8.size(), &udata), 0);

  std::vector<uint8_t> bad_csum = raw8;
  bad_csum.back() ^= 0xffu;
  EXPECT_EQ(
      H5AC_OHDR->verify_chksum(bad_csum.data(), bad_csum.size(), &udata),
      0);

  // Also test v1 checksum skip branch in H5O__cache_verify_chksum.
  udata.oh_version = H5O_VERSION_1;
  EXPECT_GT(H5AC_OHDR->verify_chksum(raw8.data(), raw8.size(), &udata), 0);

  bool dirty = false;
  auto* oh = static_cast<H5O_t*>(
      H5AC_OHDR->deserialize(raw8.data(), raw8.size(), &udata, &dirty));
  ASSERT_NE(oh, nullptr);
  oh->cache_info.type = H5AC_OHDR;
  absl::Cleanup free_oh = [&] { H5AC_OHDR->free_icr(oh); };

  size_t img_len = 0;
  ASSERT_GE(H5AC_OHDR->image_len(oh, &img_len), 0);
  EXPECT_EQ(img_len, raw8.size());

  std::vector<uint8_t> reserialized(img_len, 0);
  ASSERT_GE(H5AC_OHDR->serialize(f, reserialized.data(), img_len, oh), 0);
  EXPECT_EQ(reserialized, raw8);

  // Also test 2-byte (flags=0x01) and 4-byte (flags=0x02) chunk0_size variants.
  for (uint8_t sz_flag : {uint8_t{0x01}, uint8_t{0x02}}) {
    const std::vector<uint8_t> raw_var =
        BuildRawV2Header(/*version=*/2, sz_flag, /*chunk0_size=*/0, {});
    H5O_cont_msgs_t vcont{};
    H5O_cache_ud_t vud{};
    vud.common.f = f;
    vud.common.file_intent = H5F_ACC_RDWR;
    vud.common.cont_msg_info = &vcont;
    vud.common.addr = 0x2100 + sz_flag;
    size_t vlen = raw_var.size();
    ASSERT_GE(
        H5AC_OHDR->get_final_load_size(raw_var.data(), raw_var.size(), &vud,
                                       &vlen),
        0);
    EXPECT_EQ(vlen, raw_var.size());
    EXPECT_GT(
        H5AC_OHDR->verify_chksum(raw_var.data(), raw_var.size(), &vud), 0);
    bool vdirty = false;
    auto* voh = static_cast<H5O_t*>(
        H5AC_OHDR->deserialize(raw_var.data(), raw_var.size(), &vud, &vdirty));
    ASSERT_NE(voh, nullptr);
    voh->cache_info.type = H5AC_OHDR;
    absl::Cleanup free_voh = [&] { H5AC_OHDR->free_icr(voh); };
    size_t vimg_len = 0;
    ASSERT_GE(H5AC_OHDR->image_len(voh, &vimg_len), 0);
    EXPECT_EQ(vimg_len, raw_var.size());
    std::vector<uint8_t> vser(vimg_len, 0);
    ASSERT_GE(H5AC_OHDR->serialize(f, vser.data(), vimg_len, voh), 0);
    EXPECT_EQ(vser, raw_var);
  }

  // Test non-SWMR and unknown notify actions.
  for (H5AC_notify_action_t act : {
           H5AC_NOTIFY_ACTION_AFTER_INSERT,
           H5AC_NOTIFY_ACTION_AFTER_LOAD,
           H5AC_NOTIFY_ACTION_AFTER_FLUSH,
           H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
           H5AC_NOTIFY_ACTION_ENTRY_CLEANED,
           H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
           H5AC_NOTIFY_ACTION_CHILD_CLEANED,
           H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
           H5AC_NOTIFY_ACTION_CHILD_SERIALIZED,
           H5AC_NOTIFY_ACTION_BEFORE_EVICT,
       }) {
    EXPECT_GE(H5AC_OHDR->notify(act, oh), 0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5AC_OHDR->notify(static_cast<H5AC_notify_action_t>(999), oh), 0);
  }
}

TEST(H5OcacheTest, DirectUnknownMessagesAndGapAndDeserializeErrors) {
  ScopedFile file = CreateInMemoryFile("direct_ohdr_errors.h5");
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;

  auto try_deserialize = [&](const std::vector<uint8_t>& img, unsigned intent) {
    H5O_cont_msgs_t cont_info{};
    H5O_cache_ud_t udata{};
    udata.common.f = f;
    udata.common.file_intent = intent;
    udata.common.cont_msg_info = &cont_info;
    udata.common.addr = 0x3000;
    bool dirty = false;
    void* res = H5AC_OHDR->deserialize(img.data(), img.size(), &udata, &dirty);
    if (res != nullptr) {
      auto* oh = static_cast<H5O_t*>(res);
      oh->cache_info.type = H5AC_OHDR;
      // Mark messages dirty/clean paths check.
      H5AC_OHDR->notify(H5AC_NOTIFY_ACTION_ENTRY_CLEANED, oh);
      H5AC_OHDR->free_icr(oh);
    }
    return std::make_pair(res != nullptr, dirty);
  };

  // Unknown message with MARK_IF_UNKNOWN under RDWR marks message & dirty=true.
  {
    std::vector<uint8_t> body;
    body.push_back(H5O_BOGUS_VALID_ID);  // unknown in default build
    AppendLe16(&body, 4);                // mesg_size
    body.push_back(H5O_MSG_FLAG_MARK_IF_UNKNOWN);
    AppendLe32(&body, 0x11223344u);
    auto img = BuildRawV2Header(2, 0x00, body.size(), body);
    auto [ok, dirty] = try_deserialize(img, H5F_ACC_RDWR);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(dirty);
  }

  // Gap (< H5O_SIZEOF_MSGHDR_OH) at end of non-NULL v2 chunk succeeds and
  // re-serializes with zeroed gap bytes.
  {
    std::vector<uint8_t> body;
    // Empty REFCOUNT message with 2-byte trailing gap.
    // Encode MTIME_NEW (id=0x12, len=8: ver=1,3pad,4B time) + 2-byte gap.
    body.push_back(H5O_MTIME_NEW_ID);
    AppendLe16(&body, 8);
    body.push_back(0);
    body.push_back(1);  // mtime version
    body.push_back(0);
    body.push_back(0);
    body.push_back(0);
    AppendLe32(&body, 12345u);
    body.push_back(0xaa);  // 2-byte gap (< 4)
    body.push_back(0xbb);
    auto img = BuildRawV2Header(2, 0x00, body.size(), body);
    H5O_cont_msgs_t cont_info{};
    H5O_cache_ud_t udata{};
    udata.common.f = f;
    udata.common.file_intent = H5F_ACC_RDWR;
    udata.common.cont_msg_info = &cont_info;
    udata.common.addr = 0x3100;
    bool dirty = false;
    auto* oh = static_cast<H5O_t*>(
        H5AC_OHDR->deserialize(img.data(), img.size(), &udata, &dirty));
    ASSERT_NE(oh, nullptr);
    oh->cache_info.type = H5AC_OHDR;
    absl::Cleanup free_oh = [&] { H5AC_OHDR->free_icr(oh); };
    EXPECT_EQ(oh->chunk[0].gap, 2u);
    std::vector<uint8_t> out(oh->chunk[0].size, 0xffu);
    ASSERT_GE(H5AC_OHDR->serialize(f, out.data(), out.size(), oh), 0);
    // Gap bytes before 4-byte checksum are zeroed.
    EXPECT_EQ(out[out.size() - 6], 0u);
    EXPECT_EQ(out[out.size() - 5], 0u);
  }

  // Also test old MTIME message (H5O_MTIME_ID) & REFCOUNT (H5O_REFCOUNT_ID).
  {
    std::vector<uint8_t> body;
    body.push_back(H5O_MTIME_ID);
    AppendLe16(&body, 16);  // 14 bytes "YYYYMMDDHHMMSS" + 2 reserved bytes
    body.push_back(0);
    const char ts[] = "20260101000000\0\0";
    body.insert(body.end(), ts, ts + 16);
    body.push_back(H5O_REFCOUNT_ID);
    AppendLe16(&body, 5);
    body.push_back(0);
    body.push_back(0);  // refcount version 0
    AppendLe32(&body, 3);
    auto img = BuildRawV2Header(2, 0x00, body.size(), body);
    auto [ok, dirty] = try_deserialize(img, H5F_ACC_RDONLY);
    EXPECT_TRUE(ok);
    (void)dirty;
  }

  // Also cover adjacent NULL message merging in H5F_ACC_RDWR mode.
  {
    const std::vector<uint8_t> two_v2_nulls(8, 0);
    auto img = BuildRawV2Header(2, 0x00, two_v2_nulls.size(), two_v2_nulls);
    auto [ok, dirty] = try_deserialize(img, H5F_ACC_RDWR);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(dirty);
  }

  ScopedErrorSilence silence;

  // Continuation message decoded cleanly followed by a failing unknown message
  // exercises freeing cont_msg_info->msgs on error.
  {
    std::vector<uint8_t> body;
    body.push_back(H5O_CONT_ID);
    AppendLe16(&body, 16);  // 8-byte addr + 8-byte size
    body.push_back(0);
    AppendLe64(&body, 0x9000);
    AppendLe64(&body, 64);
    body.push_back(0xfe);
    AppendLe16(&body, 0);
    body.push_back(H5O_MSG_FLAG_FAIL_IF_UNKNOWN_ALWAYS);
    auto img = BuildRawV2Header(2, 0x00, body.size(), body);
    EXPECT_FALSE(try_deserialize(img, H5F_ACC_RDONLY).first);
  }

  // V2 message payload spilling into checksum triggers overflow check at the
  // trailing checksum decode.
  {
    const std::vector<uint8_t> body = {0, 4, 0, 0, 0, 0};
    auto img = BuildRawV2Header(2, 0x00, body.size(), body);
    EXPECT_FALSE(try_deserialize(img, H5F_ACC_RDONLY).first);
  }

  // V1 truncated trailing second message headers (id=2B, size=2B, flags=1B,
  // reserved=3B) hit exact overflow checks for each field.
  for (uint32_t extra : {1u, 3u, 4u, 5u}) {
    std::vector<uint8_t> body(8 + extra, 0);
    EXPECT_FALSE(
        try_deserialize(BuildRawV1Header(1, 1, 8 + extra, body), H5F_ACC_RDONLY)
            .first);
  }

  // Verify H5F_get_checksums fails on too-short buffer (< 4 bytes).
  {
    std::array<uint8_t, 2> tiny{0, 1};
    H5O_cache_ud_t udata{};
    udata.oh_version = H5O_VERSION_2;
    EXPECT_LT(H5AC_OHDR->verify_chksum(tiny.data(), tiny.size(), &udata), 0);

    H5O_t dummy_oh{};
    dummy_oh.version = H5O_VERSION_2;
    H5O_chk_cache_ud_t chk_ud{};
    chk_ud.oh = &dummy_oh;
    EXPECT_LT(
        H5AC_OHDR_CHK->verify_chksum(tiny.data(), tiny.size(), &chk_ud), 0);
  }

  // Prefix truncation and corruption paths.
  for (size_t trunc : {1u, 4u, 5u, 6u, 10u}) {
    std::vector<uint8_t> v2_short = {'O', 'H', 'D', 'R', 2, H5O_HDR_STORE_TIMES,
                                     0,   0,   0,   0};
    v2_short.resize(trunc);
    EXPECT_FALSE(try_deserialize(v2_short, H5F_ACC_RDONLY).first);
  }
  // Bad v2 version, unknown v2 flags, bad attr phase change (max < min).
  EXPECT_FALSE(
      try_deserialize(BuildRawV2Header(3, 0, 0, {}), H5F_ACC_RDONLY).first);
  EXPECT_FALSE(
      try_deserialize(BuildRawV2Header(2, 0x80, 0, {}), H5F_ACC_RDONLY).first);
  EXPECT_FALSE(try_deserialize(
                   BuildRawV2Header(2, H5O_HDR_ATTR_STORE_PHASE_CHANGE, 0, {},
                                    /*max_compact=*/2, /*min_dense=*/5),
                   H5F_ACC_RDONLY)
                   .first);
  // Truncated attr phase change & truncated chunk0 sizes (1/2/4/8 byte).
  EXPECT_FALSE(
      try_deserialize({'O', 'H', 'D', 'R', 2, H5O_HDR_ATTR_STORE_PHASE_CHANGE,
                       1, 0},
                      H5F_ACC_RDONLY)
          .first);
  for (uint8_t csz_flag : {0u, 1u, 2u, 3u}) {
    std::vector<uint8_t> trunc_csz = {'O', 'H', 'D', 'R', 2, csz_flag};
    if (csz_flag > 0) trunc_csz.push_back(0);
    EXPECT_FALSE(try_deserialize(trunc_csz, H5F_ACC_RDONLY).first);
  }
  // Nonzero chunk0_size < H5O_SIZEOF_MSGHDR_OH.
  EXPECT_FALSE(
      try_deserialize(BuildRawV2Header(2, 0, 2, {0, 0}), H5F_ACC_RDONLY).first);

  // V1 prefix truncation/corruption paths.
  for (size_t trunc : {1u, 2u, 4u, 8u, 12u}) {
    std::vector<uint8_t> v1_short(trunc, 1);
    EXPECT_FALSE(try_deserialize(v1_short, H5F_ACC_RDONLY).first);
  }
  EXPECT_FALSE(
      try_deserialize(BuildRawV1Header(2, 0, 0, {}), H5F_ACC_RDONLY).first);
  EXPECT_FALSE(
      try_deserialize(BuildRawV1Header(1, 0, 8, std::vector<uint8_t>(8, 0)),
                      H5F_ACC_RDONLY)
          .first);
  EXPECT_FALSE(
      try_deserialize(BuildRawV1Header(1, 1, 4, std::vector<uint8_t>(4, 0)),
                      H5F_ACC_RDONLY)
          .first);
  // V1 nmesgs mismatch (v1_pfx_nmesgs = 0 vs body has 1 msg -> line 305):
  // v1_pfx_nmesgs < oh->nmesgs triggers when v1_pfx_nmesgs == 1 and 2 null msgs.
  {
    std::vector<uint8_t> two_nulls(16, 0);
    EXPECT_FALSE(
        try_deserialize(BuildRawV1Header(1, 1, 16, two_nulls), H5F_ACC_RDONLY)
            .first);
  }

  // Message header corruption paths inside H5O__chunk_deserialize.
  auto fail_v2_body = [&](const std::vector<uint8_t>& body, unsigned intent) {
    auto img = BuildRawV2Header(2, 0x00, body.size(), body);
    EXPECT_FALSE(try_deserialize(img, intent).first);
  };
  // Message size exceeds buffer end.
  fail_v2_body({0, 100, 0, 0}, H5F_ACC_RDONLY);
  // SHARED + DONTSHARE.
  fail_v2_body({0, 0, 0, H5O_MSG_FLAG_SHARED | H5O_MSG_FLAG_DONTSHARE},
               H5F_ACC_RDONLY);
  // WAS_UNKNOWN + FAIL_IF_UNKNOWN_AND_OPEN_FOR_WRITE.
  fail_v2_body({0, 0, 0,
                H5O_MSG_FLAG_WAS_UNKNOWN |
                    H5O_MSG_FLAG_FAIL_IF_UNKNOWN_AND_OPEN_FOR_WRITE},
               H5F_ACC_RDONLY);
  // WAS_UNKNOWN without MARK_IF_UNKNOWN.
  fail_v2_body({0, 0, 0, H5O_MSG_FLAG_WAS_UNKNOWN}, H5F_ACC_RDONLY);
  // Unknown message with FAIL_IF_UNKNOWN_ALWAYS & FAIL_IF_UNKNOWN_AND_WRITE.
  fail_v2_body({0xfe, 0, 0, H5O_MSG_FLAG_FAIL_IF_UNKNOWN_ALWAYS},
               H5F_ACC_RDONLY);
  fail_v2_body({0xfe, 0, 0, H5O_MSG_FLAG_FAIL_IF_UNKNOWN_AND_OPEN_FOR_WRITE},
               H5F_ACC_RDWR);
  // Unshareable class marked SHAREABLE (H5O_NAME_ID = 0x0d).
  fail_v2_body({H5O_NAME_ID, 0, 0, H5O_MSG_FLAG_SHAREABLE}, H5F_ACC_RDONLY);
  // Refcount message rejected in v1 ohdr.
  {
    std::vector<uint8_t> body = {H5O_REFCOUNT_ID, 0, 8, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_FALSE(
        try_deserialize(BuildRawV1Header(1, 1, 16, body), H5F_ACC_RDONLY)
            .first);
  }
  // Unaligned message size in v1 ohdr (size = 3 instead of multiple of 8).
  {
    std::vector<uint8_t> body = {0, 0, 3, 0, 0, 0, 0, 0, 1, 2, 3, 0, 0, 0, 0, 0};
    EXPECT_FALSE(
        try_deserialize(BuildRawV1Header(1, 1, 16, body), H5F_ACC_RDONLY)
            .first);
  }
  // Gap in chunk that also contains a NULL message (nullcnt != 0).
  fail_v2_body({0, 0, 0, 0, 0xff}, H5F_ACC_RDONLY);
  // Gap in v1 ohdr (trailing 2 bytes after valid 8-byte null message).
  {
    std::vector<uint8_t> body(10, 0);
    EXPECT_FALSE(
        try_deserialize(BuildRawV1Header(1, 1, 10, body), H5F_ACC_RDONLY)
            .first);
  }
  // Bad mtime decode.
  fail_v2_body({H5O_MTIME_ID, 2, 0, 0, 'x', 'x'}, H5F_ACC_RDONLY);
  fail_v2_body({H5O_MTIME_NEW_ID, 1, 0, 0, 99}, H5F_ACC_RDONLY);
  fail_v2_body({H5O_REFCOUNT_ID, 1, 0, 0, 99}, H5F_ACC_RDONLY);
  // Bad continuation message decode (invalid size 1).
  fail_v2_body({H5O_CONT_ID, 1, 0, 0, 0}, H5F_ACC_RDONLY);
}

TEST(H5OcacheTest, DirectChunkCacheCallbacksSerializeAndErrors) {
  ScopedFile file = CreateInMemoryFile("direct_chk.h5");
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;

  // Build valid minimal v2 ohdr to host chunks.
  const std::vector<uint8_t> raw_oh = BuildRawV2Header(2, 0x00, 0, {});
  H5O_cont_msgs_t cont_info{};
  H5O_cache_ud_t oh_ud{};
  oh_ud.common.f = f;
  oh_ud.common.file_intent = H5F_ACC_RDWR;
  oh_ud.common.cont_msg_info = &cont_info;
  oh_ud.common.addr = 0x4000;
  bool dirty = false;
  auto* oh = static_cast<H5O_t*>(
      H5AC_OHDR->deserialize(raw_oh.data(), raw_oh.size(), &oh_ud, &dirty));
  ASSERT_NE(oh, nullptr);
  oh->cache_info.type = H5AC_OHDR;
  // Keep oh->rc > 0 across chunk proxy lifecycle so H5O__inc_rc/H5O__dec_rc do
  // not attempt to pin/unpin an uncached `oh` in H5AC; reset before free_icr.
  oh->rc = 1;
  absl::Cleanup free_oh = [&] {
    oh->rc = 0;
    H5AC_OHDR->free_icr(oh);
  };

  // Build a valid continuation chunk image ("OCHK" + 4-byte checksum = 8 bytes).
  std::vector<uint8_t> chk_img = {'O', 'C', 'H', 'K'};
  AppendV2Checksum(&chk_img);

  H5O_chk_cache_ud_t chk_ud{};
  chk_ud.decoding = true;
  chk_ud.oh = oh;
  chk_ud.chunkno = 1;
  chk_ud.size = chk_img.size();
  chk_ud.common.f = f;
  chk_ud.common.file_intent = H5F_ACC_RDWR;
  chk_ud.common.cont_msg_info = &cont_info;
  chk_ud.common.addr = 0x5000;

  size_t chk_init_len = 0;
  ASSERT_GE(H5AC_OHDR_CHK->get_initial_load_size(&chk_ud, &chk_init_len), 0);
  EXPECT_EQ(chk_init_len, chk_img.size());
  EXPECT_GT(
      H5AC_OHDR_CHK->verify_chksum(chk_img.data(), chk_img.size(), &chk_ud),
      0);

  std::vector<uint8_t> bad_chk = chk_img;
  bad_chk.back() ^= 1u;
  EXPECT_EQ(
      H5AC_OHDR_CHK->verify_chksum(bad_chk.data(), bad_chk.size(), &chk_ud),
      0);

  auto* chk_proxy = static_cast<H5O_chunk_proxy_t*>(H5AC_OHDR_CHK->deserialize(
      chk_img.data(), chk_img.size(), &chk_ud, &dirty));
  ASSERT_NE(chk_proxy, nullptr);
  chk_proxy->cache_info.type = H5AC_OHDR_CHK;
  absl::Cleanup free_chk = [&] { H5AC_OHDR_CHK->free_icr(chk_proxy); };

  // Also cover H5O__cache_chk_deserialize with decoding=false (chunk proxy
  // reload when the object header remained cached).
  {
    H5O_chk_cache_ud_t chk_ud_reload = chk_ud;
    chk_ud_reload.decoding = false;
    chk_ud_reload.chunkno = 1;
    auto* chk_proxy_reload = static_cast<H5O_chunk_proxy_t*>(
        H5AC_OHDR_CHK->deserialize(chk_img.data(), chk_img.size(),
                                   &chk_ud_reload, &dirty));
    ASSERT_NE(chk_proxy_reload, nullptr);
    chk_proxy_reload->cache_info.type = H5AC_OHDR_CHK;
    EXPECT_GE(H5AC_OHDR_CHK->free_icr(chk_proxy_reload), 0);
  }

  size_t chk_len = 0;
  ASSERT_GE(H5AC_OHDR_CHK->image_len(chk_proxy, &chk_len), 0);
  EXPECT_EQ(chk_len, chk_img.size());

  std::vector<uint8_t> serialized_chk(chk_len, 0);
  ASSERT_GE(
      H5AC_OHDR_CHK->serialize(f, serialized_chk.data(), chk_len, chk_proxy),
      0);
  EXPECT_EQ(serialized_chk, chk_img);

  for (H5AC_notify_action_t act : {
           H5AC_NOTIFY_ACTION_AFTER_INSERT,
           H5AC_NOTIFY_ACTION_AFTER_LOAD,
           H5AC_NOTIFY_ACTION_AFTER_FLUSH,
           H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
           H5AC_NOTIFY_ACTION_ENTRY_CLEANED,
           H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
           H5AC_NOTIFY_ACTION_CHILD_CLEANED,
           H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
           H5AC_NOTIFY_ACTION_CHILD_SERIALIZED,
           H5AC_NOTIFY_ACTION_BEFORE_EVICT,
       }) {
    EXPECT_GE(H5AC_OHDR_CHK->notify(act, chk_proxy), 0);
  }

  ScopedErrorSilence silence;
  EXPECT_LT(
      H5AC_OHDR_CHK->notify(static_cast<H5AC_notify_action_t>(999), chk_proxy),
      0);

  // Wrong magic on chunkno > 0 triggers H5O__chunk_deserialize failure.
  std::vector<uint8_t> bad_magic = {'B', 'A', 'D', '!'};
  AppendV2Checksum(&bad_magic);
  EXPECT_EQ(H5AC_OHDR_CHK->deserialize(bad_magic.data(), bad_magic.size(),
                                       &chk_ud, &dirty),
            nullptr);

  // Input buffer shorter than chunk size triggers H5E_CANTCOPY check.
  EXPECT_EQ(H5AC_OHDR_CHK->deserialize(chk_img.data(), chk_img.size() - 2,
                                       &chk_ud, &dirty),
            nullptr);

  // Chunk size shorter than H5_SIZEOF_MAGIC triggers overflow check.
  H5O_chk_cache_ud_t chk_ud_tiny = chk_ud;
  chk_ud_tiny.size = 2;
  EXPECT_EQ(H5AC_OHDR_CHK->deserialize(chk_img.data(), chk_img.size(),
                                       &chk_ud_tiny, &dirty),
            nullptr);
}

}  // namespace
}  // namespace hdf5
