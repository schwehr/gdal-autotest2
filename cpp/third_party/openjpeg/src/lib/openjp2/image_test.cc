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
// Tests internal and public OpenJPEG image lifecycle and header utilities
// (src/lib/openjp2/image.c).
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/image.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/image.h
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/openjpeg.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/opj_internal.h"
#include "third_party/openjpeg2/google/util/test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::Each;
using ::testing::ElementsAreArray;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(opj_image_t) > 0);
static_assert(sizeof(opj_image_comp_t) > 0);
static_assert(sizeof(opj_image_cmptparm_t) > 0);

constexpr std::array<OPJ_BYTE, 8> kSampleIccProfile = {
    0x00, 0x00, 0x00, 0x08, 'A', 'C', 'S', 'P',
};

// Builds a zero-initialized opj_image_cmptparm_t without touching the
// deprecated `bpp` field.
constexpr opj_image_cmptparm_t MakeCmptParam(OPJ_UINT32 dx, OPJ_UINT32 dy,
                                             OPJ_UINT32 w, OPJ_UINT32 h,
                                             OPJ_UINT32 x0, OPJ_UINT32 y0,
                                             OPJ_UINT32 prec, OPJ_UINT32 sgnd) {
  opj_image_cmptparm_t param{};
  param.dx = dx;
  param.dy = dy;
  param.w = w;
  param.h = h;
  param.x0 = x0;
  param.y0 = y0;
  param.prec = prec;
  param.sgnd = sgnd;
  return param;
}

// Allocates `image->icc_profile_buf` with opj_malloc() so opj_image_destroy()
// frees it cleanly via opj_free(). Note: test_util.h::CloneTestImage uses
// std::malloc() for the same field, which assumes opj_free == std::free
// (verified in opj_malloc.c and alloc_fault_injector.cc).
void PopulateIccProfile(opj_image_t* image, std::span<const OPJ_BYTE> profile) {
  ASSERT_NE(nullptr, image);
  ASSERT_FALSE(profile.empty());
  ASSERT_EQ(nullptr, image->icc_profile_buf);
  auto* buf = static_cast<OPJ_BYTE*>(opj_malloc(profile.size()));
  ASSERT_NE(nullptr, buf);
  std::memcpy(buf, profile.data(), profile.size());
  image->icc_profile_buf = buf;
  image->icc_profile_len = static_cast<OPJ_UINT32>(profile.size());
}

void ExpectComponentMatchesParam(const opj_image_cmptparm_t& expected,
                                 const opj_image_comp_t& actual) {
  EXPECT_EQ(expected.dx, actual.dx);
  EXPECT_EQ(expected.dy, actual.dy);
  EXPECT_EQ(expected.w, actual.w);
  EXPECT_EQ(expected.h, actual.h);
  EXPECT_EQ(expected.x0, actual.x0);
  EXPECT_EQ(expected.y0, actual.y0);
  EXPECT_EQ(expected.prec, actual.prec);
  EXPECT_EQ(expected.sgnd, actual.sgnd);
  EXPECT_EQ(0u, actual.resno_decoded);
  EXPECT_EQ(0u, actual.factor);
  EXPECT_EQ(0u, actual.alpha);
}

void ExpectComponentHeaderEq(const opj_image_comp_t& expected,
                             const opj_image_comp_t& actual) {
  EXPECT_EQ(expected.dx, actual.dx);
  EXPECT_EQ(expected.dy, actual.dy);
  EXPECT_EQ(expected.w, actual.w);
  EXPECT_EQ(expected.h, actual.h);
  EXPECT_EQ(expected.x0, actual.x0);
  EXPECT_EQ(expected.y0, actual.y0);
  EXPECT_EQ(expected.prec, actual.prec);
  EXPECT_EQ(expected.sgnd, actual.sgnd);
  EXPECT_EQ(expected.resno_decoded, actual.resno_decoded);
  EXPECT_EQ(expected.factor, actual.factor);
  EXPECT_EQ(expected.alpha, actual.alpha);
}

void VerifyHeaderCopyIntoDest(opj_image_t* dest, bool with_icc_profile) {
  std::array<opj_image_cmptparm_t, 2> src_params = {
      MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/8, /*h=*/6, /*x0=*/4, /*y0=*/8,
                    /*prec=*/12, /*sgnd=*/0),
      MakeCmptParam(/*dx=*/2, /*dy=*/2, /*w=*/4, /*h=*/3, /*x0=*/2, /*y0=*/4,
                    /*prec=*/10, /*sgnd=*/1),
  };
  OpjImagePtr src(opj_image_create(static_cast<OPJ_UINT32>(src_params.size()),
                                   src_params.data(), OPJ_CLRSPC_SYCC));
  ASSERT_NE(nullptr, src);
  src->x0 = 4;
  src->y0 = 8;
  src->x1 = 12;
  src->y1 = 14;
  src->comps[0].resno_decoded = 3;
  src->comps[0].factor = 1;
  src->comps[0].alpha = 0;
  src->comps[1].resno_decoded = 2;
  src->comps[1].factor = 2;
  src->comps[1].alpha = 1;
  src->comps[0].data[0] = 1234;
  src->comps[1].data[11] = -321;

  if (with_icc_profile) {
    ASSERT_NO_FATAL_FAILURE(PopulateIccProfile(src.get(), kSampleIccProfile));
  }

  opj_copy_image_header(src.get(), dest);

  EXPECT_EQ(src->x0, dest->x0);
  EXPECT_EQ(src->y0, dest->y0);
  EXPECT_EQ(src->x1, dest->x1);
  EXPECT_EQ(src->y1, dest->y1);
  EXPECT_EQ(src->numcomps, dest->numcomps);
  EXPECT_EQ(src->color_space, dest->color_space);
  ASSERT_NE(nullptr, dest->comps);
  EXPECT_NE(src->comps, dest->comps);

  for (OPJ_UINT32 c = 0; c < src->numcomps; ++c) {
    ExpectComponentHeaderEq(src->comps[c], dest->comps[c]);
    // opj_copy_image_header() copies component headers only, never pixel data.
    EXPECT_EQ(nullptr, dest->comps[c].data);
  }

  // Source buffers remain untouched.
  EXPECT_EQ(1234, src->comps[0].data[0]);
  EXPECT_EQ(-321, src->comps[1].data[11]);

  if (with_icc_profile) {
    EXPECT_EQ(static_cast<OPJ_UINT32>(kSampleIccProfile.size()),
              dest->icc_profile_len);
    ASSERT_NE(nullptr, dest->icc_profile_buf);
    EXPECT_NE(src->icc_profile_buf, dest->icc_profile_buf);
    EXPECT_THAT(
        std::span<const OPJ_BYTE>(dest->icc_profile_buf, dest->icc_profile_len),
        ElementsAreArray(kSampleIccProfile));
  } else {
    EXPECT_EQ(0u, dest->icc_profile_len);
    EXPECT_EQ(nullptr, dest->icc_profile_buf);
  }
}

TEST(ImageTest, Create0InitializesZeroedEmptyImage) {
  OpjImagePtr image(opj_image_create0());
  ASSERT_NE(nullptr, image);

  EXPECT_EQ(0u, image->x0);
  EXPECT_EQ(0u, image->y0);
  EXPECT_EQ(0u, image->x1);
  EXPECT_EQ(0u, image->y1);
  EXPECT_EQ(0u, image->numcomps);
  EXPECT_EQ(OPJ_CLRSPC_UNSPECIFIED, image->color_space);
  EXPECT_EQ(nullptr, image->comps);
  EXPECT_EQ(nullptr, image->icc_profile_buf);
  EXPECT_EQ(0u, image->icc_profile_len);
}

TEST(ImageTest,
     CreateValidMultiComponentAllocatesZeroedSamplesAndCopiesMetadata) {
  std::array<opj_image_cmptparm_t, 3> params = {
      MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/8, /*h=*/6, /*x0=*/2, /*y0=*/4,
                    /*prec=*/12, /*sgnd=*/0),
      MakeCmptParam(/*dx=*/2, /*dy=*/2, /*w=*/4, /*h=*/3, /*x0=*/1, /*y0=*/2,
                    /*prec=*/10, /*sgnd=*/1),
      MakeCmptParam(/*dx=*/2, /*dy=*/2, /*w=*/4, /*h=*/3, /*x0=*/1, /*y0=*/2,
                    /*prec=*/8, /*sgnd=*/0),
  };

  OpjImagePtr image(opj_image_create(static_cast<OPJ_UINT32>(params.size()),
                                     params.data(), OPJ_CLRSPC_SYCC));
  ASSERT_NE(nullptr, image);

  EXPECT_EQ(OPJ_CLRSPC_SYCC, image->color_space);
  EXPECT_EQ(3u, image->numcomps);
  EXPECT_EQ(0u, image->x0);
  EXPECT_EQ(0u, image->y0);
  EXPECT_EQ(0u, image->x1);
  EXPECT_EQ(0u, image->y1);
  EXPECT_EQ(nullptr, image->icc_profile_buf);
  EXPECT_EQ(0u, image->icc_profile_len);
  ASSERT_NE(nullptr, image->comps);

  for (size_t c = 0; c < params.size(); ++c) {
    ExpectComponentMatchesParam(params[c], image->comps[c]);
    ASSERT_NE(nullptr, image->comps[c].data);
    // opj_image_data_alloc() uses 16-byte aligned allocation.
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(image->comps[c].data) % 16u);

    const size_t sample_count = static_cast<size_t>(params[c].w) * params[c].h;
    EXPECT_THAT(std::span<const OPJ_INT32>(image->comps[c].data, sample_count),
                Each(0));

    // Verify full sample buffer range is writable without ASan faults.
    image->comps[c].data[0] = static_cast<OPJ_INT32>(c + 1);
    image->comps[c].data[sample_count - 1] = -static_cast<OPJ_INT32>(c + 10);
    EXPECT_EQ(static_cast<OPJ_INT32>(c + 1), image->comps[c].data[0]);
    EXPECT_EQ(-static_cast<OPJ_INT32>(c + 10),
              image->comps[c].data[sample_count - 1]);
  }
}

TEST(ImageTest, CreateWithZeroComponentsReturnsNull) {
  // opj_calloc(0, sizeof(opj_image_comp_t)) returns nullptr in opj_malloc.c,
  // exercising the !image->comps cleanup path without dereferencing cmptparms.
  OpjImagePtr image(opj_image_create(0, nullptr, OPJ_CLRSPC_SRGB));
  EXPECT_EQ(nullptr, image);
}

void ExpectCreateFailsOnInvalidDimension(OPJ_UINT32 w, OPJ_UINT32 h,
                                         bool with_prior_valid_comp,
                                         OPJ_COLOR_SPACE color_space) {
  std::vector<opj_image_cmptparm_t> params;
  if (with_prior_valid_comp) {
    params.push_back(MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/4, /*h=*/4,
                                   /*x0=*/0, /*y0=*/0, /*prec=*/8, /*sgnd=*/0));
  }
  params.push_back(MakeCmptParam(/*dx=*/1, /*dy=*/1, w, h, /*x0=*/0, /*y0=*/0,
                                 /*prec=*/8, /*sgnd=*/0));

  OpjImagePtr image(opj_image_create(static_cast<OPJ_UINT32>(params.size()),
                                     params.data(), color_space));
  EXPECT_EQ(nullptr, image);
}

TEST(ImageTest, CreateWithZeroDimensionComponentReturnsNullAndFreesPriorComps) {
  struct ZeroDimCase {
    OPJ_UINT32 w;
    OPJ_UINT32 h;
    bool with_prior_valid_comp;
  };
  constexpr ZeroDimCase kCases[] = {
      {.w = 4, .h = 0, .with_prior_valid_comp = false},
      {.w = 0, .h = 4, .with_prior_valid_comp = true},
  };

  // opj_image_data_alloc(0) returns nullptr, exercising !comp->data cleanup.
  for (const auto& test_case : kCases) {
    ExpectCreateFailsOnInvalidDimension(test_case.w, test_case.h,
                                        test_case.with_prior_valid_comp,
                                        OPJ_CLRSPC_GRAY);
  }
}

TEST(ImageTest,
     CreateWithDimensionProductOverflowReturnsNullAndFreesPriorComps) {
  // Derives from SIZE_MAX so opj_image_create()'s overflow check triggers
  // portably on both 32-bit and 64-bit builds.
  constexpr OPJ_UINT32 kMaxU32 = std::numeric_limits<OPJ_UINT32>::max();
  constexpr OPJ_UINT32 kOverflowWidthAtMaxHeight = static_cast<OPJ_UINT32>(
      (std::numeric_limits<OPJ_SIZE_T>::max() / kMaxU32 / sizeof(OPJ_INT32)) +
      1u);

  struct OverflowCase {
    OPJ_UINT32 w;
    OPJ_UINT32 h;
    bool with_prior_valid_comp;
  };
  constexpr OverflowCase kCases[] = {
      {.w = kMaxU32, .h = kMaxU32, .with_prior_valid_comp = false},
      {.w = kOverflowWidthAtMaxHeight,
       .h = kMaxU32,
       .with_prior_valid_comp = true},
      {.w = kMaxU32,
       .h = kOverflowWidthAtMaxHeight,
       .with_prior_valid_comp = true},
  };

  for (const auto& test_case : kCases) {
    ExpectCreateFailsOnInvalidDimension(test_case.w, test_case.h,
                                        test_case.with_prior_valid_comp,
                                        OPJ_CLRSPC_SRGB);
  }
}

TEST(ImageTest, TileCreatePopulatesMetadataWithoutAllocatingSampleData) {
  std::array<opj_image_cmptparm_t, 2> params = {
      // Large virtual dimensions succeed because opj_image_tile_create()
      // leaves comps[].data as nullptr.
      MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/0xFFFFFFFFu, /*h=*/0xFFFFFFFFu,
                    /*x0=*/16, /*y0=*/32, /*prec=*/16, /*sgnd=*/1),
      MakeCmptParam(/*dx=*/2, /*dy=*/4, /*w=*/128, /*h=*/64, /*x0=*/8, /*y0=*/8,
                    /*prec=*/8, /*sgnd=*/0),
  };

  OpjImagePtr image(opj_image_tile_create(
      static_cast<OPJ_UINT32>(params.size()), params.data(), OPJ_CLRSPC_EYCC));
  ASSERT_NE(nullptr, image);

  EXPECT_EQ(OPJ_CLRSPC_EYCC, image->color_space);
  EXPECT_EQ(2u, image->numcomps);
  EXPECT_EQ(0u, image->x0);
  EXPECT_EQ(0u, image->y0);
  EXPECT_EQ(0u, image->x1);
  EXPECT_EQ(0u, image->y1);
  EXPECT_EQ(nullptr, image->icc_profile_buf);
  EXPECT_EQ(0u, image->icc_profile_len);
  ASSERT_NE(nullptr, image->comps);

  for (size_t c = 0; c < params.size(); ++c) {
    ExpectComponentMatchesParam(params[c], image->comps[c]);
    EXPECT_EQ(nullptr, image->comps[c].data);
  }

  // Manually populate only component 1 via opj_image_data_alloc() to verify
  // opj_image_destroy() cleans up mixed null and non-null comps[].data.
  constexpr size_t kSamples = 4;
  auto* tile_samples = static_cast<OPJ_INT32*>(
      opj_image_data_alloc(kSamples * sizeof(OPJ_INT32)));
  ASSERT_NE(nullptr, tile_samples);
  image->comps[1].data = tile_samples;
  tile_samples[0] = -10;
  tile_samples[1] = 0;
  tile_samples[2] = 42;
  tile_samples[3] = 1023;
  EXPECT_EQ(42, image->comps[1].data[2]);
}

TEST(ImageTest, TileCreateWithZeroComponentsReturnsNull) {
  OpjImagePtr image(opj_image_tile_create(0, nullptr, OPJ_CLRSPC_GRAY));
  EXPECT_EQ(nullptr, image);
}

TEST(ImageTest, DestroyHandlesNullAndPopulatedImageWithIccProfile) {
  opj_image_destroy(nullptr);

  std::array<opj_image_cmptparm_t, 2> params = {
      MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/4, /*h=*/4, /*x0=*/0, /*y0=*/0,
                    /*prec=*/8, /*sgnd=*/0),
      MakeCmptParam(/*dx=*/2, /*dy=*/2, /*w=*/2, /*h=*/2, /*x0=*/0, /*y0=*/0,
                    /*prec=*/8, /*sgnd=*/0),
  };
  OpjImagePtr image(opj_image_create(static_cast<OPJ_UINT32>(params.size()),
                                     params.data(), OPJ_CLRSPC_SRGB));
  ASSERT_NE(nullptr, image);
  ASSERT_NO_FATAL_FAILURE(PopulateIccProfile(image.get(), kSampleIccProfile));

  image.reset();
  EXPECT_EQ(nullptr, image);
}

TEST(ImageTest, CompHeaderUpdateComputesTileIntersectedSubsampledDimensions) {
  // Zero initial geometry; opj_image_comp_header_update() derives w/h/x0/y0.
  std::array<opj_image_cmptparm_t, 3> params = {
      MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/1, /*h=*/1, /*x0=*/0, /*y0=*/0,
                    /*prec=*/8, /*sgnd=*/0),
      MakeCmptParam(/*dx=*/2, /*dy=*/2, /*w=*/1, /*h=*/1, /*x0=*/0, /*y0=*/0,
                    /*prec=*/10, /*sgnd=*/1),
      MakeCmptParam(/*dx=*/3, /*dy=*/4, /*w=*/1, /*h=*/1, /*x0=*/0, /*y0=*/0,
                    /*prec=*/12, /*sgnd=*/0),
  };
  OpjImagePtr image(opj_image_tile_create(
      static_cast<OPJ_UINT32>(params.size()), params.data(), OPJ_CLRSPC_SYCC));
  ASSERT_NE(nullptr, image);

  // Mix tx0 > x0 and ty0 < y0, and mix tile-grid vs image-extent upper bounds:
  //   l_x0 = max(10, 3) = 10
  //   l_y0 = max(5, 20) = 20
  //   l_x1 = min(10 + (3 - 1)*32 + 32, 100) = min(106, 100) = 100
  //   l_y1 = min(5 + (2 - 1)*32 + 32, 80)   = min(69, 80)   = 69
  image->x0 = 3;
  image->y0 = 20;
  image->x1 = 100;
  image->y1 = 80;
  image->comps[0].factor = 0;
  image->comps[1].factor = 1;
  image->comps[2].factor = 2;
  image->comps[2].alpha = 1;
  image->comps[2].resno_decoded = 4;

  opj_cp_t cp{};
  cp.tx0 = 10;
  cp.ty0 = 5;
  cp.tdx = 32;
  cp.tdy = 32;
  cp.tw = 3;
  cp.th = 2;

  opj_image_comp_header_update(image.get(), &cp);

  // Comp 0: dx=1, dy=1, factor=0 => x0=10, y0=20, w=90, h=49
  EXPECT_EQ(10u, image->comps[0].x0);
  EXPECT_EQ(20u, image->comps[0].y0);
  EXPECT_EQ(90u, image->comps[0].w);
  EXPECT_EQ(49u, image->comps[0].h);

  // Comp 1: dx=2, dy=2, factor=1 => ceil(10/2)=5, ceil(20/2)=10,
  //   ceil(100/2)=50, ceil(69/2)=35 => w=ceil(45/2)=23, h=ceil(25/2)=13
  EXPECT_EQ(5u, image->comps[1].x0);
  EXPECT_EQ(10u, image->comps[1].y0);
  EXPECT_EQ(23u, image->comps[1].w);
  EXPECT_EQ(13u, image->comps[1].h);

  // Comp 2: dx=3, dy=4, factor=2 => ceil(10/3)=4, ceil(20/4)=5,
  //   ceil(100/3)=34, ceil(69/4)=18 => w=ceil(30/4)=8, h=ceil(13/4)=4
  EXPECT_EQ(4u, image->comps[2].x0);
  EXPECT_EQ(5u, image->comps[2].y0);
  EXPECT_EQ(8u, image->comps[2].w);
  EXPECT_EQ(4u, image->comps[2].h);

  // Non-geometry component fields remain intact.
  EXPECT_EQ(12u, image->comps[2].prec);
  EXPECT_EQ(0u, image->comps[2].sgnd);
  EXPECT_EQ(1u, image->comps[2].alpha);
  EXPECT_EQ(4u, image->comps[2].resno_decoded);
  EXPECT_EQ(nullptr, image->comps[2].data);
}

TEST(ImageTest, CompHeaderUpdateHandlesSaturatedAddAndEmptyImage) {
  opj_cp_t cp{};
  cp.tx0 = 0xFFFFFFF0u;
  cp.ty0 = 0xFFFFFFE0u;
  cp.tdx = 0x20u;
  cp.tdy = 0x40u;
  cp.tw = 1;
  cp.th = 1;

  // Empty image (numcomps == 0) completes without touching comps.
  OpjImagePtr empty(opj_image_create0());
  ASSERT_NE(nullptr, empty);
  opj_image_comp_header_update(empty.get(), &cp);
  EXPECT_EQ(0u, empty->numcomps);
  EXPECT_EQ(nullptr, empty->comps);

  // Near UINT32_MAX: opj_uint_adds(0xFFFFFFF0, 0x20) saturates to UINT32_MAX
  // rather than wrapping past 2^32, so x1/y1 clamp to image->x1/y1 cleanly.
  std::array<opj_image_cmptparm_t, 1> params = {
      MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/1, /*h=*/1, /*x0=*/0, /*y0=*/0,
                    /*prec=*/8, /*sgnd=*/0),
  };
  OpjImagePtr image(opj_image_tile_create(1, params.data(), OPJ_CLRSPC_GRAY));
  ASSERT_NE(nullptr, image);
  image->x0 = 0xFFFFFFF0u;
  image->y0 = 0xFFFFFFE0u;
  image->x1 = 0xFFFFFFFCu;
  image->y1 = 0xFFFFFFF8u;

  opj_image_comp_header_update(image.get(), &cp);

  EXPECT_EQ(0xFFFFFFF0u, image->comps[0].x0);
  EXPECT_EQ(0xFFFFFFE0u, image->comps[0].y0);
  EXPECT_EQ(12u, image->comps[0].w);
  EXPECT_EQ(24u, image->comps[0].h);
}

TEST(ImageTest, CopyImageHeaderIntoEmptyDestWithoutAndWithIccProfile) {
  for (const bool with_icc : {false, true}) {
    OpjImagePtr dest(opj_image_create0());
    ASSERT_NE(nullptr, dest);
    ASSERT_NO_FATAL_FAILURE(VerifyHeaderCopyIntoDest(dest.get(), with_icc));
  }
}

TEST(ImageTest, CopyImageHeaderIntoPopulatedDestFreesExistingCompsAndData) {
  // Note: opj_copy_image_header() frees dest->comps and comps[].data, then
  // overwrites dest->icc_profile_buf without freeing any prior dest profile.
  // Keep dest->icc_profile_buf == nullptr prior to copying.
  for (const bool with_icc : {false, true}) {
    std::array<opj_image_cmptparm_t, 3> dest_params = {
        MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/3, /*h=*/3, /*x0=*/1, /*y0=*/1,
                      /*prec=*/8, /*sgnd=*/0),
        MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/3, /*h=*/3, /*x0=*/1, /*y0=*/1,
                      /*prec=*/8, /*sgnd=*/0),
        MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/3, /*h=*/3, /*x0=*/1, /*y0=*/1,
                      /*prec=*/8, /*sgnd=*/0),
    };
    OpjImagePtr dest(
        opj_image_create(static_cast<OPJ_UINT32>(dest_params.size()),
                         dest_params.data(), OPJ_CLRSPC_SRGB));
    ASSERT_NE(nullptr, dest);
    dest->x0 = 99;
    dest->y0 = 88;
    dest->x1 = 777;
    dest->y1 = 666;

    ASSERT_NO_FATAL_FAILURE(VerifyHeaderCopyIntoDest(dest.get(), with_icc));
  }

  // Also cover dest having comps != nullptr with comps[].data == nullptr
  // (as produced by opj_image_tile_create()).
  std::array<opj_image_cmptparm_t, 1> tile_params = {
      MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/16, /*h=*/16, /*x0=*/0, /*y0=*/0,
                    /*prec=*/8, /*sgnd=*/0),
  };
  OpjImagePtr tile_dest(
      opj_image_tile_create(1, tile_params.data(), OPJ_CLRSPC_GRAY));
  ASSERT_NE(nullptr, tile_dest);
  ASSERT_NO_FATAL_FAILURE(
      VerifyHeaderCopyIntoDest(tile_dest.get(), /*with_icc_profile=*/true));
}

TEST(ImageTest, CopyImageHeaderFromEmptySourceClearsExistingDestComps) {
  std::array<opj_image_cmptparm_t, 2> dest_params = {
      MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/4, /*h=*/4, /*x0=*/0, /*y0=*/0,
                    /*prec=*/8, /*sgnd=*/0),
      MakeCmptParam(/*dx=*/1, /*dy=*/1, /*w=*/4, /*h=*/4, /*x0=*/0, /*y0=*/0,
                    /*prec=*/8, /*sgnd=*/0),
  };
  OpjImagePtr dest(opj_image_create(static_cast<OPJ_UINT32>(dest_params.size()),
                                    dest_params.data(), OPJ_CLRSPC_SRGB));
  ASSERT_NE(nullptr, dest);

  OpjImagePtr empty_src(opj_image_create0());
  ASSERT_NE(nullptr, empty_src);
  empty_src->x0 = 5;
  empty_src->y0 = 7;
  empty_src->x1 = 25;
  empty_src->y1 = 35;

  // Because opj_malloc(0) returns nullptr, opj_copy_image_header() frees
  // dest->comps and returns early at !p_image_dest->comps with numcomps == 0.
  opj_copy_image_header(empty_src.get(), dest.get());

  EXPECT_EQ(5u, dest->x0);
  EXPECT_EQ(7u, dest->y0);
  EXPECT_EQ(25u, dest->x1);
  EXPECT_EQ(35u, dest->y1);
  EXPECT_EQ(0u, dest->numcomps);
  EXPECT_EQ(nullptr, dest->comps);
}

// TODO(schwehr): Cover remaining OOM branches (`!image` in opj_image_create()
// and opj_image_tile_create(), and `!p_image_dest->icc_profile_buf` in
// opj_copy_image_header()) via :openjpeg2_internal_no_malloc and
// ScopedOpjAllocFailCountdown.

}  // namespace
}  // namespace openjpeg2
