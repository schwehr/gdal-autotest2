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
// Tests HDF5 High-Level Image APIs (third_party/hdf5/hl/src/H5IM.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5IM.c
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/test/test_image.c

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5_test_util.h"
#include "third_party/hdf5/hl/src/H5IMpublic.h"
#include "third_party/hdf5/hl/src/H5LTpublic.h"
#include "third_party/hdf5/hl/src/hdf5_hl.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::StrEq;

TEST(H5imTest, MakeReadAndInspect8BitImages) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  constexpr hsize_t kWidth = 4;
  constexpr hsize_t kHeight = 3;
  constexpr std::array<unsigned char, kWidth * kHeight> kPixels = {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
  };

  ASSERT_EQ(0,
            H5IMmake_image_8bit(file, "img8", kWidth, kHeight, kPixels.data()));
  EXPECT_EQ(1, H5IMis_image(file, "img8"));
  EXPECT_EQ(0, H5IMis_palette(file, "img8"));

  // Creating an 8-bit image with nullptr buffer creates an initialized dataset
  // without failing.
  ASSERT_EQ(0, H5IMmake_image_8bit(file, "img8_null", 2, 2, nullptr));
  EXPECT_EQ(1, H5IMis_image(file, "img8_null"));

  hsize_t width = 0;
  hsize_t height = 0;
  hsize_t planes = 0;
  hssize_t npals = -1;
  std::array<char, 32> interlace = {"PRESERVED"};
  ASSERT_EQ(0, H5IMget_image_info(file, "img8", &width, &height, &planes,
                                  interlace.data(), &npals));
  EXPECT_EQ(kWidth, width);
  EXPECT_EQ(kHeight, height);
  EXPECT_EQ(1u, planes);
  EXPECT_EQ(0, npals);
  // 8-bit images have no INTERLACE_MODE attribute, so `interlace` stays intact.
  EXPECT_THAT(interlace.data(), StrEq("PRESERVED"));

  std::vector<unsigned char> read_back(kWidth * kHeight, 0);
  ASSERT_EQ(0, H5IMread_image(file, "img8", read_back.data()));
  EXPECT_THAT(read_back, ElementsAreArray(kPixels));
}

TEST(H5imTest, MakeReadAndInspect24BitPixelAndPlaneImages) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  constexpr hsize_t kWidth = 3;
  constexpr hsize_t kHeight = 2;
  constexpr std::array<unsigned char, kWidth * kHeight * 3> kPixels = {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
  };

  ASSERT_EQ(0, H5IMmake_image_24bit(file, "img24_pixel", kWidth, kHeight,
                                    "INTERLACE_PIXEL", kPixels.data()));
  ASSERT_EQ(0, H5IMmake_image_24bit(file, "img24_plane", kWidth, kHeight,
                                    "INTERLACE_PLANE", kPixels.data()));
  EXPECT_EQ(1, H5IMis_image(file, "img24_pixel"));
  EXPECT_EQ(1, H5IMis_image(file, "img24_plane"));

  hsize_t width = 0;
  hsize_t height = 0;
  hsize_t planes = 0;
  hssize_t npals = -1;
  std::array<char, 32> interlace = {};

  ASSERT_EQ(0, H5IMget_image_info(file, "img24_pixel", &width, &height, &planes,
                                  interlace.data(), &npals));
  EXPECT_EQ(kWidth, width);
  EXPECT_EQ(kHeight, height);
  EXPECT_EQ(3u, planes);
  EXPECT_EQ(0, npals);
  EXPECT_THAT(interlace.data(), StrEq("INTERLACE_PIXEL"));

  interlace.fill('\0');
  ASSERT_EQ(0, H5IMget_image_info(file, "img24_plane", &width, &height, &planes,
                                  interlace.data(), &npals));
  EXPECT_EQ(kWidth, width);
  EXPECT_EQ(kHeight, height);
  EXPECT_EQ(3u, planes);
  EXPECT_EQ(0, npals);
  EXPECT_THAT(interlace.data(), StrEq("INTERLACE_PLANE"));

  std::vector<unsigned char> read_back(kPixels.size(), 0);
  ASSERT_EQ(0, H5IMread_image(file, "img24_pixel", read_back.data()));
  EXPECT_THAT(read_back, ElementsAreArray(kPixels));
}

TEST(H5imTest, PaletteCreateLinkInspectReadAndUnlink) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  // 1. Create an 8-bit image and two 3x3 RGB palettes.
  constexpr std::array<unsigned char, 4> kImg = {0, 1, 2, 3};
  ASSERT_EQ(0, H5IMmake_image_8bit(file, "img", 2, 2, kImg.data()));

  constexpr std::array<hsize_t, 2> kPalDims = {3, 3};
  constexpr std::array<unsigned char, 9> kPal1 = {
      255, 0, 0, 0, 255, 0, 0, 0, 255,
  };
  constexpr std::array<unsigned char, 9> kPal2 = {
      10, 20, 30, 40, 50, 60, 70, 80, 90,
  };

  ASSERT_EQ(0, H5IMmake_palette(file, "pal1", kPalDims.data(), kPal1.data()));
  // Calling H5IMmake_palette on an existing dataset succeeds as a no-op.
  ASSERT_EQ(0, H5IMmake_palette(file, "pal1", kPalDims.data(), kPal1.data()));
  ASSERT_EQ(0, H5IMmake_palette(file, "pal2", kPalDims.data(), kPal2.data()));

  EXPECT_EQ(1, H5IMis_palette(file, "pal1"));
  EXPECT_EQ(0, H5IMis_image(file, "pal1"));

  // 2. Before linking any palette, npals is 0 and get_palette_info/get_palette
  //    succeed as no-ops (pal_dims/pal_data remain untouched).
  hssize_t npals = -1;
  ASSERT_EQ(0, H5IMget_npalettes(file, "img", &npals));
  EXPECT_EQ(0, npals);
  std::array<hsize_t, 2> pal_dims_out = {999, 999};
  EXPECT_EQ(0, H5IMget_palette_info(file, "img", 0, pal_dims_out.data()));
  EXPECT_THAT(pal_dims_out, ElementsAre(999, 999));
  std::array<unsigned char, 9> pal_data_out = {77};
  EXPECT_EQ(0, H5IMget_palette(file, "img", 0, pal_data_out.data()));
  EXPECT_EQ(77, pal_data_out[0]);

  // 3. Link first palette (ok_pal == 0), then second palette (ok_pal > 0).
  ASSERT_EQ(0, H5IMlink_palette(file, "img", "pal1"));
  ASSERT_EQ(0, H5IMlink_palette(file, "img", "pal2"));

  ASSERT_EQ(0, H5IMget_npalettes(file, "img", &npals));
  EXPECT_EQ(2, npals);

  hsize_t width = 0;
  hsize_t height = 0;
  hsize_t planes = 0;
  hssize_t info_npals = 0;
  std::array<char, 32> interlace = {};
  ASSERT_EQ(0, H5IMget_image_info(file, "img", &width, &height, &planes,
                                  interlace.data(), &info_npals));
  EXPECT_EQ(2, info_npals);

  // 4. Query both linked palettes.
  pal_dims_out = {0, 0};
  ASSERT_EQ(0, H5IMget_palette_info(file, "img", 0, pal_dims_out.data()));
  EXPECT_THAT(pal_dims_out, ElementsAre(3, 3));
  pal_dims_out = {0, 0};
  ASSERT_EQ(0, H5IMget_palette_info(file, "img", 1, pal_dims_out.data()));
  EXPECT_THAT(pal_dims_out, ElementsAre(3, 3));

  pal_data_out.fill(0);
  ASSERT_EQ(0, H5IMget_palette(file, "img", 0, pal_data_out.data()));
  EXPECT_THAT(pal_data_out, ElementsAreArray(kPal1));
  pal_data_out.fill(0);
  ASSERT_EQ(0, H5IMget_palette(file, "img", 1, pal_data_out.data()));
  EXPECT_THAT(pal_data_out, ElementsAreArray(kPal2));

  // 5. Unlink deletes the PALETTE attribute; unlinking again fails because
  //    PALETTE attribute no longer exists (ok_pal == 0).
  ASSERT_EQ(0, H5IMunlink_palette(file, "img", "pal1"));
  ASSERT_EQ(0, H5IMget_npalettes(file, "img", &npals));
  EXPECT_EQ(0, npals);
  EXPECT_LT(H5IMunlink_palette(file, "img", "pal1"), 0);
}

TEST(H5imTest, ClassificationAndNonReferencePaletteAttributes) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);
  // Upstream H5IMget_image_info() leaks `sid` on rank > 3 and leaks both `did`
  // and `sid` on unrecognized INTERLACE_MODE; H5IMis_image()/H5IMis_palette()
  // `out:` labels leave `aid`/`atid` open. Sweep leaked IDs before `file`
  // closes.
  const absl::Cleanup sweep_leaked = [] {
    SweepOpenH5Ids(
        std::array{H5I_DATASPACE, H5I_DATATYPE, H5I_ATTR, H5I_DATASET});
  };

  // Plain dataset without CLASS attribute returns 0 for both predicates.
  constexpr hsize_t kDim1d = 4;
  constexpr std::array<int32_t, 4> kInts = {1, 2, 3, 4};
  ASSERT_EQ(0, H5LTmake_dataset_int(file, "plain", 1, &kDim1d, kInts.data()));
  EXPECT_EQ(0, H5IMis_image(file, "plain"));
  EXPECT_EQ(0, H5IMis_palette(file, "plain"));

  // Dataset with unrelated CLASS string also returns 0.
  ASSERT_EQ(
      0, H5LTmake_dataset_int(file, "other_class", 1, &kDim1d, kInts.data()));
  ASSERT_EQ(0, H5LTset_attribute_string(file, "other_class", "CLASS", "TABLE"));
  EXPECT_EQ(0, H5IMis_image(file, "other_class"));
  EXPECT_EQ(0, H5IMis_palette(file, "other_class"));

  // Image dataset with a non-reference "PALETTE" attribute exercises the
  // `aclass != H5T_REFERENCE` branches in info/npalettes/unlink cleanly.
  constexpr std::array<unsigned char, 4> kImg = {1, 2, 3, 4};
  ASSERT_EQ(0, H5IMmake_image_8bit(file, "bad_pal_attr", 2, 2, kImg.data()));
  constexpr int32_t kNonRefVal = 7;
  ASSERT_EQ(0, H5LTset_attribute_int(file, "bad_pal_attr", "PALETTE",
                                     &kNonRefVal, 1));
  constexpr std::array<hsize_t, 2> kPalDims = {1, 3};
  constexpr std::array<unsigned char, 3> kPal = {1, 2, 3};
  ASSERT_EQ(0, H5IMmake_palette(file, "pal_ok", kPalDims.data(), kPal.data()));

  hssize_t npals = -1;
  EXPECT_EQ(0, H5IMget_npalettes(file, "bad_pal_attr", &npals));
  EXPECT_EQ(0, npals);

  hsize_t w = 0;
  hsize_t h = 0;
  hsize_t p = 0;
  std::array<char, 32> interlace = {};
  npals = -1;
  EXPECT_EQ(0, H5IMget_image_info(file, "bad_pal_attr", &w, &h, &p,
                                  interlace.data(), &npals));
  EXPECT_EQ(0, npals);
  EXPECT_EQ(0, H5IMunlink_palette(file, "bad_pal_attr", "pal_ok"));

  // Dataset with rank > 3 rejects H5IMget_image_info cleanly.
  constexpr std::array<hsize_t, 4> kDims4d = {1, 1, 1, 1};
  ASSERT_EQ(
      0, H5LTmake_dataset_int(file, "dset4d", 4, kDims4d.data(), kInts.data()));
  EXPECT_LT(
      H5IMget_image_info(file, "dset4d", &w, &h, &p, interlace.data(), &npals),
      0);

  // CLASS attribute validation edge cases: non-string, H5T_STR_NULLPAD, and
  // zero-byte storage size (H5S_NULL).
  ASSERT_EQ(0,
            H5LTmake_dataset_int(file, "int_class", 1, &kDim1d, kInts.data()));
  ASSERT_EQ(0,
            H5LTset_attribute_int(file, "int_class", "CLASS", &kNonRefVal, 1));
  EXPECT_LT(H5IMis_image(file, "int_class"), 0);
  EXPECT_LT(H5IMis_palette(file, "int_class"), 0);

  ASSERT_EQ(
      0, H5LTmake_dataset_int(file, "nullpad_class", 1, &kDim1d, kInts.data()));
  {
    ScopedDataset dset(H5Dopen2(file, "nullpad_class", H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);
    ASSERT_EQ(0, WriteNullpadScalarStringAttr(dset, "CLASS", "IMAGE"));
  }
  EXPECT_LT(H5IMis_image(file, "nullpad_class"), 0);
  EXPECT_LT(H5IMis_palette(file, "nullpad_class"), 0);

  ASSERT_EQ(
      0, H5LTmake_dataset_int(file, "empty_class", 1, &kDim1d, kInts.data()));
  {
    ScopedDataset dset(H5Dopen2(file, "empty_class", H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);
    ScopedDatatype str_tid(H5Tcopy(H5T_C_S1));
    ASSERT_GE(str_tid.get(), 0);
    ASSERT_GE(H5Tset_size(str_tid, 6), 0);
    ScopedDataspace null_space(H5Screate(H5S_NULL));
    ASSERT_GE(null_space.get(), 0);
    ScopedAttribute attr(H5Acreate2(dset, "CLASS", str_tid, null_space,
                                    H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_GE(attr.get(), 0);
  }
  EXPECT_LT(H5IMis_image(file, "empty_class"), 0);
  EXPECT_LT(H5IMis_palette(file, "empty_class"), 0);

  // Image dataset with an unrecognized INTERLACE_MODE value fails cleanly.
  constexpr std::array<hsize_t, 3> kDims3d = {1, 2, 2};
  ASSERT_EQ(0, H5LTmake_dataset_int(file, "bad_interlace_img", 3,
                                    kDims3d.data(), kInts.data()));
  ASSERT_EQ(0, H5LTset_attribute_string(file, "bad_interlace_img",
                                        "INTERLACE_MODE", "BAD_MODE"));
  interlace.fill('\0');
  EXPECT_LT(H5IMget_image_info(file, "bad_interlace_img", &w, &h, &p,
                               interlace.data(), &npals),
            0);
}

TEST(H5imTest, ErrorPathsAndInvalidArguments) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  constexpr std::array<unsigned char, 4> kImg = {1, 2, 3, 4};
  ASSERT_EQ(0, H5IMmake_image_8bit(file, "img", 2, 2, kImg.data()));

  constexpr std::array<hsize_t, 2> kPalDims = {1, 3};
  constexpr std::array<unsigned char, 3> kPal = {10, 20, 30};
  ASSERT_EQ(0, H5IMmake_palette(file, "pal", kPalDims.data(), kPal.data()));

  ScopedErrorSilence silence;

  // make_image_8bit / make_image_24bit errors.
  EXPECT_LT(H5IMmake_image_8bit(file, nullptr, 2, 2, kImg.data()), 0);
  EXPECT_LT(H5IMmake_image_8bit(file, "img", 2, 2, kImg.data()), 0);
  EXPECT_LT(H5IMmake_image_8bit(H5I_INVALID_HID, "bad", 2, 2, kImg.data()), 0);

  EXPECT_LT(H5IMmake_image_24bit(file, "bad24", 2, 2, nullptr, kImg.data()), 0);
  EXPECT_LT(
      H5IMmake_image_24bit(file, nullptr, 2, 2, "INTERLACE_PIXEL", kImg.data()),
      0);
  EXPECT_LT(H5IMmake_image_24bit(file, "bad_mode", 2, 2, "INVALID_INTERLACE",
                                 kImg.data()),
            0);
  EXPECT_LT(
      H5IMmake_image_24bit(file, "img", 2, 2, "INTERLACE_PIXEL", kImg.data()),
      0);

  // get_image_info / read_image errors.
  hsize_t w = 0;
  hsize_t h = 0;
  hsize_t p = 0;
  hssize_t npals = 0;
  std::array<char, 32> interlace = {};
  std::array<unsigned char, 4> read_buf = {};
  EXPECT_LT(
      H5IMget_image_info(file, nullptr, &w, &h, &p, interlace.data(), &npals),
      0);
  EXPECT_LT(H5IMget_image_info(file, "img", &w, &h, &p, nullptr, &npals), 0);
  EXPECT_LT(
      H5IMget_image_info(file, "missing", &w, &h, &p, interlace.data(), &npals),
      0);

  EXPECT_LT(H5IMread_image(file, nullptr, read_buf.data()), 0);
  EXPECT_LT(H5IMread_image(file, "missing", read_buf.data()), 0);
  EXPECT_LT(H5IMread_image(file, "img", nullptr), 0);

  // make_palette / link / unlink / get_npalettes / get_palette* errors.
  EXPECT_LT(H5IMmake_palette(file, nullptr, kPalDims.data(), kPal.data()), 0);
  EXPECT_LT(H5IMmake_palette(H5I_INVALID_HID, "bad_pal", kPalDims.data(),
                             kPal.data()),
            0);
  EXPECT_LT(H5IMmake_palette(file, "bad_pal_dims", nullptr, kPal.data()), 0);

  EXPECT_LT(H5IMlink_palette(file, nullptr, "pal"), 0);
  EXPECT_LT(H5IMlink_palette(file, "img", nullptr), 0);
  EXPECT_LT(H5IMlink_palette(file, "missing", "pal"), 0);
  // Safe: "img" has no PALETTE attribute yet (ok_pal == 0), so no refbuf is
  // allocated when H5Rcreate fails on "missing_pal".
  EXPECT_LT(H5IMlink_palette(file, "img", "missing_pal"), 0);

  EXPECT_LT(H5IMunlink_palette(file, nullptr, "pal"), 0);
  EXPECT_LT(H5IMunlink_palette(file, "img", nullptr), 0);
  EXPECT_LT(H5IMunlink_palette(file, "img", "missing_pal"), 0);
  EXPECT_LT(H5IMunlink_palette(file, "missing", "pal"), 0);

  EXPECT_LT(H5IMget_npalettes(file, nullptr, &npals), 0);
  EXPECT_LT(H5IMget_npalettes(file, "missing", &npals), 0);

  std::array<hsize_t, 2> pal_dims_out = {};
  std::array<unsigned char, 3> pal_data_out = {};
  EXPECT_LT(H5IMget_palette_info(file, nullptr, 0, pal_dims_out.data()), 0);
  EXPECT_LT(H5IMget_palette_info(file, "missing", 0, pal_dims_out.data()), 0);

  EXPECT_LT(H5IMget_palette(file, nullptr, 0, pal_data_out.data()), 0);
  EXPECT_LT(H5IMget_palette(file, "img", 0, nullptr), 0);
  EXPECT_LT(H5IMget_palette(file, "missing", 0, pal_data_out.data()), 0);

  EXPECT_LT(H5IMis_image(file, nullptr), 0);
  EXPECT_LT(H5IMis_image(file, "missing"), 0);
  EXPECT_LT(H5IMis_palette(file, nullptr), 0);
  EXPECT_LT(H5IMis_palette(file, "missing"), 0);
}

// Pass-through VOL connector forwarding to H5VL_NATIVE with selective fault
// injection for internal error-handling paths in H5IM.c.
struct FaultVolState {
  int attr_create_calls = 0;
  int fail_attr_create_call = 0;
  int attr_open_calls = 0;
  int fail_attr_open_call = 0;
  int attr_get_type_calls = 0;
  int fail_attr_get_type_call = 0;
  int bad_attr_get_type_call = 0;
  int immutable_attr_get_type_call = 0;
  bool fail_attr_exists = false;
  bool fail_attr_delete = false;
  bool fail_attr_read = false;
  bool close_mem_type_on_read = false;
  bool fail_attr_write = false;
  bool close_mem_type_on_write = false;
  bool fail_attr_get_space = false;
  bool bad_attr_get_space = false;
  bool dead_attr_get_space = false;
  bool fail_dset_create = false;
  bool fail_dset_get_space = false;
  bool bad_dset_get_space = false;
  int dset_close_calls = 0;
  int fail_dset_close_call = 0;
  int in_attr_specific = 0;
  int injected_faults = 0;

  void Reset() { *this = FaultVolState{}; }
};

FaultVolState g_fault_vol_state;

void* FaultFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                      hid_t fapl_id, hid_t dxpl_id, void** req) {
  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  if (!under_fapl.valid() || H5Pset_vol(under_fapl, H5VL_NATIVE, nullptr) < 0) {
    return nullptr;
  }
  return H5VLfile_create(name, flags, fcpl_id, under_fapl, dxpl_id, req);
}

herr_t FaultFileGet(void* file, H5VL_file_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  return H5VLfile_get(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultFileSpecific(void* obj, H5VL_file_specific_args_t* args,
                         hid_t dxpl_id, void** req) {
  return H5VLfile_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultFileOptional(void* file, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLfile_optional(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultFileClose(void* file, hid_t dxpl_id, void** req) {
  return H5VLfile_close(file, H5VL_NATIVE, dxpl_id, req);
}

void* FaultAttrCreate(void* obj, const H5VL_loc_params_t* loc_params,
                      const char* attr_name, hid_t type_id, hid_t space_id,
                      hid_t acpl_id, hid_t aapl_id, hid_t dxpl_id, void** req) {
  ++g_fault_vol_state.attr_create_calls;
  if (g_fault_vol_state.fail_attr_create_call ==
      g_fault_vol_state.attr_create_calls) {
    ++g_fault_vol_state.injected_faults;
    return nullptr;
  }
  return H5VLattr_create(obj, loc_params, H5VL_NATIVE, attr_name, type_id,
                         space_id, acpl_id, aapl_id, dxpl_id, req);
}

void* FaultAttrOpen(void* obj, const H5VL_loc_params_t* loc_params,
                    const char* attr_name, hid_t aapl_id, hid_t dxpl_id,
                    void** req) {
  ++g_fault_vol_state.attr_open_calls;
  if (g_fault_vol_state.fail_attr_open_call ==
      g_fault_vol_state.attr_open_calls) {
    ++g_fault_vol_state.injected_faults;
    return nullptr;
  }
  return H5VLattr_open(obj, loc_params, H5VL_NATIVE, attr_name, aapl_id,
                       dxpl_id, req);
}

herr_t FaultAttrRead(void* attr, hid_t mem_type_id, void* buf, hid_t dxpl_id,
                     void** req) {
  if (g_fault_vol_state.fail_attr_read) {
    ++g_fault_vol_state.injected_faults;
    return -1;
  }
  const herr_t status =
      H5VLattr_read(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
  if (status >= 0 && g_fault_vol_state.close_mem_type_on_read) {
    ++g_fault_vol_state.injected_faults;
    H5Tclose(mem_type_id);
  }
  return status;
}

herr_t FaultAttrWrite(void* attr, hid_t mem_type_id, const void* buf,
                      hid_t dxpl_id, void** req) {
  if (g_fault_vol_state.fail_attr_write) {
    ++g_fault_vol_state.injected_faults;
    return -1;
  }
  const herr_t status =
      H5VLattr_write(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
  if (status >= 0 && g_fault_vol_state.close_mem_type_on_write) {
    ++g_fault_vol_state.injected_faults;
    H5Tclose(mem_type_id);
  }
  return status;
}

herr_t FaultAttrGet(void* obj, H5VL_attr_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  if (args->op_type == H5VL_ATTR_GET_TYPE) {
    ++g_fault_vol_state.attr_get_type_calls;
    if (g_fault_vol_state.fail_attr_get_type_call ==
        g_fault_vol_state.attr_get_type_calls) {
      ++g_fault_vol_state.injected_faults;
      return -1;
    }
    if (g_fault_vol_state.bad_attr_get_type_call ==
        g_fault_vol_state.attr_get_type_calls) {
      ++g_fault_vol_state.injected_faults;
      // 0 is never a valid object ID.
      args->args.get_type.type_id = H5P_DEFAULT;
      return 0;
    }
    if (g_fault_vol_state.immutable_attr_get_type_call ==
        g_fault_vol_state.attr_get_type_calls) {
      ++g_fault_vol_state.injected_faults;
      args->args.get_type.type_id = H5T_NATIVE_INT32;
      return 0;
    }
  } else if (args->op_type == H5VL_ATTR_GET_SPACE) {
    if (g_fault_vol_state.fail_attr_get_space) {
      ++g_fault_vol_state.injected_faults;
      return -1;
    }
    if (g_fault_vol_state.bad_attr_get_space) {
      ++g_fault_vol_state.injected_faults;
      // 0 is never a valid object ID.
      args->args.get_space.space_id = H5S_ALL;
      return 0;
    }
    if (g_fault_vol_state.dead_attr_get_space) {
      ++g_fault_vol_state.injected_faults;
      const hid_t dead = H5Screate(H5S_SCALAR);
      if (dead >= 0) {
        H5Sclose(dead);
      }
      args->args.get_space.space_id = dead;
      return 0;
    }
  }
  return H5VLattr_get(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultAttrSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_attr_specific_args_t* args, hid_t dxpl_id,
                         void** req) {
  if (args->op_type == H5VL_ATTR_EXISTS && g_fault_vol_state.fail_attr_exists) {
    ++g_fault_vol_state.injected_faults;
    return -1;
  }
  if (args->op_type == H5VL_ATTR_DELETE && g_fault_vol_state.fail_attr_delete) {
    ++g_fault_vol_state.injected_faults;
    return -1;
  }
  ++g_fault_vol_state.in_attr_specific;
  const herr_t status =
      H5VLattr_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
  --g_fault_vol_state.in_attr_specific;
  return status;
}

herr_t FaultAttrOptional(void* obj, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLattr_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultAttrClose(void* attr, hid_t dxpl_id, void** req) {
  return H5VLattr_close(attr, H5VL_NATIVE, dxpl_id, req);
}

void* FaultDatasetCreate(void* obj, const H5VL_loc_params_t* loc_params,
                         const char* name, hid_t lcpl_id, hid_t type_id,
                         hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                         hid_t dxpl_id, void** req) {
  if (g_fault_vol_state.fail_dset_create) {
    ++g_fault_vol_state.injected_faults;
    return nullptr;
  }
  return H5VLdataset_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id,
                            type_id, space_id, dcpl_id, dapl_id, dxpl_id, req);
}

void* FaultDatasetOpen(void* obj, const H5VL_loc_params_t* loc_params,
                       const char* name, hid_t dapl_id, hid_t dxpl_id,
                       void** req) {
  return H5VLdataset_open(obj, loc_params, H5VL_NATIVE, name, dapl_id, dxpl_id,
                          req);
}

herr_t FaultDatasetRead(size_t count, void* dset[], hid_t mem_type_id[],
                        hid_t mem_space_id[], hid_t file_space_id[],
                        hid_t dxpl_id, void* buf[], void** req) {
  return H5VLdataset_read(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                          file_space_id, dxpl_id, buf, req);
}

herr_t FaultDatasetWrite(size_t count, void* dset[], hid_t mem_type_id[],
                         hid_t mem_space_id[], hid_t file_space_id[],
                         hid_t dxpl_id, const void* buf[], void** req) {
  return H5VLdataset_write(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                           file_space_id, dxpl_id, buf, req);
}

herr_t FaultDatasetGet(void* dset, H5VL_dataset_get_args_t* args, hid_t dxpl_id,
                       void** req) {
  if (args->op_type == H5VL_DATASET_GET_SPACE) {
    if (g_fault_vol_state.fail_dset_get_space) {
      ++g_fault_vol_state.injected_faults;
      return -1;
    }
    if (g_fault_vol_state.bad_dset_get_space) {
      ++g_fault_vol_state.injected_faults;
      // 0 is never a valid object ID.
      args->args.get_space.space_id = H5S_ALL;
      return 0;
    }
  }
  return H5VLdataset_get(dset, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultDatasetSpecific(void* obj, H5VL_dataset_specific_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultDatasetOptional(void* obj, H5VL_optional_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultDatasetClose(void* dset, hid_t dxpl_id, void** req) {
  const herr_t status = H5VLdataset_close(dset, H5VL_NATIVE, dxpl_id, req);
  if (g_fault_vol_state.in_attr_specific == 0) {
    ++g_fault_vol_state.dset_close_calls;
    if (g_fault_vol_state.fail_dset_close_call ==
        g_fault_vol_state.dset_close_calls) {
      ++g_fault_vol_state.injected_faults;
      return -1;
    }
  }
  return status;
}

// H5LTfind_dataset -> H5Literate2 -> H5G_iterate wraps a temporary H5I_GROUP
// on the active VOL connector and releases it via group_cls.close.
herr_t FaultGroupClose(void* grp, hid_t dxpl_id, void** req) {
  return H5VLgroup_close(grp, H5VL_NATIVE, dxpl_id, req);
}

herr_t FaultLinkGet(void* obj, const H5VL_loc_params_t* loc_params,
                    H5VL_link_get_args_t* args, hid_t dxpl_id, void** req) {
  return H5VLlink_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultLinkSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_link_specific_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLlink_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

void* FaultObjectOpen(void* obj, const H5VL_loc_params_t* loc_params,
                      H5I_type_t* opened_type, hid_t dxpl_id, void** req) {
  return H5VLobject_open(obj, loc_params, H5VL_NATIVE, opened_type, dxpl_id,
                         req);
}

herr_t FaultObjectGet(void* obj, const H5VL_loc_params_t* loc_params,
                      H5VL_object_get_args_t* args, hid_t dxpl_id, void** req) {
  return H5VLobject_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultObjectSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                           H5VL_object_specific_args_t* args, hid_t dxpl_id,
                           void** req) {
  return H5VLobject_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultObjectOptional(void* obj, const H5VL_loc_params_t* loc_params,
                           H5VL_optional_args_t* args, hid_t dxpl_id,
                           void** req) {
  return H5VLobject_optional(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

extern const H5VL_class_t kFaultVolClass;

herr_t FaultIntrospectGetConnCls(void* obj, H5VL_get_conn_lvl_t lvl,
                                 const H5VL_class_t** conn_cls) {
  if (lvl == H5VL_GET_CONN_LVL_CURR) {
    *conn_cls = &kFaultVolClass;
    return 0;
  }
  return H5VLintrospect_get_conn_cls(obj, H5VL_NATIVE, lvl, conn_cls);
}

herr_t FaultIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  return H5VLintrospect_get_cap_flags(nullptr, H5VL_NATIVE, cap_flags);
}

herr_t FaultIntrospectOptQuery(void* obj, H5VL_subclass_t cls, int opt_type,
                               uint64_t* flags) {
  return H5VLintrospect_opt_query(obj, H5VL_NATIVE, cls, opt_type, flags);
}

const H5VL_class_t kFaultVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(51004),
    .name = "h5im_fault_vol",
    .conn_version = 1,
    .cap_flags = 0,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls = {},
    .attr_cls =
        {
            .create = FaultAttrCreate,
            .open = FaultAttrOpen,
            .read = FaultAttrRead,
            .write = FaultAttrWrite,
            .get = FaultAttrGet,
            .specific = FaultAttrSpecific,
            .optional = FaultAttrOptional,
            .close = FaultAttrClose,
        },
    .dataset_cls =
        {
            .create = FaultDatasetCreate,
            .open = FaultDatasetOpen,
            .read = FaultDatasetRead,
            .write = FaultDatasetWrite,
            .get = FaultDatasetGet,
            .specific = FaultDatasetSpecific,
            .optional = FaultDatasetOptional,
            .close = FaultDatasetClose,
        },
    .datatype_cls = {},
    .file_cls =
        {
            .create = FaultFileCreate,
            .open = nullptr,
            .get = FaultFileGet,
            .specific = FaultFileSpecific,
            .optional = FaultFileOptional,
            .close = FaultFileClose,
        },
    .group_cls =
        {
            .close = FaultGroupClose,
        },
    .link_cls =
        {
            .create = nullptr,
            .copy = nullptr,
            .move = nullptr,
            .get = FaultLinkGet,
            .specific = FaultLinkSpecific,
            .optional = nullptr,
        },
    .object_cls =
        {
            .open = FaultObjectOpen,
            .copy = nullptr,
            .get = FaultObjectGet,
            .specific = FaultObjectSpecific,
            .optional = FaultObjectOptional,
        },
    .introspect_cls =
        {
            .get_conn_cls = FaultIntrospectGetConnCls,
            .get_cap_flags = FaultIntrospectGetCapFlags,
            .opt_query = FaultIntrospectOptQuery,
        },
    .request_cls = {},
    .blob_cls = {},
    .token_cls = {},
    .optional = nullptr,
};

template <typename FieldT, typename Fn>
void ExpectImFault(FieldT FaultVolState::* knob, FieldT value, Fn&& op) {
  g_fault_vol_state.Reset();
  g_fault_vol_state.*knob = value;
  EXPECT_LT(op(), 0);
  EXPECT_EQ(1, g_fault_vol_state.injected_faults);
}

TEST(H5imTest, PropagatesInternalVolAttributeAndDatasetFailures) {
  g_fault_vol_state.Reset();
  const hid_t vol_id = H5VLregister_connector(&kFaultVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  const absl::Cleanup cleanup_vol = [vol_id] {
    g_fault_vol_state.Reset();
    SweepOpenH5Ids();
    H5VLunregister_connector(vol_id);
  };

  ScopedFile file = CreateCoreVolFile(vol_id, "h5im_fault_vol.h5");
  ASSERT_GE(file.get(), 0);

  constexpr std::array<unsigned char, 12> kPixels = {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
  };
  constexpr std::array<hsize_t, 2> kPalDims = {1, 3};
  constexpr std::array<unsigned char, 3> kPal = {10, 20, 30};

  ASSERT_EQ(0, H5IMmake_image_24bit(file, "img24_base", 2, 2, "INTERLACE_PIXEL",
                                    kPixels.data()));
  ASSERT_EQ(0,
            H5IMmake_palette(file, "pal_base", kPalDims.data(), kPal.data()));
  ASSERT_EQ(0, H5IMlink_palette(file, "img24_base", "pal_base"));
  ASSERT_EQ(0,
            H5IMmake_image_8bit(file, "img8_unlinked", 2, 2, kPixels.data()));
  ASSERT_EQ(0,
            H5IMmake_image_8bit(file, "img8_write_fail", 2, 2, kPixels.data()));
  ASSERT_EQ(
      0, H5IMmake_image_8bit(file, "img8_tclose_fail", 2, 2, kPixels.data()));
  ASSERT_EQ(
      0, H5IMmake_image_8bit(file, "img8_link_dclose", 2, 2, kPixels.data()));
  ASSERT_EQ(
      0, H5IMmake_image_8bit(file, "img8_unlink_dclose", 2, 2, kPixels.data()));
  ASSERT_EQ(0, H5IMlink_palette(file, "img8_unlink_dclose", "pal_base"));

  ScopedErrorSilence silence;

  // 1. Dataset and attribute creation failures across make_image_8bit,
  //    make_image_24bit, make_palette, and link_palette (ok_pal == 0).
  ExpectImFault(&FaultVolState::fail_dset_create, true, [&] {
    return H5IMmake_image_8bit(file, "fail_dset8", 2, 2, kPixels.data());
  });
  ExpectImFault(&FaultVolState::fail_dset_create, true, [&] {
    return H5IMmake_image_24bit(file, "fail_dset24", 2, 2, "INTERLACE_PIXEL",
                                kPixels.data());
  });
  ExpectImFault(&FaultVolState::fail_dset_create, true, [&] {
    return H5IMmake_palette(file, "fail_dset_pal", kPalDims.data(),
                            kPal.data());
  });

  for (int call = 1; call <= 3; ++call) {
    const std::string name = "fail8_" + std::to_string(call);
    ExpectImFault(&FaultVolState::fail_attr_create_call, call, [&] {
      return H5IMmake_image_8bit(file, name.c_str(), 2, 2, kPixels.data());
    });
    EXPECT_EQ(call, g_fault_vol_state.attr_create_calls);
  }
  for (int call = 1; call <= 4; ++call) {
    const std::string name = "fail24_" + std::to_string(call);
    ExpectImFault(&FaultVolState::fail_attr_create_call, call, [&] {
      return H5IMmake_image_24bit(file, name.c_str(), 2, 2, "INTERLACE_PIXEL",
                                  kPixels.data());
    });
    EXPECT_EQ(call, g_fault_vol_state.attr_create_calls);
  }
  for (int call = 1; call <= 2; ++call) {
    const std::string name = "fail_pal_" + std::to_string(call);
    ExpectImFault(&FaultVolState::fail_attr_create_call, call, [&] {
      return H5IMmake_palette(file, name.c_str(), kPalDims.data(), kPal.data());
    });
    EXPECT_EQ(call, g_fault_vol_state.attr_create_calls);
  }

  ExpectImFault(&FaultVolState::fail_attr_create_call, 1, [&] {
    return H5IMlink_palette(file, "img8_unlinked", "pal_base");
  });

  // 2. H5Aexists failures across all callers.
  hsize_t w = 0;
  hsize_t h = 0;
  hsize_t p = 0;
  hssize_t npals = 0;
  std::array<char, 32> interlace = {};
  ExpectImFault(&FaultVolState::fail_attr_exists, true, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::fail_attr_exists, true, [&] {
    return H5IMlink_palette(file, "img8_unlinked", "pal_base");
  });
  ExpectImFault(&FaultVolState::fail_attr_exists, true, [&] {
    return H5IMunlink_palette(file, "img24_base", "pal_base");
  });
  ExpectImFault(&FaultVolState::fail_attr_exists, true,
                [&] { return H5IMis_image(file, "img24_base"); });
  ExpectImFault(&FaultVolState::fail_attr_exists, true,
                [&] { return H5IMis_palette(file, "pal_base"); });

  // 3. H5IMget_image_info failures on attribute/dataspace inspection.
  ExpectImFault(&FaultVolState::fail_attr_open_call, 1, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::fail_attr_get_type_call, 1, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::fail_attr_read, true, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });

  ExpectImFault(&FaultVolState::close_mem_type_on_read, true, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::close_mem_type_on_read, true,
                [&] { return H5IMis_image(file, "img24_base"); });
  ExpectImFault(&FaultVolState::close_mem_type_on_read, true,
                [&] { return H5IMis_palette(file, "pal_base"); });

  ExpectImFault(&FaultVolState::fail_dset_get_space, true, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::bad_dset_get_space, true, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });

  ExpectImFault(&FaultVolState::fail_attr_open_call, 2, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::fail_attr_get_type_call, 2, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::bad_attr_get_type_call, 2, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::immutable_attr_get_type_call, 2, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::fail_attr_get_space, true, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::dead_attr_get_space, true, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });

  // 4. H5IMlink_palette (ok_pal == 0) write/close failures on dedicated images,
  //    plus PALETTE attribute inspection failures before refbuf malloc.
  ExpectImFault(&FaultVolState::fail_attr_write, true, [&] {
    return H5IMlink_palette(file, "img8_write_fail", "pal_base");
  });
  ExpectImFault(&FaultVolState::close_mem_type_on_write, true, [&] {
    return H5IMlink_palette(file, "img8_tclose_fail", "pal_base");
  });

  std::array<hsize_t, 2> pal_dims_out = {};
  std::array<unsigned char, 3> pal_data_out = {};

  ExpectImFault(&FaultVolState::fail_attr_open_call, 1, [&] {
    return H5IMlink_palette(file, "img24_base", "pal_base");
  });
  ExpectImFault(&FaultVolState::fail_attr_open_call, 1, [&] {
    return H5IMunlink_palette(file, "img24_base", "pal_base");
  });
  ExpectImFault(&FaultVolState::fail_attr_open_call, 1,
                [&] { return H5IMget_npalettes(file, "img24_base", &npals); });
  ExpectImFault(&FaultVolState::fail_attr_open_call, 1, [&] {
    return H5IMget_palette_info(file, "img24_base", 0, pal_dims_out.data());
  });
  ExpectImFault(&FaultVolState::fail_attr_open_call, 1, [&] {
    return H5IMget_palette(file, "img24_base", 0, pal_data_out.data());
  });
  ExpectImFault(&FaultVolState::fail_attr_open_call, 1,
                [&] { return H5IMis_image(file, "img24_base"); });
  ExpectImFault(&FaultVolState::fail_attr_open_call, 1,
                [&] { return H5IMis_palette(file, "pal_base"); });

  ExpectImFault(&FaultVolState::fail_attr_get_type_call, 1, [&] {
    return H5IMlink_palette(file, "img24_base", "pal_base");
  });
  ExpectImFault(&FaultVolState::fail_attr_get_type_call, 1, [&] {
    return H5IMunlink_palette(file, "img24_base", "pal_base");
  });
  ExpectImFault(&FaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5IMget_npalettes(file, "img24_base", &npals); });
  ExpectImFault(&FaultVolState::fail_attr_get_type_call, 1, [&] {
    return H5IMget_palette_info(file, "img24_base", 0, pal_dims_out.data());
  });
  ExpectImFault(&FaultVolState::fail_attr_get_type_call, 1, [&] {
    return H5IMget_palette(file, "img24_base", 0, pal_data_out.data());
  });
  ExpectImFault(&FaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5IMis_image(file, "img24_base"); });
  ExpectImFault(&FaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5IMis_palette(file, "pal_base"); });

  ExpectImFault(&FaultVolState::bad_attr_get_type_call, 1, [&] {
    return H5IMlink_palette(file, "img24_base", "pal_base");
  });
  ExpectImFault(&FaultVolState::bad_attr_get_type_call, 1, [&] {
    return H5IMunlink_palette(file, "img24_base", "pal_base");
  });
  ExpectImFault(&FaultVolState::bad_attr_get_type_call, 1,
                [&] { return H5IMget_npalettes(file, "img24_base", &npals); });
  ExpectImFault(&FaultVolState::bad_attr_get_type_call, 1, [&] {
    return H5IMget_palette_info(file, "img24_base", 0, pal_dims_out.data());
  });
  ExpectImFault(&FaultVolState::bad_attr_get_type_call, 1, [&] {
    return H5IMget_palette(file, "img24_base", 0, pal_data_out.data());
  });

  // H5T_NATIVE_INT32 triggers non-reference + immutable close failure safely
  // in get_npalettes and unlink_palette without deleting PALETTE.
  ExpectImFault(&FaultVolState::immutable_attr_get_type_call, 1,
                [&] { return H5IMget_npalettes(file, "img24_base", &npals); });
  ExpectImFault(&FaultVolState::immutable_attr_get_type_call, 1, [&] {
    return H5IMunlink_palette(file, "img24_base", "pal_base");
  });

  ExpectImFault(&FaultVolState::fail_attr_get_space, true, [&] {
    return H5IMlink_palette(file, "img24_base", "pal_base");
  });
  ExpectImFault(&FaultVolState::fail_attr_get_space, true,
                [&] { return H5IMget_npalettes(file, "img24_base", &npals); });
  ExpectImFault(&FaultVolState::fail_attr_get_space, true, [&] {
    return H5IMget_palette_info(file, "img24_base", 0, pal_dims_out.data());
  });
  ExpectImFault(&FaultVolState::fail_attr_get_space, true, [&] {
    return H5IMget_palette(file, "img24_base", 0, pal_data_out.data());
  });

  ExpectImFault(&FaultVolState::dead_attr_get_space, true,
                [&] { return H5IMget_npalettes(file, "img24_base", &npals); });
  ExpectImFault(&FaultVolState::fail_attr_delete, true, [&] {
    return H5IMunlink_palette(file, "img24_base", "pal_base");
  });

  // 5. H5Dclose failures across all H5IM functions (underlying native handle
  //    is closed cleanly first to avoid leaks; call=2 skips pal_id so refbuf
  //    is freed before failing the final H5Dclose(did)).
  std::vector<unsigned char> img_buf(kPixels.size(), 0);
  ExpectImFault(&FaultVolState::fail_dset_close_call, 1, [&] {
    return H5IMget_image_info(file, "img24_base", &w, &h, &p, interlace.data(),
                              &npals);
  });
  ExpectImFault(&FaultVolState::fail_dset_close_call, 1, [&] {
    return H5IMread_image(file, "img24_base", img_buf.data());
  });
  ExpectImFault(&FaultVolState::fail_dset_close_call, 1, [&] {
    return H5IMlink_palette(file, "img8_link_dclose", "pal_base");
  });
  ExpectImFault(&FaultVolState::fail_dset_close_call, 1,
                [&] { return H5IMget_npalettes(file, "img24_base", &npals); });
  ExpectImFault(&FaultVolState::fail_dset_close_call, 2, [&] {
    return H5IMget_palette_info(file, "img24_base", 0, pal_dims_out.data());
  });
  ExpectImFault(&FaultVolState::fail_dset_close_call, 2, [&] {
    return H5IMget_palette(file, "img24_base", 0, pal_data_out.data());
  });
  ExpectImFault(&FaultVolState::fail_dset_close_call, 1,
                [&] { return H5IMis_image(file, "img24_base"); });
  ExpectImFault(&FaultVolState::fail_dset_close_call, 1,
                [&] { return H5IMis_palette(file, "pal_base"); });
  ExpectImFault(&FaultVolState::fail_dset_close_call, 1, [&] {
    return H5IMunlink_palette(file, "img8_unlink_dclose", "pal_base");
  });

  g_fault_vol_state.Reset();
  // Upstream `out:` handlers in H5IM.c (H5IMget_npalettes, H5IMunlink_palette,
  // H5IMis_image, H5IMis_palette, and H5IMget_palette_info/H5IMget_palette
  // `pal_id`) and H5LT.c leave opened attribute/type/space/dataset handles
  // unclosed, and H5IMget_palette leaks `H5Dget_type(pal_id)` on success;
  // sweep those leaked handles and verify only `file` remains open.
  SweepOpenH5Ids(
      std::array{H5I_DATASPACE, H5I_DATATYPE, H5I_ATTR, H5I_DATASET});
  EXPECT_EQ(1, H5Fget_obj_count(file, H5F_OBJ_ALL | H5F_OBJ_LOCAL));
}

// TODO(schwehr): Remaining uncovered lines in H5IM.c (~8%) are defensive
// HDF5/malloc error checks whose trigger conditions either leak un-freed
// buffers or require sub-allocation failures inside HDF5: `malloc` OOM checks
// for `refbuf`/`attr_data`, `H5Screate`/`H5Tcopy`/`H5Sget_simple_extent_*`
// error returns, `H5Dopen2` failure inside H5IMget_palette_info/H5IMget_palette
// after `H5Rdereference2`, and secondary `out:` close failures.

}  // namespace
}  // namespace hdf5
