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
// Tests object header message management routines in
// third_party/hdf5/src/H5Omessage.c: creating, appending, updating, reading,
// resetting, freeing, copying, counting, existence checks, removal (sequence
// and callback), iteration (app and nested lib ops), sizing, SOHM sharing
// queries/updates, creation index queries, encode/decode, delete, flush, and
// flag queries.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Omessage.c

#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs pulled in via H5Opkg.h. Define NDEBUG around internal headers
// so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5OMESSAGE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5SMprivate.h"
#include "third_party/hdf5/src/H5Tprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5OMESSAGE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OMESSAGE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__SUPERBLOCK_TAG) {
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

// Pin/unprotect helper using an intentionally out-of-range object header addr.
H5O_loc_t MakeBadOloc(H5F_t* f) {
  H5O_loc_t bad{};
  bad.file = f;
  bad.addr = static_cast<haddr_t>(0x10000000ULL);
  return bad;
}

TEST(H5OmessageTest, CreateAppendWriteReadCountExistsAndGetFlags) {
  ScopedFile file = CreateInMemoryFile("msg_crud.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(oloc, nullptr);

  ScopedApiContext ctx(oloc->addr);

  EXPECT_EQ(H5O_msg_exists(oloc, H5O_NAME_ID), 0);
  EXPECT_EQ(H5O_msg_count(oloc, H5O_NAME_ID), 0);

  // Create a constant name message.
  H5O_name_t initial{};
  initial.s = const_cast<char*>("initial");
  ASSERT_GE(H5O_msg_create(oloc, H5O_NAME_ID, H5O_MSG_FLAG_CONSTANT,
                           H5O_UPDATE_TIME, &initial),
            0);
  EXPECT_GT(H5O_msg_exists(oloc, H5O_NAME_ID), 0);
  EXPECT_EQ(H5O_msg_count(oloc, H5O_NAME_ID), 1);

  uint8_t flags = 0;
  ASSERT_GE(H5O_msg_get_flags(oloc, H5O_NAME_ID, &flags), 0);
  EXPECT_EQ(flags, static_cast<uint8_t>(H5O_MSG_FLAG_CONSTANT));

  // Missing message type checks on get_flags / read / write / write_oh.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5O_msg_get_flags(oloc, H5O_EFL_ID, &flags), 0);
    H5O_efl_t dummy_efl{};
    EXPECT_EQ(H5O_msg_read(oloc, H5O_EFL_ID, &dummy_efl), nullptr);
    EXPECT_LT(H5O_msg_write(oloc, H5O_EFL_ID, 0, 0, &dummy_efl), 0);

    // Constant message cannot be overwritten without H5O_UPDATE_FORCE.
    H5O_name_t upd{};
    upd.s = const_cast<char*>("updated");
    EXPECT_LT(H5O_msg_write(oloc, H5O_NAME_ID, 0, 0, &upd), 0);
  }

  // Overwrite with H5O_UPDATE_FORCE succeeds.
  H5O_name_t forced{};
  forced.s = const_cast<char*>("forced_update");
  ASSERT_GE(H5O_msg_write(oloc, H5O_NAME_ID, 0,
                          H5O_UPDATE_FORCE | H5O_UPDATE_TIME, &forced),
            0);
  ASSERT_GE(H5O_msg_get_flags(oloc, H5O_NAME_ID, &flags), 0);
  EXPECT_EQ(flags, 0U);

  // Read into caller buffer and allocating read (mesg == nullptr).
  H5O_name_t read_buf{};
  ASSERT_EQ(H5O_msg_read(oloc, H5O_NAME_ID, &read_buf), &read_buf);
  EXPECT_STREQ(read_buf.s, "forced_update");
  EXPECT_GE(H5O_msg_reset(H5O_NAME_ID, &read_buf), 0);
  EXPECT_EQ(read_buf.s, nullptr);

  H5O_name_t* read_alloc =
      static_cast<H5O_name_t*>(H5O_msg_read(oloc, H5O_NAME_ID, nullptr));
  ASSERT_NE(read_alloc, nullptr);
  EXPECT_STREQ(read_alloc->s, "forced_update");
  EXPECT_EQ(H5O_msg_free(H5O_NAME_ID, read_alloc), nullptr);

  // Direct *_oh variants (append_oh, write_oh, read_oh, exists_oh, count_real).
  H5O_t* oh = H5O_pin(oloc);
  ASSERT_NE(oh, nullptr);
  {
    absl::Cleanup unpin_oh = [oh] { H5O_unpin(oh); };
    EXPECT_GT(H5O_msg_exists_oh(oh, H5O_NAME_ID), 0);
    EXPECT_EQ(H5O_msg_exists_oh(oh, H5O_EFL_ID), 0);
    EXPECT_EQ(H5O__msg_count_real(oh, H5O_MSG_NAME), 1U);

    H5O_name_t second{};
    second.s = const_cast<char*>("second");
    ASSERT_GE(H5O_msg_append_oh(f, oh, H5O_NAME_ID, 0, 0, &second), 0);
    EXPECT_EQ(H5O__msg_count_real(oh, H5O_MSG_NAME), 2U);

    H5O_name_t oh_upd{};
    oh_upd.s = const_cast<char*>("oh_updated");
    ASSERT_GE(H5O_msg_write_oh(f, oh, H5O_NAME_ID, 0, 0, &oh_upd), 0);

    H5O_name_t oh_read{};
    ASSERT_EQ(H5O_msg_read_oh(f, oh, H5O_NAME_ID, &oh_read), &oh_read);
    EXPECT_STREQ(oh_read.s, "oh_updated");
    H5O_msg_reset(H5O_NAME_ID, &oh_read);

    ScopedErrorSilence silence;
    EXPECT_LT(H5O_msg_write_oh(f, oh, H5O_EFL_ID, 0, 0, &oh_upd), 0);
    EXPECT_EQ(H5O_msg_read_oh(f, oh, H5O_EFL_ID, &oh_read), nullptr);
  }
}

TEST(H5OmessageTest, BadLocAndCopySizeFailureErrors) {
  ScopedFile file = CreateInMemoryFile("msg_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ScopedErrorSilence silence;

  const H5O_loc_t bad_loc = MakeBadOloc(f);
  H5O_name_t valid_name{};
  valid_name.s = const_cast<char*>("x");
  uint8_t flags = 0;

  EXPECT_LT(H5O_msg_create(&bad_loc, H5O_NAME_ID, 0, 0, &valid_name), 0);
  EXPECT_LT(H5O_msg_write(&bad_loc, H5O_NAME_ID, 0, 0, &valid_name), 0);
  EXPECT_EQ(H5O_msg_read(&bad_loc, H5O_NAME_ID, nullptr), nullptr);
  EXPECT_LT(H5O_msg_count(&bad_loc, H5O_NAME_ID), 0);
  EXPECT_LT(H5O_msg_exists(&bad_loc, H5O_NAME_ID), 0);
  EXPECT_LT(H5O_msg_remove(&bad_loc, H5O_NAME_ID, 0, false), 0);
  EXPECT_LT(
      H5O_msg_remove_op(&bad_loc, H5O_NAME_ID, 0, nullptr, nullptr, false), 0);
  H5O_mesg_operator_t noop_op{};
  noop_op.op_type = H5O_MESG_OP_APP;
  noop_op.u.app_op = [](const void*, unsigned, void*) -> herr_t { return 0; };
  EXPECT_LT(H5O_msg_iterate(&bad_loc, H5O_NAME_ID, &noop_op, nullptr), 0);
  EXPECT_LT(H5O_msg_get_flags(&bad_loc, H5O_NAME_ID, &flags), 0);

  // Copy and raw_size/size_f/size_oh failures using H5O_name_t with s == NULL.
  H5O_name_t null_name{};
  EXPECT_EQ(H5O_msg_copy(H5O_NAME_ID, &null_name, nullptr), nullptr);
  EXPECT_EQ(H5O_msg_raw_size(f, H5O_NAME_ID, false, &null_name), 0U);
  EXPECT_EQ(H5O_msg_size_f(f, H5I_INVALID_HID, H5O_NAME_ID, &valid_name, 0),
            0U);

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  EXPECT_EQ(H5O_msg_size_f(f, gcpl.get(), H5O_NAME_ID, &null_name, 0), 0U);
  ScopedPlist fapl(MakeCoreFapl());
  ASSERT_TRUE(fapl.valid());
  EXPECT_EQ(H5O_msg_size_f(f, fapl.get(), H5O_NAME_ID, &valid_name, 0), 0U);

  // Trigger H5O__copy_mesg failure in create and write, then clean up cleanly.
  ScopedGroup grp(H5Gcreate2(file.get(), "/err_grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(oloc, nullptr);

  H5O_t* oh = H5O_pin(oloc);
  ASSERT_NE(oh, nullptr);
  EXPECT_EQ(H5O_msg_size_oh(f, oh, H5O_NAME_ID, &null_name, 0), 0U);
  H5O_unpin(oh);

  // Create a valid name message first so H5O_msg_write_oh reaches
  // H5O__copy_mesg and hits type->copy returning NULL; capture and free the
  // pre-existing native H5O_name_t pointer because HDF5 overwrites
  // idx_msg->native with NULL on error.
  EXPECT_LT(H5O_msg_create(oloc, H5O_NAME_ID, 0, 0, &null_name), 0);
  ASSERT_GE(H5O_msg_create(oloc, H5O_NAME_ID, 0, 0, &valid_name), 0);
  oh = H5O_pin(oloc);
  ASSERT_NE(oh, nullptr);
  void* prev_native = nullptr;
  for (unsigned u = 0; u < oh->nmesgs; ++u) {
    if (oh->mesg[u].type == H5O_MSG_NAME) {
      prev_native = oh->mesg[u].native;
      break;
    }
  }
  EXPECT_LT(H5O_msg_write_oh(f, oh, H5O_NAME_ID, 0, 0, &null_name), 0);
  if (prev_native != nullptr) {
    H5O_msg_free(H5O_NAME_ID, prev_native);
  }
  ASSERT_GE(H5O_unpin(oh), 0);
  EXPECT_GE(H5O_msg_remove(oloc, H5O_NAME_ID, H5O_ALL, false), 0);
}

TEST(H5OmessageTest, ResetFreeEncodeDecodeAndSizing) {
  ScopedFile file = CreateInMemoryFile("msg_codec_size.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  // Reset null pointer is a safe no-op; reset with type->reset == NULL memsets.
  EXPECT_GE(H5O_msg_reset(H5O_NAME_ID, nullptr), 0);
  EXPECT_EQ(H5O_msg_free(H5O_NAME_ID, nullptr), nullptr);

  time_t mtime_val = 123456;
  EXPECT_GE(H5O_msg_reset(H5O_MTIME_NEW_ID, &mtime_val), 0);
  EXPECT_EQ(mtime_val, 0);

  // Free using H5O__msg_free_mesg.
  time_t sample_time = 999999;
  H5O_mesg_t mesg_wrapper{};
  mesg_wrapper.type = H5O_MSG_MTIME_NEW;
  mesg_wrapper.native = H5O_msg_copy(H5O_MTIME_NEW_ID, &sample_time, nullptr);
  ASSERT_NE(mesg_wrapper.native, nullptr);
  EXPECT_GE(H5O__msg_free_mesg(&mesg_wrapper), 0);
  EXPECT_EQ(mesg_wrapper.native, nullptr);

  // Size calculations with default OCPL vs creation-order-tracked OCPL.
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  H5O_name_t nm{};
  nm.s = const_cast<char*>("abcdef");
  const size_t sz_default = H5O_msg_size_f(f, gcpl.get(), H5O_NAME_ID, &nm, 5);
  EXPECT_GE(sz_default, 7U + 5U);

  ASSERT_GE(H5Pset_attr_creation_order(gcpl.get(), H5P_CRT_ORDER_TRACKED), 0);
  const size_t sz_tracked = H5O_msg_size_f(f, gcpl.get(), H5O_NAME_ID, &nm, 5);
  EXPECT_GT(sz_tracked, sz_default);

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/sz", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ScopedApiContext ctx(oloc->addr);
  H5O_t* oh = H5O_pin(oloc);
  ASSERT_NE(oh, nullptr);
  EXPECT_EQ(H5O_msg_size_oh(f, oh, H5O_NAME_ID, &nm, 5), sz_tracked);
  H5O_unpin(oh);

  // Encode / decode round-trip & decode failure.
  std::array<uint8_t, 8> buf{};
  const time_t t_in = 1700000000;
  ASSERT_GE(H5O_msg_encode(f, H5O_MTIME_NEW_ID, false, buf.data(), &t_in), 0);
  time_t* t_out = static_cast<time_t*>(
      H5O_msg_decode(f, nullptr, H5O_MTIME_NEW_ID, buf.size(), buf.data()));
  ASSERT_NE(t_out, nullptr);
  EXPECT_EQ(*t_out, t_in);
  H5O_msg_free(H5O_MTIME_NEW_ID, t_out);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_NEW_ID, 2, buf.data()),
            nullptr);
}

TEST(H5OmessageTest, IterateRemoveOpReadOnlyAndNestedRecursion) {
  TempFile tmp("h5omessage_iter_rm.h5");
  {
    ScopedFile file(
        H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gcreate2(file.get(), "/multi", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    H5O_loc_t* oloc = H5O_get_loc(grp.get());
    ScopedApiContext ctx(oloc->addr);
    for (const char* s : {"m0", "m1", "m2", "m3"}) {
      H5O_name_t nm{};
      nm.s = const_cast<char*>(s);
      ASSERT_GE(H5O_msg_create(oloc, H5O_NAME_ID, 0, 0, &nm), 0);
    }
    EXPECT_EQ(H5O_msg_count(oloc, H5O_NAME_ID), 4);
  }

  // Reopen RDONLY first: verifies H5O_LOAD_NATIVE during iterate/read AND
  // read-only removal rejection.
  {
    ScopedFile file(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gopen2(file.get(), "/multi", H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    H5O_loc_t* oloc = H5O_get_loc(grp.get());
    ScopedApiContext ctx(oloc->addr);

    std::vector<std::string> seen;
    H5O_mesg_operator_t op{};
    op.op_type = H5O_MESG_OP_APP;
    op.u.app_op = [](const void* mesg, unsigned /*idx*/,
                     void* op_data) -> herr_t {
      auto* vec = static_cast<std::vector<std::string>*>(op_data);
      vec->emplace_back(static_cast<const H5O_name_t*>(mesg)->s);
      return 0;
    };
    EXPECT_EQ(H5O_msg_iterate(oloc, H5O_NAME_ID, &op, &seen), 0);
    EXPECT_THAT(seen, ElementsAre("m0", "m1", "m2", "m3"));

    // Positive short-circuit return from app callback.
    H5O_mesg_operator_t stop_op{};
    stop_op.op_type = H5O_MESG_OP_APP;
    stop_op.u.app_op = [](const void*, unsigned idx, void*) -> herr_t {
      return idx == 1 ? 42 : 0;
    };
    EXPECT_EQ(H5O_msg_iterate(oloc, H5O_NAME_ID, &stop_op, nullptr), 42);

    // Negative error return from app callback.
    ScopedErrorSilence silence;
    H5O_mesg_operator_t err_op{};
    err_op.op_type = H5O_MESG_OP_APP;
    err_op.u.app_op = [](const void*, unsigned, void*) -> herr_t { return -1; };
    EXPECT_LT(H5O_msg_iterate(oloc, H5O_NAME_ID, &err_op, nullptr), 0);

    // Removal on read-only file fails (both via H5O_pin and directly in
    // H5O__msg_remove_real).
    EXPECT_LT(H5O_msg_remove(oloc, H5O_NAME_ID, 0, false), 0);
    H5O_t* oh_ro = H5O_protect(oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh_ro, nullptr);
    EXPECT_LT(H5O__msg_remove_real(oloc->file, oh_ro, H5O_MSG_NAME, 0, nullptr,
                                   nullptr, false),
              0);
    EXPECT_GE(H5O_unprotect(oloc, oh_ro, H5AC__NO_FLAGS_SET), 0);
  }

  // Reopen RDWR and exercise remove_op, remove(sequence), nested lib iterate,
  // and H5O_ALL removal.
  {
    ScopedFile file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gopen2(file.get(), "/multi", H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    H5O_loc_t* oloc = H5O_get_loc(grp.get());
    ScopedApiContext ctx(oloc->addr);

    // Negative callback in H5O_msg_remove_op.
    {
      ScopedErrorSilence silence;
      auto fail_rm = [](const void*, unsigned, void*) -> herr_t { return -1; };
      EXPECT_LT(H5O_msg_remove_op(oloc, H5O_NAME_ID, H5O_ALL, fail_rm, nullptr,
                                  false),
                0);
    }

    // Callback selecting only "m1" with sequence = H5O_FIRST.
    auto rm_m1 = [](const void* mesg, unsigned, void*) -> herr_t {
      return std::strcmp(static_cast<const H5O_name_t*>(mesg)->s, "m1") == 0
                 ? 1
                 : 0;
    };
    ASSERT_GE(
        H5O_msg_remove_op(oloc, H5O_NAME_ID, H5O_FIRST, rm_m1, nullptr, false),
        0);
    EXPECT_EQ(H5O_msg_count(oloc, H5O_NAME_ID), 3);

    // Remove by explicit sequence index 1 ("m2").
    ASSERT_GE(H5O_msg_remove(oloc, H5O_NAME_ID, 1, false), 0);
    EXPECT_EQ(H5O_msg_count(oloc, H5O_NAME_ID), 2);

    // Nested H5O_MESG_OP_LIB iteration: outer iterates H5O_MSG_GINFO while
    // inner deletes H5O_MSG_NAME with oh->recursion_level > 0, deferring
    // condense to outer finish.
    H5O_t* oh = H5O_pin(oloc);
    ASSERT_NE(oh, nullptr);
    H5O_mesg_operator_t outer_op{};
    outer_op.op_type = H5O_MESG_OP_LIB;
    outer_op.u.lib_op = [](H5O_t* inner_oh, H5O_mesg_t*, unsigned,
                           void* file_ptr) -> herr_t {
      H5F_t* inner_f = static_cast<H5F_t*>(file_ptr);
      H5O_mesg_operator_t inner_op{};
      inner_op.op_type = H5O_MESG_OP_LIB;
      inner_op.u.lib_op = [](H5O_t* target_oh, H5O_mesg_t* msg, unsigned,
                             void* target_f) -> herr_t {
        if (H5O__release_mesg(static_cast<H5F_t*>(target_f), target_oh, msg,
                              false) < 0) {
          return -1;
        }
        target_oh->mesgs_modified = H5O_MODIFY_CONDENSE;
        return 1;
      };
      return H5O__msg_iterate_real(inner_f, inner_oh, H5O_MSG_NAME, &inner_op,
                                   file_ptr) < 0
                 ? -1
                 : 1;
    };
    ASSERT_GE(H5O__msg_iterate_real(oloc->file, oh, H5O_MSG_GINFO, &outer_op,
                                    oloc->file),
              0);
    H5O_unpin(oh);
    EXPECT_EQ(H5O_msg_count(oloc, H5O_NAME_ID), 1);

    // Remove remaining messages with H5O_ALL.
    ASSERT_GE(H5O_msg_remove(oloc, H5O_NAME_ID, H5O_ALL, false), 0);
    EXPECT_EQ(H5O_msg_count(oloc, H5O_NAME_ID), 0);
  }
}

TEST(H5OmessageTest, SharedMessagesAllocWriteDeleteAndSharingQueries) {
  // Enable SOHM index for filter pipelines with phase change cutoff 1 so
  // repeated pipelines quickly move into the SOHM heap.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_PLINE_FLAG, 40),
            0);
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 1, 1), 0);

  ScopedFile file =
      CreateInMemoryFile("msg_sohm.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  // Query sharing metadata across message types.
  H5O_pline_t pl{};
  pl.version = H5O_PLINE_VERSION_2;
  H5O_name_t nm{};
  nm.s = const_cast<char*>("not_sharable");
  EXPECT_GT(H5O_msg_can_share(H5O_PLINE_ID, &pl), 0);
  EXPECT_EQ(H5O_msg_can_share(H5O_NAME_ID, &nm), 0);
  EXPECT_GT(H5O_msg_can_share_in_ohdr(H5O_PLINE_ID), 0);
  EXPECT_EQ(H5O_msg_can_share_in_ohdr(H5O_ATTR_ID), 0);
  EXPECT_EQ(H5O_msg_is_shared(H5O_NAME_ID, &nm), 0);
  EXPECT_EQ(H5O_msg_is_shared(H5O_PLINE_ID, &pl), 0);

  // H5O_msg_can_share on H5O_DTYPE_ID (type->can_share != NULL).
  H5T_t* imm_dt =
      static_cast<H5T_t*>(H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
  ASSERT_NE(imm_dt, nullptr);
  EXPECT_EQ(H5O_msg_can_share(H5O_DTYPE_ID, imm_dt), 0);
  ScopedType cmp_tid(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(cmp_tid.valid());
  ASSERT_GE(H5Tinsert(cmp_tid.get(), "x", 0, H5T_NATIVE_INT32), 0);
  H5T_t* cmp_dt =
      static_cast<H5T_t*>(H5I_object_verify(cmp_tid.get(), H5I_DATATYPE));
  ASSERT_NE(cmp_dt, nullptr);
  EXPECT_GT(H5O_msg_can_share(H5O_DTYPE_ID, cmp_dt), 0);

  // set_share and reset_share for both type->set_share == NULL (PLINE) and
  // != NULL (DTYPE).
  H5O_shared_t fake_sh{};
  fake_sh.type = H5O_SHARE_TYPE_COMMITTED;
  fake_sh.file = f;
  fake_sh.msg_type_id = H5O_PLINE_ID;
  fake_sh.u.loc.oh_addr = 128;
  fake_sh.u.loc.index = 0;
  ASSERT_GE(H5O_msg_set_share(H5O_PLINE_ID, &fake_sh, &pl), 0);
  EXPECT_GT(H5O_msg_is_shared(H5O_PLINE_ID, &pl), 0);
  ASSERT_GE(H5O_msg_reset_share(H5O_PLINE_ID, &pl), 0);
  EXPECT_EQ(H5O_msg_is_shared(H5O_PLINE_ID, &pl), 0);

  fake_sh.msg_type_id = H5O_DTYPE_ID;
  ASSERT_GE(H5O_msg_set_share(H5O_DTYPE_ID, &fake_sh, cmp_dt), 0);
  EXPECT_GT(H5O_msg_is_shared(H5O_DTYPE_ID, cmp_dt), 0);
  ASSERT_GE(H5O_msg_reset_share(H5O_DTYPE_ID, cmp_dt), 0);
  EXPECT_EQ(H5O_msg_is_shared(H5O_DTYPE_ID, cmp_dt), 0);

  // Create two chunked deflate datasets so their H5O_PLINE_ID messages enter
  // SOHM, then exercise H5O_msg_write on shared/shareable messages and
  // H5O__msg_alloc linking an already-shared message.
  const hsize_t dims[1] = {16};
  const hsize_t cdims[1] = {8};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(space.valid());
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, cdims), 0);
  ASSERT_GE(H5Pset_deflate(dcpl.get(), 5), 0);
  const unsigned cd8[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5Pset_filter(dcpl.get(), 312, H5Z_FLAG_OPTIONAL, 8, cd8), 0);

  ScopedDataset d1(H5Dcreate2(file.get(), "/d1", H5T_NATIVE_INT32, space.get(),
                              H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ScopedDataset d2(H5Dcreate2(file.get(), "/d2", H5T_NATIVE_INT32, space.get(),
                              H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(d1.valid());
  ASSERT_TRUE(d2.valid());

  H5O_loc_t* d2_loc = H5O_get_loc(d2.get());
  ASSERT_NE(d2_loc, nullptr);
  ScopedApiContext ctx(d2_loc->addr);

  H5O_pline_t* shared_pl =
      static_cast<H5O_pline_t*>(H5O_msg_read(d2_loc, H5O_PLINE_ID, nullptr));
  ASSERT_NE(shared_pl, nullptr);
  absl::Cleanup free_shared_pl = [shared_pl] {
    H5O_msg_free(H5O_PLINE_ID, shared_pl);
  };
  EXPECT_GT(H5O_msg_is_shared(H5O_PLINE_ID, shared_pl), 0);

  // Append already-shared pipeline onto group headers (hits H5O__msg_alloc
  // shared_mesg > 0 -> type->link branch) and exercise write/remove paths.
  ScopedGroup target_grp(H5Gcreate2(file.get(), "/shared_holder", H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(target_grp.valid());
  H5O_loc_t* tgt_loc = H5O_get_loc(target_grp.get());
  ASSERT_GE(H5O_msg_create(tgt_loc, H5O_PLINE_ID, 0, 0, shared_pl), 0);
  // Overwrite shared slot with an equally shared message, then remove cleanly.
  ASSERT_GE(H5O_msg_write(tgt_loc, H5O_PLINE_ID, 0, 0, shared_pl), 0);
  EXPECT_GE(H5O_msg_remove(tgt_loc, H5O_PLINE_ID, H5O_ALL, false), 0);

  // Build an unshared small pipeline (1 filter < 40-byte SOHM minsize) for
  // negative write tests.
  H5O_pline_t unique_pl{};
  unique_pl.version = H5O_PLINE_VERSION_2;
  unique_pl.nused = 1;
  unique_pl.nalloc = 1;
  unique_pl.filter =
      static_cast<H5Z_filter_info_t*>(H5MM_calloc(sizeof(H5Z_filter_info_t)));
  ASSERT_NE(unique_pl.filter, nullptr);
  absl::Cleanup reset_unique_pl = [&unique_pl] {
    H5O_msg_reset(H5O_PLINE_ID, &unique_pl);
  };
  unique_pl.filter[0].id = H5Z_FILTER_DEFLATE;
  unique_pl.filter[0].cd_nelmts = 1;
  unique_pl.filter[0].cd_values = unique_pl.filter[0]._cd_values;
  unique_pl.filter[0].cd_values[0] = 9;

  // Trying to modify dataset's constant PLINE message without FORCE fails.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5O_msg_write(d2_loc, H5O_PLINE_ID, 0, 0, &unique_pl), 0);
  }

  // Test "message changed sharing status" error in H5O__msg_write_real: force
  // H5O_MSG_FLAG_SHARED on a smaller pipeline below the 40-byte SOHM minsize.
  ScopedGroup bad_share_grp(H5Gcreate2(file.get(), "/shared_bad", H5P_DEFAULT,
                                       H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(bad_share_grp.valid());
  H5O_loc_t* bad_loc = H5O_get_loc(bad_share_grp.get());
  ASSERT_GE(H5O_msg_create(bad_loc, H5O_PLINE_ID, 0, 0, shared_pl), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5O_msg_write(bad_loc, H5O_PLINE_ID, H5O_MSG_FLAG_SHARED, 0,
                            &unique_pl),
              0);
  }

  // H5O_msg_delete / H5O__delete_mesg with both del == NULL and del != NULL.
  EXPECT_GE(H5O_msg_delete(f, nullptr, H5O_NAME_ID, &nm), 0);
  H5O_pline_t unshared_del{};
  EXPECT_GE(H5O_msg_delete(f, nullptr, H5O_PLINE_ID, &unshared_del), 0);
  {
    ScopedErrorSilence silence;
    H5O_pline_t bad_shared{};
    bad_shared.sh_loc.type = H5O_SHARE_TYPE_SOHM;
    bad_shared.sh_loc.file = f;
    bad_shared.sh_loc.msg_type_id = H5O_PLINE_ID;
    bad_shared.sh_loc.u.heap_id.val = 0xdeadbeefcafebabeULL;
    EXPECT_LT(H5O_msg_delete(f, nullptr, H5O_PLINE_ID, &bad_shared), 0);
  }
}

TEST(H5OmessageTest, CrtIndexAttributeAllocDeletedFlushAndUnknownMsg) {
  // Attribute creation order tracking + H5O_msg_get_crt_index + H5O__msg_alloc
  // crt_idx + v1 and v2 H5O_msg_flush + H5O_MSG_UNKNOWN + num_deleted_mesgs.
  ScopedFile file_v1 = [&] {
    ScopedPlist fapl = MakeCoreFapl();
    EXPECT_TRUE(fapl.valid());
    EXPECT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    return ScopedFile(
        H5Fcreate("msg_v1.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  }();
  ASSERT_TRUE(file_v1.valid());
  ScopedGroup g_v1(
      H5Gcreate2(file_v1.get(), "/gv1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_v1.valid());
  ASSERT_GE(H5Oset_comment(g_v1.get(), "v1 comment"), 0);
  H5O_loc_t* v1_loc = H5O_get_loc(g_v1.get());
  ASSERT_NE(v1_loc, nullptr);
  {
    ScopedApiContext ctx(v1_loc->addr);
    H5O_t* oh_v1 = H5O_pin(v1_loc);
    ASSERT_NE(oh_v1, nullptr);
    ASSERT_GE(H5O__flush_msgs(v1_loc->file, oh_v1), 0);
    H5O_unpin(oh_v1);
  }

  // V2 file with tracked attribute creation order.
  ScopedFile file_v2 = CreateInMemoryFile("msg_v2.h5");
  ASSERT_TRUE(file_v2.valid());
  H5F_t* f2 = UnwrapFile(file_v2.get());
  ASSERT_NE(f2, nullptr);

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_attr_creation_order(gcpl.get(), H5P_CRT_ORDER_TRACKED), 0);
  ScopedGroup g_v2(
      H5Gcreate2(file_v2.get(), "/gv2", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(g_v2.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  for (const char* aname : {"a0", "a1"}) {
    ScopedAttr a(H5Acreate2(g_v2.get(), aname, H5T_NATIVE_INT32, scalar.get(),
                            H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a.valid());
    int32_t v = 7;
    ASSERT_GE(H5Awrite(a.get(), H5T_NATIVE_INT32, &v), 0);
  }

  H5O_loc_t* v2_loc = H5O_get_loc(g_v2.get());
  ASSERT_NE(v2_loc, nullptr);
  ScopedApiContext ctx(v2_loc->addr);

  // Read attribute message natively to verify H5O_msg_get_crt_index and
  // H5O__msg_alloc with get_crt_index != NULL.
  void* attr_native = H5O_msg_read(v2_loc, H5O_ATTR_ID, nullptr);
  ASSERT_NE(attr_native, nullptr);
  absl::Cleanup free_attr = [attr_native] {
    H5O_msg_free(H5O_ATTR_ID, attr_native);
  };
  H5O_msg_crt_idx_t crt_idx = 999;
  ASSERT_GE(H5O_msg_get_crt_index(H5O_ATTR_ID, attr_native, &crt_idx), 0);
  EXPECT_EQ(crt_idx, 0U);

  H5O_name_t dummy_nm{};
  H5O_msg_crt_idx_t no_crt = 123;
  ASSERT_GE(H5O_msg_get_crt_index(H5O_NAME_ID, &dummy_nm, &no_crt), 0);
  EXPECT_EQ(no_crt, 0U);

  // Add enough comments with deletions to force chunk growth and
  // H5O_MSG_DELETED coalescing in H5O__msg_alloc, then flush.
  for (int i = 0; i < 12; ++i) {
    std::string txt = "msg_payload_" + std::to_string(i) + "_padding_text";
    H5O_name_t nm{};
    nm.s = txt.data();
    ASSERT_GE(H5O_msg_create(v2_loc, H5O_NAME_ID, 0, 0, &nm), 0);
    if (i % 2 == 1) {
      ASSERT_GE(H5O_msg_remove(v2_loc, H5O_NAME_ID, 0, false), 0);
    }
  }

  H5O_t* oh_v2 = H5O_pin(v2_loc);
  ASSERT_NE(oh_v2, nullptr);
  {
    absl::Cleanup unpin_v2 = [oh_v2] { H5O_unpin(oh_v2); };
    ASSERT_GE(H5O__flush_msgs(f2, oh_v2), 0);

    // Direct test of H5O__delete_mesg with both del == NULL and del != NULL.
    H5O_mesg_t del_null_mesg{};
    del_null_mesg.type = H5O_MSG_NAME;
    EXPECT_GE(H5O__delete_mesg(f2, oh_v2, &del_null_mesg), 0);

    H5O_pline_t tmp_pl{};
    H5O_mesg_t del_pline_mesg{};
    del_pline_mesg.type = H5O_MSG_PLINE;
    del_pline_mesg.native = &tmp_pl;
    EXPECT_GE(H5O__delete_mesg(f2, oh_v2, &del_pline_mesg), 0);

    ScopedErrorSilence silence;
    H5O_pline_t bad_shared_pl{};
    bad_shared_pl.sh_loc.type = H5O_SHARE_TYPE_SOHM;
    bad_shared_pl.sh_loc.file = f2;
    bad_shared_pl.sh_loc.msg_type_id = H5O_PLINE_ID;
    bad_shared_pl.sh_loc.u.heap_id.val = 0xdeadbeefcafebabeULL;
    H5O_mesg_t del_bad_mesg{};
    del_bad_mesg.type = H5O_MSG_PLINE;
    del_bad_mesg.native = &bad_shared_pl;
    EXPECT_LT(H5O__delete_mesg(f2, oh_v2, &del_bad_mesg), 0);
  }

  // Exercise H5O_DELETED_ID coalescing in H5O__msg_alloc (num_deleted_mesgs > 0
  // when a moved message absorbs an adjacent NULL message) and nested
  // recursion_level > 0 allocation inside H5O__msg_iterate_real.
  ScopedGroup g_frag(H5Gcreate2(file_v2.get(), "/frag", H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_frag.valid());
  H5O_loc_t* frag_loc = H5O_get_loc(g_frag.get());
  ASSERT_NE(frag_loc, nullptr);
  {
    ScopedApiContext frag_ctx(frag_loc->addr);
    for (const char* s : {"k0_hdr", "move_me_payload_0123456789abcdef",
                          "del_after_move_0123456789", "tail_keep_msg_1234"}) {
      H5O_name_t nm{};
      nm.s = const_cast<char*>(s);
      ASSERT_GE(H5O_msg_create(frag_loc, H5O_NAME_ID, 0, 0, &nm), 0);
    }
    ASSERT_GE(H5O_msg_remove(frag_loc, H5O_NAME_ID, 2, false), 0);
    std::string huge_comment(512, 'z');
    H5O_name_t huge_nm{};
    huge_nm.s = huge_comment.data();
    ASSERT_GE(H5O_msg_create(frag_loc, H5O_NAME_ID, 0, 0, &huge_nm), 0);

    H5O_t* oh_frag = H5O_pin(frag_loc);
    ASSERT_NE(oh_frag, nullptr);
    absl::Cleanup unpin_frag = [oh_frag] { H5O_unpin(oh_frag); };
    struct AppendCtx {
      H5F_t* f;
      H5O_t* oh;
    } append_ctx{f2, oh_frag};
    H5O_mesg_operator_t append_op{};
    append_op.op_type = H5O_MESG_OP_APP;
    append_op.u.app_op = [](const void*, unsigned, void* op_data) -> herr_t {
      auto* ctx_ptr = static_cast<AppendCtx*>(op_data);
      H5O_name_t extra{};
      extra.s = const_cast<char*>("nested_append");
      if (H5O_msg_append_oh(ctx_ptr->f, ctx_ptr->oh, H5O_NAME_ID, 0, 0,
                            &extra) < 0) {
        return -1;
      }
      return 1;  // Stop immediately before oh->mesg reallocation could matter.
    };
    EXPECT_EQ(H5O__msg_iterate_real(f2, oh_frag, H5O_MSG_NAME, &append_op,
                                    &append_ctx),
              1);
  }

  // Exercise H5O__msg_copy_file via H5Ocopy of a filtered chunked dataset.
  const hsize_t dims[1] = {16};
  const hsize_t cdims[1] = {8};
  ScopedSpace ds_space(H5Screate_simple(1, dims, nullptr));
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(ds_space.valid());
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, cdims), 0);
  ASSERT_GE(H5Pset_deflate(dcpl.get(), 4), 0);
  ScopedDataset dset(H5Dcreate2(file_v2.get(), "/ds_copy_src", H5T_NATIVE_INT32,
                                ds_space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedFile file_dst = CreateInMemoryFile("msg_copy_dst.h5");
  ASSERT_TRUE(file_dst.valid());
  EXPECT_GE(H5Ocopy(file_v2.get(), "/ds_copy_src", file_dst.get(),
                    "/ds_copy_dst", H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Exercise H5O_msg_flush on a synthetic in-memory H5O_t with H5O_MSG_UNKNOWN
  // (covers unknown message ID retrieval & skipping encode).
  std::vector<uint8_t> chunk_img(64, 0);
  H5O_chunk_t synth_chunk{};
  synth_chunk.image = chunk_img.data();
  synth_chunk.size = chunk_img.size();
  H5O_t synth_oh{};
  synth_oh.version = H5O_VERSION_2;
  synth_oh.flags = H5O_HDR_ATTR_CRT_ORDER_TRACKED;
  synth_oh.nchunks = 1;
  synth_oh.chunk = &synth_chunk;

  H5O_unknown_t unk_id = 0x7f;
  const size_t hdr_sz = H5O_SIZEOF_MSGHDR_OH(&synth_oh);
  H5O_mesg_t unk_mesg{};
  unk_mesg.type = H5O_MSG_UNKNOWN;
  unk_mesg.flags = H5O_MSG_FLAG_MARK_IF_UNKNOWN;
  unk_mesg.crt_idx = 3;
  unk_mesg.chunkno = 0;
  unk_mesg.native = &unk_id;
  unk_mesg.raw = chunk_img.data() + hdr_sz;
  unk_mesg.raw_size = 8;
  unk_mesg.dirty = true;
  EXPECT_GE(H5O_msg_flush(f2, &synth_oh, &unk_mesg), 0);
  EXPECT_FALSE(unk_mesg.dirty);
  EXPECT_EQ(chunk_img[0], 0x7fU);
}

}  // namespace
}  // namespace hdf5
