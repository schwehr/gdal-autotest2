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
// Tests dataset access property list (DAPL) properties, callbacks, encoding/
// decoding, and error handling: chunk cache parameters, VDS view/printf gap/
// prefix/spatial tree, append flush, and external file prefix
// (third_party/hdf5/src/H5Pdapl.c).
//
// Build/feature caveats: Tests run in the default google3 non-MPI Linux build.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pdapl.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

herr_t DummyAppendFlushCb(hid_t /*dataset_id*/, hsize_t* /*cur_dims*/,
                          void* /*op_data*/) {
  return 0;
}

TEST(H5PdaplTest, ChunkCacheDefaultsCustomValuesAndEncodeDecode) {
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl.valid());

  // Query default FAPL chunk cache parameters for comparison.
  size_t fapl_nslots = 0;
  size_t fapl_nbytes = 0;
  double fapl_w0 = -1.0;
  ASSERT_GE(
      H5Pget_cache(H5P_FILE_ACCESS_DEFAULT, nullptr, &fapl_nslots, &fapl_nbytes,
                   &fapl_w0),
      0);

  // 1. Default DAPL inherits defaults from H5P_FILE_ACCESS_DEFAULT.
  size_t nslots = 0;
  size_t nbytes = 0;
  double w0 = -1.0;
  ASSERT_GE(H5Pget_chunk_cache(H5P_DATASET_ACCESS_DEFAULT, &nslots, &nbytes, &w0),
            0);
  EXPECT_EQ(nslots, fapl_nslots);
  EXPECT_EQ(nbytes, fapl_nbytes);
  EXPECT_DOUBLE_EQ(w0, fapl_w0);

  // Passing nullptr output pointers is explicitly supported.
  EXPECT_GE(H5Pget_chunk_cache(dapl.get(), nullptr, nullptr, nullptr), 0);

  // Encode/decode default DAPL exercises the zero-size default fast-path.
  size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(dapl.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> def_buf(enc_size);
  ASSERT_GE(H5Pencode2(dapl.get(), def_buf.data(), &enc_size, H5P_DEFAULT), 0);
  ScopedPlist dec_def(H5Pdecode(def_buf.data()));
  ASSERT_TRUE(dec_def.valid());
  EXPECT_GT(H5Pequal(dapl.get(), dec_def.get()), 0);

  // 2. Override each parameter individually and together, then round-trip.
  ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 1031, 4 * 1024 * 1024, 0.25), 0);
  ASSERT_GE(H5Pget_chunk_cache(dapl.get(), &nslots, &nbytes, &w0), 0);
  EXPECT_EQ(nslots, 1031U);
  EXPECT_EQ(nbytes, 4U * 1024 * 1024);
  EXPECT_DOUBLE_EQ(w0, 0.25);

  enc_size = 0;
  ASSERT_GE(H5Pencode2(dapl.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> custom_buf(enc_size);
  ASSERT_GE(H5Pencode2(dapl.get(), custom_buf.data(), &enc_size, H5P_DEFAULT),
            0);
  ScopedPlist dec_custom(H5Pdecode(custom_buf.data()));
  ASSERT_TRUE(dec_custom.valid());
  EXPECT_GT(H5Pequal(dapl.get(), dec_custom.get()), 0);

  size_t dec_slots = 0;
  size_t dec_bytes = 0;
  double dec_w0 = -1.0;
  ASSERT_GE(
      H5Pget_chunk_cache(dec_custom.get(), &dec_slots, &dec_bytes, &dec_w0), 0);
  EXPECT_EQ(dec_slots, 1031U);
  EXPECT_EQ(dec_bytes, 4U * 1024 * 1024);
  EXPECT_DOUBLE_EQ(dec_w0, 0.25);

  // Unsetting individual values falls back to default FAPL values.
  ASSERT_GE(H5Pset_chunk_cache(dapl.get(), H5D_CHUNK_CACHE_NSLOTS_DEFAULT,
                               H5D_CHUNK_CACHE_NBYTES_DEFAULT,
                               H5D_CHUNK_CACHE_W0_DEFAULT),
            0);
  ASSERT_GE(H5Pget_chunk_cache(dapl.get(), &nslots, &nbytes, &w0), 0);
  EXPECT_EQ(nslots, fapl_nslots);
  EXPECT_EQ(nbytes, fapl_nbytes);
  EXPECT_DOUBLE_EQ(w0, fapl_w0);

  // 3. Negative checks for invalid ID and out-of-range rdcc_w0.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_chunk_cache(dapl.get(), 10, 1024, 1.5), 0);
    EXPECT_LT(H5Pset_chunk_cache(H5P_FILE_ACCESS_DEFAULT, 10, 1024, 0.5), 0);
    EXPECT_LT(H5Pset_chunk_cache(H5I_INVALID_HID, 10, 1024, 0.5), 0);
    EXPECT_LT(
        H5Pget_chunk_cache(H5P_FILE_ACCESS_DEFAULT, &nslots, &nbytes, &w0), 0);
    EXPECT_LT(H5Pget_chunk_cache(H5I_INVALID_HID, &nslots, &nbytes, &w0), 0);
  }
}

TEST(H5PdaplTest, VirtualViewPrintfGapAndSpatialTree) {
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl.valid());

  // 1. Default values and nullptr getter tolerance.
  H5D_vds_view_t view = H5D_VDS_ERROR;
  ASSERT_GE(H5Pget_virtual_view(H5P_DATASET_ACCESS_DEFAULT, &view), 0);
  EXPECT_EQ(view, H5D_VDS_LAST_AVAILABLE);
  EXPECT_GE(H5Pget_virtual_view(dapl.get(), nullptr), 0);

  hsize_t gap = 99;
  ASSERT_GE(H5Pget_virtual_printf_gap(H5P_DATASET_ACCESS_DEFAULT, &gap), 0);
  EXPECT_EQ(gap, 0U);
  EXPECT_GE(H5Pget_virtual_printf_gap(dapl.get(), nullptr), 0);

  bool use_tree = false;
  ASSERT_GE(H5Pget_virtual_spatial_tree(H5P_DATASET_ACCESS_DEFAULT, &use_tree),
            0);
  EXPECT_TRUE(use_tree);

  // 2. Modify properties and verify encode/decode round-trip.
  ASSERT_GE(H5Pset_virtual_view(dapl.get(), H5D_VDS_FIRST_MISSING), 0);
  ASSERT_GE(H5Pset_virtual_printf_gap(dapl.get(), 17), 0);
  ASSERT_GE(H5Pset_virtual_spatial_tree(dapl.get(), false), 0);

  size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(dapl.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> buf(enc_size);
  ASSERT_GE(H5Pencode2(dapl.get(), buf.data(), &enc_size, H5P_DEFAULT), 0);
  ScopedPlist decoded(H5Pdecode(buf.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(dapl.get(), decoded.get()), 0);

  H5D_vds_view_t dec_view = H5D_VDS_ERROR;
  hsize_t dec_gap = 0;
  bool dec_tree = true;
  ASSERT_GE(H5Pget_virtual_view(decoded.get(), &dec_view), 0);
  ASSERT_GE(H5Pget_virtual_printf_gap(decoded.get(), &dec_gap), 0);
  ASSERT_GE(H5Pget_virtual_spatial_tree(decoded.get(), &dec_tree), 0);
  EXPECT_EQ(dec_view, H5D_VDS_FIRST_MISSING);
  EXPECT_EQ(dec_gap, 17U);
  EXPECT_FALSE(dec_tree);

  ASSERT_GE(H5Pset_virtual_spatial_tree(dapl.get(), true), 0);
  ASSERT_GE(H5Pget_virtual_spatial_tree(dapl.get(), &dec_tree), 0);
  EXPECT_TRUE(dec_tree);

  // 3. Negative checks.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Pset_virtual_view(dapl.get(), static_cast<H5D_vds_view_t>(99)), 0);
    EXPECT_LT(H5Pset_virtual_view(dapl.get(), H5D_VDS_ERROR), 0);
    EXPECT_LT(
        H5Pset_virtual_view(H5P_FILE_ACCESS_DEFAULT, H5D_VDS_FIRST_MISSING), 0);
    EXPECT_LT(H5Pget_virtual_view(H5I_INVALID_HID, &view), 0);

    EXPECT_LT(H5Pset_virtual_printf_gap(dapl.get(), HSIZE_UNDEF), 0);
    EXPECT_LT(H5Pset_virtual_printf_gap(H5P_FILE_ACCESS_DEFAULT, 1), 0);
    EXPECT_LT(H5Pget_virtual_printf_gap(H5I_INVALID_HID, &gap), 0);

    EXPECT_LT(H5Pget_virtual_spatial_tree(dapl.get(), nullptr), 0);
    EXPECT_LT(H5Pget_virtual_spatial_tree(H5I_INVALID_HID, &use_tree), 0);
    EXPECT_LT(H5Pset_virtual_spatial_tree(H5P_FILE_ACCESS_DEFAULT, false), 0);
    EXPECT_LT(H5Pset_virtual_spatial_tree(H5I_INVALID_HID, true), 0);
  }
}

TEST(H5PdaplTest, PrefixPropertiesCallbacksComparisonTruncationAndEncodeDecode) {
  ScopedPlist dapl_a(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl_a.valid());
  ScopedPlist dapl_b(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl_b.valid());

  // 1. Empty / unset prefixes return 0 and preserve comparison equality.
  std::array<char, 8> tiny_buf{};
  tiny_buf.fill('X');
  EXPECT_EQ(
      H5Pget_efile_prefix(H5P_DATASET_ACCESS_DEFAULT, tiny_buf.data(),
                          tiny_buf.size()),
      0);
  EXPECT_EQ(tiny_buf[0], 'X');
  EXPECT_EQ(
      H5Pget_virtual_prefix(H5P_DATASET_ACCESS_DEFAULT, tiny_buf.data(),
                            tiny_buf.size()),
      0);
  EXPECT_GT(H5Pequal(dapl_a.get(), dapl_b.get()), 0);

  // 2. Compare all combinations (NULL vs non-NULL, equal vs unequal strings)
  // independently for efile_prefix and virtual_prefix so both compare callbacks
  // hit all NULL/non-NULL orderings without short-circuiting each other.
  ASSERT_GE(H5Pset_efile_prefix(dapl_b.get(), "ext/dir_b"), 0);
  EXPECT_EQ(H5Pequal(dapl_a.get(), dapl_b.get()), 0);
  EXPECT_EQ(H5Pequal(dapl_b.get(), dapl_a.get()), 0);

  ASSERT_GE(H5Pset_virtual_prefix(dapl_b.get(), "vds/dir_b"), 0);
  EXPECT_EQ(H5Pequal(dapl_a.get(), dapl_b.get()), 0);
  EXPECT_EQ(H5Pequal(dapl_b.get(), dapl_a.get()), 0);

  ASSERT_GE(H5Pset_virtual_prefix(dapl_a.get(), "vds/dir_a"), 0);
  ASSERT_GE(H5Pset_efile_prefix(dapl_a.get(), "ext/dir_a"), 0);
  EXPECT_EQ(H5Pequal(dapl_a.get(), dapl_b.get()), 0);

  // Overwriting triggers *_del callbacks; matching values satisfy H5Pequal.
  ASSERT_GE(H5Pset_virtual_prefix(dapl_a.get(), "vds/dir_b"), 0);
  ASSERT_GE(H5Pset_efile_prefix(dapl_a.get(), "ext/dir_b"), 0);
  EXPECT_GT(H5Pequal(dapl_a.get(), dapl_b.get()), 0);

  // 3. Generic H5Pget invokes *_get callbacks (allocating a copy freed via
  // H5free_memory), and copy triggers *_copy callbacks.
  char* raw_vds = nullptr;
  char* raw_efile = nullptr;
  ASSERT_GE(H5Pget(dapl_a.get(), "vds_prefix", &raw_vds), 0);
  absl::Cleanup free_raw_vds = [&raw_vds] { H5free_memory(raw_vds); };
  ASSERT_GE(H5Pget(dapl_a.get(), "external file prefix", &raw_efile), 0);
  absl::Cleanup free_raw_efile = [&raw_efile] { H5free_memory(raw_efile); };
  EXPECT_STREQ(raw_vds, "vds/dir_b");
  EXPECT_STREQ(raw_efile, "ext/dir_b");

  ScopedPlist copied(H5Pcopy(dapl_a.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(dapl_a.get(), copied.get()), 0);

  // 4. Length queries, full reads, and small-buffer NUL truncation.
  EXPECT_EQ(H5Pget_virtual_prefix(dapl_a.get(), nullptr, 0), 9);
  EXPECT_EQ(H5Pget_efile_prefix(dapl_a.get(), nullptr, 0), 9);

  std::array<char, 5> trunc_vds{};
  std::array<char, 5> trunc_efile{};
  EXPECT_EQ(H5Pget_virtual_prefix(dapl_a.get(), trunc_vds.data(),
                                  trunc_vds.size()),
            9);
  EXPECT_EQ(H5Pget_efile_prefix(dapl_a.get(), trunc_efile.data(),
                                trunc_efile.size()),
            9);
  EXPECT_STREQ(trunc_vds.data(), "vds/");
  EXPECT_STREQ(trunc_efile.data(), "ext/");

  // 5. Encode/decode for non-empty prefixes and empty-string "" prefixes.
  size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(dapl_a.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> buf(enc_size);
  ASSERT_GE(H5Pencode2(dapl_a.get(), buf.data(), &enc_size, H5P_DEFAULT), 0);
  ScopedPlist decoded(H5Pdecode(buf.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(dapl_a.get(), decoded.get()), 0);

  std::array<char, 32> dec_vds{};
  std::array<char, 32> dec_efile{};
  EXPECT_EQ(
      H5Pget_virtual_prefix(decoded.get(), dec_vds.data(), dec_vds.size()), 9);
  EXPECT_EQ(
      H5Pget_efile_prefix(decoded.get(), dec_efile.data(), dec_efile.size()),
      9);
  EXPECT_STREQ(dec_vds.data(), "vds/dir_b");
  EXPECT_STREQ(dec_efile.data(), "ext/dir_b");

  // Empty string encodes length 0 and decodes back to NULL.
  ASSERT_GE(H5Pset_virtual_prefix(dapl_a.get(), ""), 0);
  ASSERT_GE(H5Pset_efile_prefix(dapl_a.get(), ""), 0);
  enc_size = 0;
  ASSERT_GE(H5Pencode2(dapl_a.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> empty_buf(enc_size);
  ASSERT_GE(H5Pencode2(dapl_a.get(), empty_buf.data(), &enc_size, H5P_DEFAULT),
            0);
  ScopedPlist dec_empty(H5Pdecode(empty_buf.data()));
  ASSERT_TRUE(dec_empty.valid());
  EXPECT_EQ(H5Pget_virtual_prefix(dec_empty.get(), nullptr, 0), 0);
  EXPECT_EQ(H5Pget_efile_prefix(dec_empty.get(), nullptr, 0), 0);

  // Explicit H5Premove also invokes *_del.
  ASSERT_GE(H5Premove(dapl_a.get(), "vds_prefix"), 0);
  ASSERT_GE(H5Premove(dapl_a.get(), "external file prefix"), 0);

  // Setting nullptr unsets prefix cleanly.
  ASSERT_GE(H5Pset_virtual_prefix(dapl_b.get(), nullptr), 0);
  ASSERT_GE(H5Pset_efile_prefix(dapl_b.get(), nullptr), 0);
  EXPECT_EQ(H5Pget_virtual_prefix(dapl_b.get(), nullptr, 0), 0);
  EXPECT_EQ(H5Pget_efile_prefix(dapl_b.get(), nullptr, 0), 0);

  // 6. Negative ID checks.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_efile_prefix(H5I_INVALID_HID, "bad"), 0);
    EXPECT_LT(H5Pset_efile_prefix(H5P_FILE_ACCESS_DEFAULT, "bad"), 0);
    EXPECT_LT(H5Pget_efile_prefix(H5I_INVALID_HID, nullptr, 0), 0);
    EXPECT_LT(H5Pset_virtual_prefix(H5I_INVALID_HID, "bad"), 0);
    EXPECT_LT(H5Pset_virtual_prefix(H5P_FILE_ACCESS_DEFAULT, "bad"), 0);
    EXPECT_LT(H5Pget_virtual_prefix(H5I_INVALID_HID, nullptr, 0), 0);
  }
}

TEST(H5PdaplTest, AppendFlushConfigurationAndValidation) {
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl.valid());

  // 1. Default plist returns empty callback and zeroed boundaries.
  std::array<hsize_t, 3> out_bounds = {99, 99, 99};
  H5D_append_cb_t out_cb = DummyAppendFlushCb;
  int dummy_ctx = 42;
  void* out_udata = &dummy_ctx;
  ASSERT_GE(H5Pget_append_flush(H5P_DATASET_ACCESS_DEFAULT, out_bounds.size(),
                                out_bounds.data(), &out_cb, &out_udata),
            0);
  EXPECT_THAT(out_bounds, ElementsAre(0U, 0U, 0U));
  EXPECT_EQ(out_cb, nullptr);
  EXPECT_EQ(out_udata, nullptr);

  // 2. Set boundary, callback, and user data, then read back (short & wide).
  constexpr std::array<hsize_t, 2> kBounds = {16, 0};
  ASSERT_GE(H5Pset_append_flush(dapl.get(), kBounds.size(), kBounds.data(),
                                DummyAppendFlushCb, &dummy_ctx),
            0);

  std::array<hsize_t, 1> short_bounds = {99};
  ASSERT_GE(H5Pget_append_flush(dapl.get(), short_bounds.size(),
                                short_bounds.data(), &out_cb, &out_udata),
            0);
  EXPECT_EQ(short_bounds[0], 16U);
  EXPECT_EQ(out_cb, DummyAppendFlushCb);
  EXPECT_EQ(out_udata, &dummy_ctx);

  out_bounds.fill(99);
  ASSERT_GE(H5Pget_append_flush(dapl.get(), out_bounds.size(),
                                out_bounds.data(), nullptr, nullptr),
            0);
  EXPECT_THAT(out_bounds, ElementsAre(16U, 0U, 0U));
  EXPECT_GE(H5Pget_append_flush(dapl.get(), 0, nullptr, nullptr, nullptr), 0);

  // 3. Invalid parameter checks.
  {
    ScopedErrorSilence silence;
    const hsize_t too_large[1] = {uint64_t{1} << 32};
    const hsize_t unlimited[1] = {H5S_UNLIMITED};
    EXPECT_LT(
        H5Pset_append_flush(dapl.get(), 0, kBounds.data(), DummyAppendFlushCb,
                            &dummy_ctx),
        0);
    EXPECT_LT(H5Pset_append_flush(dapl.get(), H5S_MAX_RANK + 1, kBounds.data(),
                                  DummyAppendFlushCb, &dummy_ctx),
              0);
    EXPECT_LT(
        H5Pset_append_flush(dapl.get(), 1, nullptr, DummyAppendFlushCb,
                            &dummy_ctx),
        0);
    EXPECT_LT(
        H5Pset_append_flush(dapl.get(), 1, kBounds.data(), nullptr, &dummy_ctx),
        0);
    EXPECT_LT(H5Pset_append_flush(dapl.get(), 1, too_large, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pset_append_flush(dapl.get(), 1, unlimited, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pset_append_flush(H5I_INVALID_HID, 1, kBounds.data(), nullptr,
                                  nullptr),
              0);
    EXPECT_LT(H5Pget_append_flush(H5I_INVALID_HID, 1, short_bounds.data(),
                                  &out_cb, &out_udata),
              0);
  }
}

TEST(H5PdaplTest, RemovedPropertiesTriggerInternalPropertyAccessErrors) {
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl.valid());

  // Removing DAPL properties leaves a valid DAPL instance that reaches the
  // internal H5P_set / H5P_get / H5P_peek / H5P_poke error branches.
  ASSERT_GE(H5Premove(dapl.get(), "rdcc_nslots"), 0);
  ASSERT_GE(H5Premove(dapl.get(), "rdcc_nbytes"), 0);
  ASSERT_GE(H5Premove(dapl.get(), "rdcc_w0"), 0);
  ASSERT_GE(H5Premove(dapl.get(), "vds_view"), 0);
  ASSERT_GE(H5Premove(dapl.get(), "vds_printf_gap"), 0);
  ASSERT_GE(H5Premove(dapl.get(), "vds_prefix"), 0);
  ASSERT_GE(H5Premove(dapl.get(), "append_flush"), 0);
  ASSERT_GE(H5Premove(dapl.get(), "external file prefix"), 0);
  ASSERT_GE(H5Premove(dapl.get(), "tree"), 0);

  ScopedErrorSilence silence;
  size_t nslots = 0;
  size_t nbytes = 0;
  double w0 = 0.0;
  H5D_vds_view_t view = H5D_VDS_ERROR;
  hsize_t gap = 0;
  bool use_tree = false;
  hsize_t bounds[1] = {1};

  EXPECT_LT(H5Pset_chunk_cache(dapl.get(), 1, 1, 0.5), 0);
  EXPECT_LT(H5Pget_chunk_cache(dapl.get(), &nslots, nullptr, nullptr), 0);
  EXPECT_LT(H5Pget_chunk_cache(dapl.get(), nullptr, &nbytes, nullptr), 0);
  EXPECT_LT(H5Pget_chunk_cache(dapl.get(), nullptr, nullptr, &w0), 0);
  EXPECT_LT(H5Pset_virtual_view(dapl.get(), H5D_VDS_FIRST_MISSING), 0);
  EXPECT_LT(H5Pget_virtual_view(dapl.get(), &view), 0);
  EXPECT_LT(H5Pset_virtual_printf_gap(dapl.get(), 1), 0);
  EXPECT_LT(H5Pget_virtual_printf_gap(dapl.get(), &gap), 0);
  EXPECT_LT(H5Pset_append_flush(dapl.get(), 1, bounds, nullptr, nullptr), 0);
  EXPECT_LT(H5Pget_append_flush(dapl.get(), 1, bounds, nullptr, nullptr), 0);
  EXPECT_LT(H5Pset_efile_prefix(dapl.get(), "x"), 0);
  EXPECT_LT(H5Pget_efile_prefix(dapl.get(), nullptr, 0), 0);
  EXPECT_LT(H5Pset_virtual_prefix(dapl.get(), "x"), 0);
  EXPECT_LT(H5Pget_virtual_prefix(dapl.get(), nullptr, 0), 0);
  EXPECT_LT(H5Pget_virtual_spatial_tree(dapl.get(), &use_tree), 0);
  EXPECT_LT(H5Pset_virtual_spatial_tree(dapl.get(), false), 0);

  // Also test partial rdcc default fallbacks before rdcc properties are removed.
  ScopedPlist dapl_rdcc(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl_rdcc.valid());
  ASSERT_GE(H5Pset_chunk_cache(dapl_rdcc.get(), 100,
                               H5D_CHUNK_CACHE_NBYTES_DEFAULT,
                               H5D_CHUNK_CACHE_W0_DEFAULT),
            0);
  EXPECT_GE(H5Pget_chunk_cache(dapl_rdcc.get(), &nslots, &nbytes, &w0), 0);
  EXPECT_EQ(nslots, 100U);
}

}  // namespace
}  // namespace hdf5
