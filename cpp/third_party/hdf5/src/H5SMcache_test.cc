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
// Tests SOHM master table and list metadata cache callbacks (initial load size,
// checksum verification, serialization, deserialization, error paths, eviction
// and reload, and ICR cleanup) in third_party/hdf5/src/H5SMcache.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5SMcache.c

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t.
// Define NDEBUG around internal headers so C++ struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5SMCACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try h5b2_try_param
extern "C" {
#define H5SM_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5SMpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5SMCACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SMCACHE_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__SOHM_TAG) {
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

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5SM_master_table_t* AllocSyntheticTable(H5F_t* f, unsigned num_indexes,
                                         size_t list_max, size_t btree_min) {
  H5SM_master_table_t* table = H5FL_CALLOC(H5SM_master_table_t);
  if (table == nullptr) {
    return nullptr;
  }
  table->cache_info.type = H5AC_SOHM_TABLE;
  table->num_indexes = num_indexes;
  table->table_size = H5SM_TABLE_SIZE(f);
  table->indexes = static_cast<H5SM_index_header_t*>(
      H5FL_ARR_MALLOC(H5SM_index_header_t, num_indexes));
  if (table->indexes == nullptr) {
    H5FL_FREE(H5SM_master_table_t, table);
    return nullptr;
  }
  for (unsigned i = 0; i < num_indexes; ++i) {
    table->indexes[i] = {};
    table->indexes[i].mesg_types = H5O_SHMESG_SDSPACE_FLAG;
    table->indexes[i].min_mesg_size = 16;
    table->indexes[i].list_max = list_max;
    table->indexes[i].btree_min = btree_min;
    table->indexes[i].num_messages = 0;
    table->indexes[i].index_type = (i % 2 == 0) ? H5SM_LIST : H5SM_BTREE;
    table->indexes[i].index_addr = 0x2000U + i * 0x100U;
    table->indexes[i].heap_addr = 0x4000U + i * 0x100U;
    table->indexes[i].list_size = H5SM_LIST_SIZE(f, list_max);
  }
  return table;
}

H5SM_list_t* AllocSyntheticList(H5F_t* f, H5SM_index_header_t* header) {
  H5SM_list_t* list = H5FL_MALLOC(H5SM_list_t);
  if (list == nullptr) {
    return nullptr;
  }
  std::memset(&list->cache_info, 0, sizeof(H5AC_info_t));
  list->cache_info.type = H5AC_SOHM_LIST;
  list->header = header;
  list->messages =
      static_cast<H5SM_sohm_t*>(H5FL_ARR_MALLOC(H5SM_sohm_t, header->list_max));
  if (list->messages == nullptr) {
    H5FL_FREE(H5SM_list_t, list);
    return nullptr;
  }
  for (size_t i = 0; i < header->list_max; ++i) {
    list->messages[i] = {};
    list->messages[i].location = H5SM_NO_LOC;
  }
  return list;
}

TEST(H5SMcacheTest, EndToEndFlushEvictAndReloadInMemoryFile) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 2), 0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(
          fcpl.get(), 0, H5O_SHMESG_SDSPACE_FLAG | H5O_SHMESG_DTYPE_FLAG, 1),
      0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 1, H5O_SHMESG_ATTR_FLAG, 1),
            0);
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 5, 2), 0);

  ScopedFile file =
      CreateInMemoryFile("sohm_cache_e2e.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  // Share a dataspace twice (IN_HEAP, refcount=2), a second dataspace once
  // (IN_OH), and an attribute (IN_HEAP) so both lists are partially populated.
  const std::array<hsize_t, 3> dims_a = {3, 4, 5};
  const std::array<hsize_t, 2> dims_b = {6, 7};
  ScopedSpace sp_a(H5Screate_simple(3, dims_a.data(), nullptr));
  ScopedSpace sp_b(H5Screate_simple(2, dims_b.data(), nullptr));
  ASSERT_TRUE(sp_a.valid());
  ASSERT_TRUE(sp_b.valid());

  ScopedDataset ds1(H5Dcreate2(file.get(), "/ds1", H5T_NATIVE_INT, sp_a.get(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset ds2(H5Dcreate2(file.get(), "/ds2", H5T_NATIVE_INT, sp_a.get(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset ds3(H5Dcreate2(file.get(), "/ds3", H5T_NATIVE_INT, sp_b.get(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds1.valid());
  ASSERT_TRUE(ds2.valid());
  ASSERT_TRUE(ds3.valid());

  ScopedType attr_dt(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(attr_dt.valid());
  ASSERT_GE(H5Tset_size(attr_dt.get(), 32), 0);
  ScopedAttr attr1(H5Acreate2(ds1.get(), "tag", attr_dt.get(), sp_a.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr1.valid());
  std::array<char, 32 * 60> attr_buf{};
  attr_buf[0] = 'x';
  ASSERT_GE(H5Awrite(attr1.get(), attr_dt.get(), attr_buf.data()), 0);
  attr1.reset();
  ds1.reset();
  ds2.reset();
  ds3.reset();

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  // Evict cache and reload through H5AC_protect (SOHM table and lists).
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ASSERT_GE(H5AC_evict(f), 0);
    EXPECT_EQ(H5SM_type_shared(f, H5O_SDSPACE_ID), 1);
    EXPECT_EQ(H5SM_type_shared(f, H5O_ATTR_ID), 1);

    hsize_t hdr_size = 0;
    H5_ih_info_t ih_info{};
    ASSERT_GE(H5SM_ih_size(f, &hdr_size, &ih_info), 0);
    EXPECT_GT(hdr_size, 0u);
    EXPECT_GT(ih_info.index_size, 0u);
  }

  // Reopen the flushed image via H5Pset_file_image so deserialization + checksum
  // verification run from a clean file handle.
  const ssize_t img_sz = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(img_sz, 0);
  std::vector<uint8_t> img(static_cast<size_t>(img_sz));
  ASSERT_EQ(H5Fget_file_image(file.get(), img.data(), img.size()), img_sz);

  ScopedPlist reopen_fapl = MakeCoreFapl();
  ASSERT_TRUE(reopen_fapl.valid());
  ASSERT_GE(H5Pset_file_image(reopen_fapl.get(), img.data(), img.size()), 0);
  ScopedFile reopened(
      H5Fopen("sohm_reopen.h5", H5F_ACC_RDWR, reopen_fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedDataset r_ds1(H5Dopen2(reopened.get(), "/ds1", H5P_DEFAULT));
  ASSERT_TRUE(r_ds1.valid());
  ScopedSpace r_sp(H5Dget_space(r_ds1.get()));
  ASSERT_TRUE(r_sp.valid());
  EXPECT_EQ(H5Sget_simple_extent_ndims(r_sp.get()), 3);

  // Delete an object to reload and modify the cached SOHM list.
  r_sp.reset();
  r_ds1.reset();
  ASSERT_GE(H5Ldelete(reopened.get(), "/ds3", H5P_DEFAULT), 0);
  ASSERT_GE(H5Fflush(reopened.get(), H5F_SCOPE_GLOBAL), 0);
}

TEST(H5SMcacheTest, TableCacheCallbacksRoundTripAndErrorPaths) {
  EXPECT_EQ(H5AC_SOHM_TABLE[0].id, H5AC_SOHM_TABLE_ID);
  EXPECT_STREQ(H5AC_SOHM_TABLE[0].name, "shared message table");
  EXPECT_EQ(H5AC_SOHM_TABLE[0].mem_type, H5FD_MEM_SOHM_TABLE);

  for (const size_t addr_sz : {size_t{4}, size_t{8}}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), addr_sz, addr_sz), 0);
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 2), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_DTYPE_FLAG, 1),
              0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 1, H5O_SHMESG_ATTR_FLAG, 1),
              0);

    ScopedFile file =
        CreateInMemoryFile("sohm_table_cb.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    H5SM_table_cache_ud_t udata{};
    udata.f = f;

    size_t load_len = 0;
    ASSERT_GE(H5AC_SOHM_TABLE[0].get_initial_load_size(&udata, &load_len), 0);
    EXPECT_EQ(load_len, H5SM_TABLE_SIZE(f));

    H5SM_master_table_t* src_tbl = AllocSyntheticTable(f, 2, 10, 4);
    ASSERT_NE(src_tbl, nullptr);
    absl::Cleanup free_src = [&] {
      if (src_tbl != nullptr) {
        H5SM__table_free(src_tbl);
      }
    };

    size_t img_len = 0;
    ASSERT_GE(H5AC_SOHM_TABLE[0].image_len(src_tbl, &img_len), 0);
    EXPECT_EQ(img_len, src_tbl->table_size);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(H5AC_SOHM_TABLE[0].serialize(f, image.data(), img_len, src_tbl),
              0);

    EXPECT_EQ(H5AC_SOHM_TABLE[0].verify_chksum(image.data(), img_len, &udata),
              1);
    {
      std::vector<uint8_t> bad_chk = image;
      bad_chk[0] ^= 0x01;
      EXPECT_EQ(
          H5AC_SOHM_TABLE[0].verify_chksum(bad_chk.data(), img_len, &udata), 0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_SOHM_TABLE[0].verify_chksum(image.data(), 2, &udata), 0);
    }

    bool dirty = false;
    void* deser_raw =
        H5AC_SOHM_TABLE[0].deserialize(image.data(), img_len, &udata, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser = static_cast<H5SM_master_table_t*>(deser_raw);
    EXPECT_EQ(deser->num_indexes, 2u);
    EXPECT_EQ(deser->table_size, img_len);
    EXPECT_EQ(deser->indexes[0].index_type, H5SM_LIST);
    EXPECT_EQ(deser->indexes[1].index_type, H5SM_BTREE);
    EXPECT_EQ(deser->indexes[0].index_addr, 0x2000U);
    EXPECT_EQ(deser->indexes[1].heap_addr, 0x4100U);
    deser->cache_info.type = H5AC_SOHM_TABLE;
    ASSERT_GE(H5AC_SOHM_TABLE[0].free_icr(deser), 0);

    // Negative deserialization cases: corrupted magic and bad list version.
    {
      ScopedErrorSilence silence;

      std::vector<uint8_t> bad_magic = image;
      bad_magic[0] = 'X';
      EXPECT_EQ(H5AC_SOHM_TABLE[0].deserialize(bad_magic.data(), img_len,
                                               &udata, &dirty),
                nullptr);

      std::vector<uint8_t> bad_list_ver = image;
      bad_list_ver[H5_SIZEOF_MAGIC] = 99;
      EXPECT_EQ(H5AC_SOHM_TABLE[0].deserialize(bad_list_ver.data(), img_len,
                                               &udata, &dirty),
                nullptr);
    }
  }
}

TEST(H5SMcacheTest, ListCacheCallbacksSparseSerializationAndErrorPaths) {
  EXPECT_EQ(H5AC_SOHM_LIST[0].id, H5AC_SOHM_LIST_ID);
  EXPECT_STREQ(H5AC_SOHM_LIST[0].name, "shared message list");
  EXPECT_EQ(H5AC_SOHM_LIST[0].mem_type, H5FD_MEM_SOHM_TABLE);

  for (const size_t addr_sz : {size_t{4}, size_t{8}}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), addr_sz, addr_sz), 0);
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(
        H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_SDSPACE_FLAG, 1), 0);

    ScopedFile file =
        CreateInMemoryFile("sohm_list_cb.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    H5SM_index_header_t hdr{};
    hdr.mesg_types = H5O_SHMESG_SDSPACE_FLAG;
    hdr.min_mesg_size = 16;
    hdr.list_max = 5;
    hdr.btree_min = 2;
    hdr.num_messages = 2;
    hdr.index_type = H5SM_LIST;
    hdr.list_size = H5SM_LIST_SIZE(f, hdr.list_max);

    H5SM_list_cache_ud_t udata{};
    udata.f = f;
    udata.header = &hdr;

    size_t load_len = 0;
    ASSERT_GE(H5AC_SOHM_LIST[0].get_initial_load_size(&udata, &load_len), 0);
    EXPECT_EQ(load_len, hdr.list_size);

    H5SM_list_t* src_list = AllocSyntheticList(f, &hdr);
    ASSERT_NE(src_list, nullptr);
    absl::Cleanup free_src = [&] {
      if (src_list != nullptr) {
        H5SM__list_free(src_list);
      }
    };

    // Place valid entries at slots 1 and 3 with H5SM_NO_LOC gaps at 0, 2, 4 so
    // serialize exercises skipping empty slots and trailing zero-padding.
    src_list->messages[1].location = H5SM_IN_HEAP;
    src_list->messages[1].hash = 0x11223344U;
    src_list->messages[1].u.heap_loc.ref_count = 3;
    src_list->messages[1].u.heap_loc.fheap_id.val = 0x1020304050607080ULL;

    src_list->messages[3].location = H5SM_IN_OH;
    src_list->messages[3].hash = 0x55667788U;
    src_list->messages[3].msg_type_id = H5O_SDSPACE_ID;
    src_list->messages[3].u.mesg_loc.oh_addr = 0x9000U;
    src_list->messages[3].u.mesg_loc.index = 0U;

    size_t img_len = 0;
    ASSERT_GE(H5AC_SOHM_LIST[0].image_len(src_list, &img_len), 0);
    EXPECT_EQ(img_len, hdr.list_size);

    std::vector<uint8_t> image(img_len, 0xff);
    ASSERT_GE(H5AC_SOHM_LIST[0].serialize(f, image.data(), img_len, src_list),
              0);

    EXPECT_EQ(H5AC_SOHM_LIST[0].verify_chksum(image.data(), img_len, &udata),
              1);
    {
      std::vector<uint8_t> bad_chk = image;
      bad_chk[0] ^= 0x01;
      EXPECT_EQ(
          H5AC_SOHM_LIST[0].verify_chksum(bad_chk.data(), img_len, &udata), 0);
    }

    bool dirty = false;
    void* deser_raw =
        H5AC_SOHM_LIST[0].deserialize(image.data(), img_len, &udata, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser = static_cast<H5SM_list_t*>(deser_raw);
    EXPECT_EQ(deser->messages[0].location, H5SM_IN_HEAP);
    EXPECT_EQ(deser->messages[0].u.heap_loc.ref_count, 3u);
    EXPECT_EQ(deser->messages[1].location, H5SM_IN_OH);
    EXPECT_EQ(deser->messages[1].u.mesg_loc.oh_addr, 0x9000U);
    EXPECT_EQ(deser->messages[2].location, H5SM_NO_LOC);
    EXPECT_EQ(deser->messages[4].location, H5SM_NO_LOC);

    deser->cache_info.type = H5AC_SOHM_LIST;
    ASSERT_GE(H5AC_SOHM_LIST[0].free_icr(deser), 0);

    // Negative deserialization: corrupted magic cleans up list cleanly.
    {
      ScopedErrorSilence silence;
      std::vector<uint8_t> bad_magic = image;
      bad_magic[0] = 'Z';
      EXPECT_EQ(H5AC_SOHM_LIST[0].deserialize(bad_magic.data(), img_len, &udata,
                                              &dirty),
                nullptr);
    }
  }
}

}  // namespace
}  // namespace hdf5
