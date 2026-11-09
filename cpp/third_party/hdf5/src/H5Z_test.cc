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
// Tests filter registration, unregistration, pipeline management, prelude
// callbacks, and pipeline execution (third_party/hdf5/src/H5Z.c).
// Note: google3 disables dynamic filter plugins (H5PL_load returns nullptr)
// and builds without H5_HAVE_PARALLEL or H5Z_DEBUG.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Z.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG; match its layout across internal headers.
#ifndef NDEBUG
#define H5Z_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5Z_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Sprivate.h"
#include "third_party/hdf5/src/H5Zpkg.h"
}  // extern "C"
#ifdef H5Z_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5Z_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Global scratch observed by direct/prelude filter callbacks.
struct PreludeProbe {
  int can_apply_calls = 0;
  int set_local_calls = 0;
  hid_t last_can_apply_dcpl = 0;
  hid_t last_can_apply_type = 0;
  hid_t last_can_apply_space = 0;
  hid_t last_set_local_dcpl = 0;
  hid_t last_set_local_type = 0;
  hid_t last_set_local_space = 0;
  unsigned last_filter_flags = 0;
};

PreludeProbe g_probe{};

extern "C" size_t XorFilterFunc(unsigned flags, size_t cd_nelmts,
                                const unsigned cd_values[], size_t nbytes,
                                size_t* buf_size, void** buf) {
  g_probe.last_filter_flags = flags;
  if (cd_nelmts > 0 && cd_values[0] == 999U) {
    return 0;  // Requested failure.
  }
  const uint8_t key = cd_nelmts > 0 ? static_cast<uint8_t>(cd_values[0]) : 0x5a;
  auto* bytes = static_cast<uint8_t*>(*buf);
  for (size_t i = 0; i < nbytes; ++i) {
    bytes[i] ^= key;
  }
  (void)buf_size;
  return nbytes;
}

extern "C" size_t AlwaysFailFilterFunc(unsigned flags, size_t cd_nelmts,
                                       const unsigned cd_values[],
                                       size_t nbytes, size_t* buf_size,
                                       void** buf) {
  g_probe.last_filter_flags = flags;
  (void)cd_nelmts;
  (void)cd_values;
  (void)nbytes;
  (void)buf_size;
  (void)buf;
  return 0;
}

extern "C" htri_t CanApplyOk(hid_t dcpl_id, hid_t type_id, hid_t space_id) {
  ++g_probe.can_apply_calls;
  g_probe.last_can_apply_dcpl = dcpl_id;
  g_probe.last_can_apply_type = type_id;
  g_probe.last_can_apply_space = space_id;
  return 1;
}

extern "C" htri_t CanApplyFalse(hid_t, hid_t, hid_t) { return 0; }
extern "C" htri_t CanApplyErr(hid_t, hid_t, hid_t) { return -1; }
extern "C" herr_t SetLocalOk(hid_t dcpl_id, hid_t type_id, hid_t space_id) {
  ++g_probe.set_local_calls;
  g_probe.last_set_local_dcpl = dcpl_id;
  g_probe.last_set_local_type = type_id;
  g_probe.last_set_local_space = space_id;
  return 0;
}
extern "C" herr_t SetLocalErr(hid_t, hid_t, hid_t) { return -1; }

H5Z_class2_t MakeFilterClass(H5Z_filter_t id, const char* name,
                             unsigned encoder = 1, unsigned decoder = 1,
                             H5Z_can_apply_func_t can_apply = nullptr,
                             H5Z_set_local_func_t set_local = nullptr,
                             H5Z_func_t filter = XorFilterFunc) {
  return H5Z_class2_t{
      .version = H5Z_CLASS_T_VERS,
      .id = id,
      .encoder_present = encoder,
      .decoder_present = decoder,
      .name = name,
      .can_apply = can_apply,
      .set_local = set_local,
      .filter = filter,
  };
}

TEST(H5ZTest, BuiltInFiltersAvailableAndInfoQueries) {
  for (H5Z_filter_t id :
       {H5Z_FILTER_DEFLATE, H5Z_FILTER_SHUFFLE, H5Z_FILTER_FLETCHER32,
        H5Z_FILTER_SZIP, H5Z_FILTER_NBIT, H5Z_FILTER_SCALEOFFSET}) {
    EXPECT_GT(H5Zfilter_avail(id), 0);
    unsigned info = 0;
    EXPECT_GE(H5Zget_filter_info(id, &info), 0);
    EXPECT_EQ(info, static_cast<unsigned>(H5Z_FILTER_CONFIG_ENCODE_ENABLED |
                                          H5Z_FILTER_CONFIG_DECODE_ENABLED));
    // Passing nullptr for flags is explicitly supported.
    EXPECT_GE(H5Zget_filter_info(id, nullptr), 0);
  }

  // Unregistered filter ID is unavailable (plugins disabled in google3).
  constexpr H5Z_filter_t kUnusedId = 499;
  EXPECT_EQ(H5Zfilter_avail(kUnusedId), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Zfilter_avail(-1), 0);
  EXPECT_LT(H5Zfilter_avail(H5Z_FILTER_MAX + 1), 0);

  unsigned flags = 0;
  EXPECT_LT(H5Zget_filter_info(kUnusedId, &flags), 0);
}

TEST(H5ZTest, RegisterValidationDeprecatedClass1AndTableGrowth) {
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Zregister(nullptr), 0);

    const H5Z_class2_t neg_id = MakeFilterClass(-1, "neg");
    EXPECT_LT(H5Zregister(&neg_id), 0);

    const H5Z_class2_t over_max = MakeFilterClass(H5Z_FILTER_MAX + 1, "big");
    EXPECT_LT(H5Zregister(&over_max), 0);

    const H5Z_class2_t reserved = MakeFilterClass(100, "reserved");
    EXPECT_LT(H5Zregister(&reserved), 0);

    const H5Z_class2_t no_fn =
        MakeFilterClass(310, "nofn", 1, 1, nullptr, nullptr, nullptr);
    EXPECT_LT(H5Zregister(&no_fn), 0);

    EXPECT_LT(H5Zunregister(-1), 0);
    EXPECT_LT(H5Zunregister(H5Z_FILTER_MAX + 1), 0);
    EXPECT_LT(H5Zunregister(H5Z_FILTER_DEFLATE), 0);
    EXPECT_LT(H5Zunregister(498), 0);
  }

  // 1. Register and replace a H5Z_class2_t filter, checking encoder/decoder
  // flag bit combinations via H5Zget_filter_info.
  constexpr H5Z_filter_t kId = 310;
  absl::Cleanup cleanup_310 = [&] {
    if (H5Zfilter_avail(kId) > 0) {
      H5Zunregister(kId);
    }
  };
  const H5Z_class2_t enc_only = MakeFilterClass(kId, "enc_only", 1, 0);
  ASSERT_GE(H5Zregister(&enc_only), 0);
  EXPECT_GT(H5Zfilter_avail(kId), 0);
  unsigned info = 0;
  ASSERT_GE(H5Zget_filter_info(kId, &info), 0);
  EXPECT_EQ(info, static_cast<unsigned>(H5Z_FILTER_CONFIG_ENCODE_ENABLED));

  const H5Z_class2_t dec_only = MakeFilterClass(kId, "dec_only", 0, 1);
  ASSERT_GE(H5Zregister(&dec_only), 0);
  ASSERT_GE(H5Zget_filter_info(kId, &info), 0);
  EXPECT_EQ(info, static_cast<unsigned>(H5Z_FILTER_CONFIG_DECODE_ENABLED));

  const H5Z_class2_t neither = MakeFilterClass(kId, "neither", 0, 0);
  ASSERT_GE(H5Zregister(&neither), 0);
  ASSERT_GE(H5Zget_filter_info(kId, &info), 0);
  EXPECT_EQ(info, 0U);

  // 2. Register deprecated H5Z_class1_t struct (translated via id != 1).
  constexpr H5Z_filter_t kLegacyId = 311;
  absl::Cleanup cleanup_311 = [&] {
    if (H5Zfilter_avail(kLegacyId) > 0) {
      H5Zunregister(kLegacyId);
    }
  };
  const H5Z_class1_t legacy_cls = {
      .id = kLegacyId,
      .name = "legacy_v1",
      .can_apply = nullptr,
      .set_local = nullptr,
      .filter = XorFilterFunc,
  };
  ASSERT_GE(H5Zregister(&legacy_cls), 0);
  EXPECT_GT(H5Zfilter_avail(kLegacyId), 0);
  ASSERT_GE(H5Zget_filter_info(kLegacyId, &info), 0);
  EXPECT_EQ(info, static_cast<unsigned>(H5Z_FILTER_CONFIG_ENCODE_ENABLED |
                                        H5Z_FILTER_CONFIG_DECODE_ENABLED));

  // 3. Register enough filters to force H5Z_table_g reallocation past 32 slots.
  constexpr H5Z_filter_t kBatchStart = 400;
  constexpr int kBatchCount = 30;
  std::vector<H5Z_filter_t> registered;
  absl::Cleanup cleanup_batch = [&] {
    for (H5Z_filter_t id : registered) {
      H5Zunregister(id);
    }
  };
  for (int i = 0; i < kBatchCount; ++i) {
    const H5Z_filter_t id = kBatchStart + i;
    const H5Z_class2_t cls = MakeFilterClass(id, "batch");
    ASSERT_GE(H5Zregister(&cls), 0);
    registered.push_back(id);
  }
  EXPECT_GT(H5Zfilter_avail(kBatchStart + kBatchCount - 1), 0);
}

TEST(H5ZTest, UnregisterBlockedByOpenDatasetOrGroupAndFlushesFiles) {
  constexpr H5Z_filter_t kFilterA = 312;
  constexpr H5Z_filter_t kFilterB = 313;
  const H5Z_class2_t cls_a = MakeFilterClass(kFilterA, "unreg_a");
  const H5Z_class2_t cls_b = MakeFilterClass(kFilterB, "unreg_b");
  ASSERT_GE(H5Zregister(&cls_a), 0);
  ASSERT_GE(H5Zregister(&cls_b), 0);
  absl::Cleanup cleanup_filters = [&] {
    if (H5Zfilter_avail(kFilterA) > 0) {
      H5Zunregister(kFilterA);
    }
    if (H5Zfilter_avail(kFilterB) > 0) {
      H5Zunregister(kFilterB);
    }
  };

  // Create a real read-only file alongside an in-memory writable file so
  // H5Z__flush_file_cb traverses both H5F_ACC_RDONLY and H5F_ACC_RDWR files.
  TempFile ro_path("h5z_ro_flush.h5");
  {
    ScopedFile tmp(H5Fcreate(ro_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             H5P_DEFAULT));
    ASSERT_TRUE(tmp.valid());
  }
  ScopedFile ro_file(H5Fopen(ro_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(ro_file.valid());

  ScopedFile rw_file = CreateInMemoryFile("h5z_rw_flush.h5");
  ASSERT_TRUE(rw_file.valid());

  constexpr hsize_t kDim = 8;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  // Plain dataset and filtered dataset.
  ScopedDataset plain_dset(H5Dcreate2(rw_file.get(), "plain_d",
                                      H5T_NATIVE_INT32, space.get(),
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(plain_dset.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kDim), 0);
  ASSERT_GE(H5Pset_filter(dcpl.get(), kFilterA, H5Z_FLAG_MANDATORY, 0, nullptr),
            0);
  ScopedDataset filtered_dset(H5Dcreate2(rw_file.get(), "filt_d",
                                         H5T_NATIVE_INT32, space.get(),
                                         H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(filtered_dset.valid());

  // Plain group and filtered group.
  ScopedGroup plain_grp(
      H5Gcreate2(rw_file.get(), "plain_g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(plain_grp.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_filter(gcpl.get(), kFilterA, H5Z_FLAG_OPTIONAL, 0, nullptr),
            0);
  ScopedGroup filtered_grp(H5Gcreate2(rw_file.get(), "filt_g", H5P_DEFAULT,
                                      gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(filtered_grp.valid());

  // 1. Open dataset using kFilterA blocks unregister.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Zunregister(kFilterA), 0);
  }
  filtered_dset.reset();

  // 2. Open group using kFilterA still blocks unregister.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Zunregister(kFilterA), 0);
  }
  filtered_grp.reset();

  // 3. With no open users, unregistering kFilterA (before kFilterB in table)
  // flushes rw_file, skips ro_file, and shifts kFilterB down via memmove.
  ASSERT_GE(H5Zunregister(kFilterA), 0);
  EXPECT_EQ(H5Zfilter_avail(kFilterA), 0);
  EXPECT_GT(H5Zfilter_avail(kFilterB), 0);
  ASSERT_GE(H5Zunregister(kFilterB), 0);
}

TEST(H5ZTest, PreludeCallbacksDirectAndIgnoreFilters) {
  g_probe = PreludeProbe{};
  constexpr H5Z_filter_t kNoEnc = 320;
  constexpr H5Z_filter_t kCanErr = 321;
  constexpr H5Z_filter_t kCanFalse = 322;
  constexpr H5Z_filter_t kSetErr = 323;
  constexpr H5Z_filter_t kOk = 324;

  const std::array<H5Z_class2_t, 5> classes = {
      MakeFilterClass(kNoEnc, "no_enc", 0, 1),
      MakeFilterClass(kCanErr, "can_err", 1, 1, CanApplyErr),
      MakeFilterClass(kCanFalse, "can_false", 1, 1, CanApplyFalse),
      MakeFilterClass(kSetErr, "set_err", 1, 1, CanApplyOk, SetLocalErr),
      MakeFilterClass(kOk, "ok_cb", 1, 1, CanApplyOk, SetLocalOk),
  };
  for (const auto& cls : classes) {
    ASSERT_GE(H5Zregister(&cls), 0);
  }
  absl::Cleanup cleanup_classes = [&] {
    for (const auto& cls : classes) {
      if (H5Zfilter_avail(cls.id) > 0) {
        H5Zunregister(cls.id);
      }
    }
  };

  // 1. Default and contiguous/empty DCPLs pass prelude callbacks trivially.
  EXPECT_GE(H5Z_can_apply(H5P_DATASET_CREATE_DEFAULT, H5T_NATIVE_INT32), 0);
  EXPECT_GE(H5Z_set_local(H5P_DATASET_CREATE_DEFAULT, H5T_NATIVE_INT32), 0);

  ScopedPlist contig_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(contig_dcpl.valid());
  EXPECT_GE(H5Z_can_apply(contig_dcpl.get(), H5T_NATIVE_INT32), 0);
  EXPECT_GE(H5Z_set_local(contig_dcpl.get(), H5T_NATIVE_INT32), 0);

  constexpr hsize_t kChunkDim = 8;
  ScopedPlist empty_chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(empty_chunk_dcpl.valid());
  ASSERT_GE(H5Pset_chunk(empty_chunk_dcpl.get(), 1, &kChunkDim), 0);
  EXPECT_GE(H5Z_can_apply(empty_chunk_dcpl.get(), H5T_NATIVE_INT32), 0);

  auto make_dcpl_with = [&](H5Z_filter_t id, unsigned flags) {
    ScopedPlist p(H5Pcreate(H5P_DATASET_CREATE));
    if (p.valid()) {
      H5Pset_chunk(p.get(), 1, &kChunkDim);
      H5Pset_filter(p.get(), id, flags, 0, nullptr);
    }
    return p;
  };

  // 2. Prelude error/skip cases for dcpl.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Z_can_apply(
                  make_dcpl_with(497, H5Z_FLAG_MANDATORY).get(), H5T_NATIVE_INT32),
              0);
    EXPECT_LT(H5Z_can_apply(
                  make_dcpl_with(kNoEnc, H5Z_FLAG_MANDATORY).get(), H5T_NATIVE_INT32),
              0);
    EXPECT_LT(H5Z_can_apply(
                  make_dcpl_with(kCanErr, H5Z_FLAG_MANDATORY).get(), H5T_NATIVE_INT32),
              0);
    EXPECT_LT(H5Z_can_apply(
                  make_dcpl_with(kCanFalse, H5Z_FLAG_MANDATORY).get(), H5T_NATIVE_INT32),
              0);
    EXPECT_LT(H5Z_set_local(
                  make_dcpl_with(kSetErr, H5Z_FLAG_MANDATORY).get(), H5T_NATIVE_INT32),
              0);
  }

  // Optional missing filter and optional can_apply==false succeed.
  EXPECT_GE(H5Z_can_apply(make_dcpl_with(497, H5Z_FLAG_OPTIONAL).get(),
                          H5T_NATIVE_INT32),
            0);
  EXPECT_GE(H5Z_set_local(make_dcpl_with(497, H5Z_FLAG_OPTIONAL).get(),
                          H5T_NATIVE_INT32),
            0);
  EXPECT_GE(H5Z_can_apply(make_dcpl_with(kCanFalse, H5Z_FLAG_OPTIONAL).get(),
                          H5T_NATIVE_INT32),
            0);

  // Success with callbacks + nullptr callbacks (deflate).
  ScopedPlist ok_dcpl = make_dcpl_with(kOk, H5Z_FLAG_MANDATORY);
  EXPECT_GE(H5Z_can_apply(ok_dcpl.get(), H5T_NATIVE_INT32), 0);
  EXPECT_GE(H5Z_set_local(ok_dcpl.get(), H5T_NATIVE_INT32), 0);
  EXPECT_EQ(g_probe.can_apply_calls, 1);  // kOk only (kSetErr skipped can_apply)
  EXPECT_EQ(g_probe.set_local_calls, 1);

  ScopedPlist defl_dcpl = make_dcpl_with(H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL);
  EXPECT_GE(H5Z_can_apply(defl_dcpl.get(), H5T_NATIVE_INT32), 0);
  EXPECT_GE(H5Z_set_local(defl_dcpl.get(), H5T_NATIVE_INT32), 0);

  // 3. Direct prelude callbacks (pass -1 for dcpl/type/space IDs).
  H5O_pline_t direct_pline{};
  absl::Cleanup cleanup_direct = [&] {
    H5Z_delete(&direct_pline, H5Z_FILTER_ALL);
  };
  ASSERT_GE(H5Z_append(&direct_pline, kOk, H5Z_FLAG_MANDATORY, 0, nullptr), 0);
  ASSERT_GE(H5Z_can_apply_direct(&direct_pline), 0);
  ASSERT_GE(H5Z_set_local_direct(&direct_pline), 0);
  EXPECT_EQ(g_probe.last_can_apply_dcpl, static_cast<hid_t>(-1));
  EXPECT_EQ(g_probe.last_can_apply_type, static_cast<hid_t>(-1));
  EXPECT_EQ(g_probe.last_can_apply_space, static_cast<hid_t>(-1));
  EXPECT_EQ(g_probe.last_set_local_dcpl, static_cast<hid_t>(-1));
  EXPECT_EQ(g_probe.last_set_local_type, static_cast<hid_t>(-1));
  EXPECT_EQ(g_probe.last_set_local_space, static_cast<hid_t>(-1));

  ASSERT_GE(H5Z_append(&direct_pline, kSetErr, H5Z_FLAG_MANDATORY, 0, nullptr),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Z_set_local_direct(&direct_pline), 0);
  }
  ASSERT_GE(H5Z_append(&direct_pline, kCanErr, H5Z_FLAG_MANDATORY, 0, nullptr),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Z_can_apply_direct(&direct_pline), 0);
  }

  // 4. H5Z_ignore_filters across simple, scalar, null spaces + invalid dcpl.
  ScopedSpace simple_sp(H5Screate_simple(1, &kChunkDim, nullptr));
  ScopedSpace scalar_sp(H5Screate(H5S_SCALAR));
  ScopedSpace null_sp(H5Screate(H5S_NULL));
  ASSERT_TRUE(simple_sp.valid() && scalar_sp.valid() && null_sp.valid());

  auto* simple_ptr =
      static_cast<const H5S_t*>(H5I_object_verify(simple_sp.get(), H5I_DATASPACE));
  auto* scalar_ptr =
      static_cast<const H5S_t*>(H5I_object_verify(scalar_sp.get(), H5I_DATASPACE));
  auto* null_ptr =
      static_cast<const H5S_t*>(H5I_object_verify(null_sp.get(), H5I_DATASPACE));
  ASSERT_NE(simple_ptr, nullptr);
  ASSERT_NE(scalar_ptr, nullptr);
  ASSERT_NE(null_ptr, nullptr);

  EXPECT_EQ(H5Z_ignore_filters(ok_dcpl.get(), simple_ptr), 0);
  EXPECT_EQ(H5Z_ignore_filters(empty_chunk_dcpl.get(), scalar_ptr), 0);

  ScopedPlist opt_dcpl = make_dcpl_with(kOk, H5Z_FLAG_OPTIONAL);
  EXPECT_GT(H5Z_ignore_filters(opt_dcpl.get(), scalar_ptr), 0);
  EXPECT_GT(H5Z_ignore_filters(opt_dcpl.get(), null_ptr), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Z_ignore_filters(H5I_INVALID_HID, scalar_ptr), 0);
    EXPECT_LT(H5Z_ignore_filters(ok_dcpl.get(), scalar_ptr), 0);
    EXPECT_LT(H5Z_ignore_filters(ok_dcpl.get(), null_ptr), 0);
  }
}

TEST(H5ZTest, PipelineManagementAppendModifyFindDeleteAndCopyRealloc) {
  H5O_pline_t pline{};
  absl::Cleanup cleanup_pline = [&] { H5Z_delete(&pline, H5Z_FILTER_ALL); };

  // Deleting from empty pipeline succeeds as a no-op.
  EXPECT_GE(H5Z_delete(&pline, H5Z_FILTER_DEFLATE), 0);
  EXPECT_EQ(H5Z_filter_in_pline(&pline, H5Z_FILTER_DEFLATE), 0);
  EXPECT_GT(H5Z_all_filters_avail(&pline), 0);

  // Append filters with 0, inline (<=4), and heap (>4) client parameters.
  const std::array<unsigned, 2> small_cd = {11, 22};
  const std::array<unsigned, 6> large_cd = {1, 2, 3, 4, 5, 6};
  ASSERT_GE(
      H5Z_append(&pline, H5Z_FILTER_FLETCHER32, H5Z_FLAG_MANDATORY, 0, nullptr),
      0);
  ASSERT_GE(
      H5Z_append(&pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL, 1, small_cd.data()),
      0);
  ASSERT_GE(H5Z_append(&pline, 350, H5Z_FLAG_OPTIONAL, large_cd.size(),
                       large_cd.data()),
            0);
  ASSERT_GE(
      H5Z_append(&pline, 351, H5Z_FLAG_MANDATORY, small_cd.size(), small_cd.data()),
      0);

  // Assign inline and heap filter names to test pointer fixups & frees in
  // H5Z_delete.
  ASSERT_EQ(pline.nused, 4U);
  std::strcpy(pline.filter[0]._name, "short0");
  pline.filter[0].name = pline.filter[0]._name;
  pline.filter[1].name = H5MM_strdup("very_long_custom_filter_name_1");
  ASSERT_NE(pline.filter[1].name, nullptr);
  std::strcpy(pline.filter[2]._name, "short2");
  pline.filter[2].name = pline.filter[2]._name;
  pline.filter[3].name = H5MM_strdup("very_long_custom_filter_name_3");
  ASSERT_NE(pline.filter[3].name, nullptr);

  EXPECT_GT(H5Z_filter_in_pline(&pline, H5Z_FILTER_DEFLATE), 0);
  EXPECT_EQ(H5Z_filter_in_pline(&pline, 399), 0);
  EXPECT_NE(H5Z_filter_info(&pline, 350), nullptr);
  EXPECT_EQ(H5Z_all_filters_avail(&pline), 0);  // 350/351 unregistered.

  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Z_filter_info(&pline, 399), nullptr);
    EXPECT_LT(H5Z_delete(&pline, 399), 0);
  }

  // Modify filter 350: heap (>4) -> inline (2) -> 0 -> heap (6).
  ASSERT_GE(H5Z_modify(&pline, 350, H5Z_FLAG_MANDATORY, small_cd.size(),
                       small_cd.data()),
            0);
  EXPECT_EQ(pline.filter[2].cd_values, pline.filter[2]._cd_values);
  ASSERT_GE(H5Z_modify(&pline, 350, H5Z_FLAG_OPTIONAL, 0, nullptr), 0);
  EXPECT_EQ(pline.filter[2].cd_values, nullptr);
  ASSERT_GE(H5Z_modify(&pline, 350, H5Z_FLAG_OPTIONAL, large_cd.size(),
                       large_cd.data()),
            0);
  EXPECT_NE(pline.filter[2].cd_values, pline.filter[2]._cd_values);

  // Delete middle filter (index 1, long heap name) -> shifts 2 (short inline
  // name + heap cd_values) and 3 (long heap name + inline _cd_values) down.
  ASSERT_GE(H5Z_delete(&pline, H5Z_FILTER_DEFLATE), 0);
  EXPECT_EQ(pline.nused, 3U);
  EXPECT_EQ(pline.filter[1].name, pline.filter[1]._name);
  EXPECT_EQ(pline.filter[2].cd_values, pline.filter[2]._cd_values);

  // Delete last filter (index 2, long heap name).
  ASSERT_GE(H5Z_delete(&pline, 351), 0);
  EXPECT_EQ(pline.nused, 2U);

  // Delete remaining filter with heap-allocated cd_values (cd_nelmts > 4).
  ASSERT_GE(H5Z_delete(&pline, 350), 0);
  EXPECT_EQ(pline.nused, 1U);

  // H5Z_find found / not-found with try=true and try=false.
  H5Z_class2_t* cls = nullptr;
  EXPECT_GE(H5Z_find(false, H5Z_FILTER_DEFLATE, &cls), 0);
  EXPECT_NE(cls, nullptr);
  EXPECT_GE(H5Z_find(true, 496, &cls), 0);
  EXPECT_EQ(cls, nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Z_find(false, 496, &cls), 0);
    EXPECT_EQ(cls, nullptr);
  }

  // Exceeding H5Z_MAX_NFILTERS (32) fails.
  H5O_pline_t full_pline{};
  absl::Cleanup cleanup_full = [&] { H5Z_delete(&full_pline, H5Z_FILTER_ALL); };
  for (int i = 0; i < H5Z_MAX_NFILTERS; ++i) {
    ASSERT_GE(H5Z_append(&full_pline, 400 + i, H5Z_FLAG_OPTIONAL, 0, nullptr),
              0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Z_append(&full_pline, 450, H5Z_FLAG_OPTIONAL, 0, nullptr), 0);
  }

  // Copy a DCPL with 2 filters (so H5O__pline_copy sets nalloc=nused=2), then
  // append a 3rd filter to exercise H5Z_append's _cd_values realloc relocation.
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_filter(dcpl.get(), H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL,
                          small_cd.size(), small_cd.data()),
            0);
  ASSERT_GE(H5Pset_filter(dcpl.get(), H5Z_FILTER_SHUFFLE, H5Z_FLAG_OPTIONAL,
                          large_cd.size(), large_cd.data()),
            0);
  ScopedPlist dcpl_copy(H5Pcopy(dcpl.get()));
  ASSERT_TRUE(dcpl_copy.valid());
  ASSERT_GE(H5Pset_filter(dcpl_copy.get(), H5Z_FILTER_FLETCHER32,
                          H5Z_FLAG_MANDATORY, 0, nullptr),
            0);
  unsigned out_cd[2] = {0, 0};
  size_t out_nelmts = 2;
  ASSERT_GE(H5Pget_filter_by_id2(dcpl_copy.get(), H5Z_FILTER_DEFLATE, nullptr,
                                  &out_nelmts, out_cd, 0, nullptr, nullptr),
            0);
  EXPECT_THAT(out_cd, ElementsAre(11U, 22U));
}

TEST(H5ZTest, PipelineExecutionWriteReadCallbacksAndMask) {
  constexpr H5Z_filter_t kXorId = 330;
  constexpr H5Z_filter_t kFailId = 331;
  const H5Z_class2_t xor_cls = MakeFilterClass(kXorId, "xor_test");
  const H5Z_class2_t fail_cls =
      MakeFilterClass(kFailId, "fail_test", 1, 1, nullptr, nullptr,
                      AlwaysFailFilterFunc);
  ASSERT_GE(H5Zregister(&xor_cls), 0);
  ASSERT_GE(H5Zregister(&fail_cls), 0);
  absl::Cleanup cleanup_filters = [&] {
    if (H5Zfilter_avail(kXorId) > 0) {
      H5Zunregister(kXorId);
    }
    if (H5Zfilter_avail(kFailId) > 0) {
      H5Zunregister(kFailId);
    }
  };

  auto alloc_buf = [](std::initializer_list<uint8_t> init) -> void* {
    void* p = H5allocate_memory(init.size(), false);
    if (p != nullptr) {
      std::memcpy(p, init.begin(), init.size());
    }
    return p;
  };

  void* buf = alloc_buf({0x01, 0x02, 0x03, 0x04});
  ASSERT_NE(buf, nullptr);
  absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };

  size_t nbytes = 4;
  size_t buf_size = 4;
  unsigned mask = 0xff;
  H5Z_cb_t no_cb{nullptr, nullptr};

  // 1. Null pipeline clears filter_mask and succeeds.
  ASSERT_GE(H5Z_pipeline(nullptr, 0, &mask, H5Z_ENABLE_EDC, no_cb, &nbytes,
                         &buf_size, &buf),
            0);
  EXPECT_EQ(mask, 0U);

  // 2. Write + read with 2-filter pipeline and selective mask exclusion.
  H5O_pline_t pline{};
  absl::Cleanup cleanup_pline = [&] { H5Z_delete(&pline, H5Z_FILTER_ALL); };
  const unsigned key1 = 0x10;
  const unsigned key2 = 0x20;
  ASSERT_GE(H5Z_append(&pline, kXorId, H5Z_FLAG_MANDATORY, 1, &key1), 0);
  ASSERT_GE(H5Z_append(&pline, kXorId, H5Z_FLAG_MANDATORY, 1, &key2), 0);

  // Exclude filter 0 via mask: only key2 is applied.
  mask = 0x1U;
  ASSERT_GE(H5Z_pipeline(&pline, 0, &mask, H5Z_ENABLE_EDC, no_cb, &nbytes,
                         &buf_size, &buf),
            0);
  EXPECT_EQ(mask, 0x1U);
  EXPECT_THAT(std::vector<uint8_t>(static_cast<uint8_t*>(buf),
                                   static_cast<uint8_t*>(buf) + 4),
              ElementsAre(0x21, 0x22, 0x23, 0x24));

  // Reverse with mask=0x1 skips filter 0 and undoes key2; H5Z_DISABLE_EDC sets
  // H5Z_FLAG_SKIP_EDC on filter flags.
  g_probe.last_filter_flags = 0;
  ASSERT_GE(H5Z_pipeline(&pline, H5Z_FLAG_REVERSE, &mask, H5Z_DISABLE_EDC,
                         no_cb, &nbytes, &buf_size, &buf),
            0);
  EXPECT_NE(g_probe.last_filter_flags & H5Z_FLAG_SKIP_EDC, 0U);
  EXPECT_THAT(std::vector<uint8_t>(static_cast<uint8_t*>(buf),
                                   static_cast<uint8_t*>(buf) + 4),
              ElementsAre(0x01, 0x02, 0x03, 0x04));

  // 3. Filter failure handling on write (optional vs mandatory + callbacks).
  H5O_pline_t opt_fail_pline{};
  absl::Cleanup cleanup_opt_fail = [&] {
    H5Z_delete(&opt_fail_pline, H5Z_FILTER_ALL);
  };
  ASSERT_GE(H5Z_append(&opt_fail_pline, 495, H5Z_FLAG_OPTIONAL, 0, nullptr), 0);
  ASSERT_GE(H5Z_append(&opt_fail_pline, kFailId, H5Z_FLAG_OPTIONAL, 0, nullptr),
            0);
  mask = 0;
  ASSERT_GE(H5Z_pipeline(&opt_fail_pline, 0, &mask, H5Z_ENABLE_EDC, no_cb,
                         &nbytes, &buf_size, &buf),
            0);
  EXPECT_EQ(mask, 0x3U);

  auto cb_fail = [](H5Z_filter_t, void*, size_t, void* ud) -> H5Z_cb_return_t {
    ++(*static_cast<int*>(ud));
    return H5Z_CB_FAIL;
  };
  auto cb_cont = [](H5Z_filter_t, void*, size_t, void* ud) -> H5Z_cb_return_t {
    ++(*static_cast<int*>(ud));
    return H5Z_CB_CONT;
  };

  int cb_calls = 0;
  H5Z_cb_t fail_cb{cb_fail, &cb_calls};
  H5Z_cb_t cont_cb{cb_cont, &cb_calls};

  H5O_pline_t mand_fail{};
  absl::Cleanup cleanup_mand_fail = [&] {
    H5Z_delete(&mand_fail, H5Z_FILTER_ALL);
  };
  ASSERT_GE(H5Z_append(&mand_fail, kFailId, H5Z_FLAG_MANDATORY, 0, nullptr), 0);

  // Write failure paths.
  {
    ScopedErrorSilence silence;
    mask = 0;
    EXPECT_LT(H5Z_pipeline(&mand_fail, 0, &mask, H5Z_ENABLE_EDC, no_cb, &nbytes,
                           &buf_size, &buf),
              0);
    mask = 0;
    EXPECT_LT(H5Z_pipeline(&mand_fail, 0, &mask, H5Z_ENABLE_EDC, fail_cb,
                           &nbytes, &buf_size, &buf),
              0);
  }
  mask = 0;
  EXPECT_GE(H5Z_pipeline(&mand_fail, 0, &mask, H5Z_ENABLE_EDC, cont_cb, &nbytes,
                         &buf_size, &buf),
            0);
  EXPECT_EQ(mask, 0x1U);

  // Read failure paths.
  {
    ScopedErrorSilence silence;
    mask = 0;
    EXPECT_LT(H5Z_pipeline(&mand_fail, H5Z_FLAG_REVERSE, &mask, H5Z_ENABLE_EDC,
                           no_cb, &nbytes, &buf_size, &buf),
              0);
    mask = 0;
    EXPECT_LT(H5Z_pipeline(&mand_fail, H5Z_FLAG_REVERSE, &mask, H5Z_ENABLE_EDC,
                           fail_cb, &nbytes, &buf_size, &buf),
              0);
  }
  mask = 0;
  EXPECT_GE(H5Z_pipeline(&mand_fail, H5Z_FLAG_REVERSE, &mask, H5Z_ENABLE_EDC,
                         cont_cb, &nbytes, &buf_size, &buf),
            0);
  EXPECT_EQ(mask, 0x1U);
  EXPECT_EQ(cb_calls, 4);

  // Unregistered mandatory filter on write, and unregistered named/unnamed
  // filters on read.
  H5O_pline_t unreg_named{};
  H5O_pline_t unreg_unnamed{};
  absl::Cleanup cleanup_unreg = [&] {
    H5Z_delete(&unreg_named, H5Z_FILTER_ALL);
    H5Z_delete(&unreg_unnamed, H5Z_FILTER_ALL);
  };
  ASSERT_GE(H5Z_append(&unreg_named, 494, H5Z_FLAG_MANDATORY, 0, nullptr), 0);
  std::strcpy(unreg_named.filter[0]._name, "missing_f");
  unreg_named.filter[0].name = unreg_named.filter[0]._name;
  ASSERT_GE(H5Z_append(&unreg_unnamed, 493, H5Z_FLAG_MANDATORY, 0, nullptr), 0);

  {
    ScopedErrorSilence silence;
    mask = 0;
    EXPECT_LT(H5Z_pipeline(&unreg_named, 0, &mask, H5Z_ENABLE_EDC, no_cb,
                           &nbytes, &buf_size, &buf),
              0);
    mask = 0;
    EXPECT_LT(H5Z_pipeline(&unreg_named, H5Z_FLAG_REVERSE, &mask,
                           H5Z_ENABLE_EDC, no_cb, &nbytes, &buf_size, &buf),
              0);
    mask = 0;
    EXPECT_LT(H5Z_pipeline(&unreg_unnamed, H5Z_FLAG_REVERSE, &mask,
                           H5Z_ENABLE_EDC, no_cb, &nbytes, &buf_size, &buf),
              0);
  }
}

TEST(H5ZTest, LibraryCloseAndReopenReinitializesFilterTable) {
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);
  EXPECT_GT(H5Zfilter_avail(H5Z_FILTER_DEFLATE), 0);
  EXPECT_GT(H5Zfilter_avail(H5Z_FILTER_FLETCHER32), 0);
  EXPECT_GT(H5Zfilter_avail(H5Z_FILTER_NBIT), 0);
}

}  // namespace
}  // namespace hdf5
