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
// Tests for Zarr shuffle, quantize, and fixedscaleoffset filters
// (zarr_filters.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_filters.cpp

#include <cstddef>
#include <cstdint>
#include <format>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/port/cpl_compressor.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

TEST(ZarrFiltersTest, ShuffleCompressDecompressRoundTrip) {
  const CPLCompressor* comp = ZarrGetShuffleCompressor();
  const CPLCompressor* decomp = ZarrGetShuffleDecompressor();
  ASSERT_NE(nullptr, comp);
  ASSERT_NE(nullptr, decomp);

  EXPECT_STREQ("shuffle", comp->pszId);
  EXPECT_EQ(CCT_FILTER, comp->eType);
  EXPECT_STREQ("shuffle", decomp->pszId);
  EXPECT_EQ(CCT_FILTER, decomp->eType);

  for (const int elt_size : {1, 2, 4, 8}) {
    std::string elt_opt = std::format("ELEMENTSIZE={}", elt_size);
    char* options[] = {elt_opt.data(), nullptr};

    std::vector<uint8_t> original(elt_size * 4);
    for (size_t i = 0; i < original.size(); ++i) {
      original[i] = static_cast<uint8_t>(i + 1);
    }

    std::vector<uint8_t> compressed(original.size(), 0);
    void* comp_ptr = compressed.data();
    size_t comp_size = compressed.size();
    EXPECT_TRUE(comp->pfnFunc(original.data(), original.size(), &comp_ptr,
                              &comp_size, options, nullptr));
    EXPECT_EQ(original.size(), comp_size);
    if (elt_size == 2) {
      EXPECT_THAT(compressed, ElementsAre(1, 3, 5, 7, 2, 4, 6, 8));
    }

    std::vector<uint8_t> decompressed(original.size(), 0);
    void* decomp_ptr = decompressed.data();
    size_t decomp_size = decompressed.size();
    EXPECT_TRUE(decomp->pfnFunc(compressed.data(), compressed.size(),
                                &decomp_ptr, &decomp_size, options, nullptr));
    EXPECT_EQ(original.size(), decomp_size);
    EXPECT_EQ(original, decompressed);
  }
}

TEST(ZarrFiltersTest, ShuffleErrorHandling) {
  const CPLCompressor* comp = ZarrGetShuffleCompressor();
  const CPLCompressor* decomp = ZarrGetShuffleDecompressor();
  ASSERT_NE(nullptr, comp);
  ASSERT_NE(nullptr, decomp);

  const uint8_t input[7] = {1, 2, 3, 4, 5, 6, 7};
  uint8_t output[16] = {0};
  void* out_ptr = output;
  size_t out_size = sizeof(output);

  const ::WithQuietHandler quiet;

  // Invalid ELEMENTSIZE
  {
    char* options[] = {const_cast<char*>("ELEMENTSIZE=3"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(comp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                               options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Only ELEMENTSIZE=1,2,4,8"));

    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Only ELEMENTSIZE=1,2,4,8"));
  }

  // input_size not multiple of ELEMENTSIZE
  {
    char* options[] = {const_cast<char*>("ELEMENTSIZE=4"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(comp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                               options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("multiple of ELEMENTSIZE"));

    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("multiple of ELEMENTSIZE"));
  }

  // Too small output size
  {
    const uint8_t input_aligned[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    out_size = 4;
    CPLErrorReset();
    EXPECT_FALSE(comp->pfnFunc(input_aligned, sizeof(input_aligned), &out_ptr,
                               &out_size, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too small output size"));

    out_size = 4;
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input_aligned, sizeof(input_aligned), &out_ptr,
                                 &out_size, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too small output size"));
  }

  // Invalid API
  {
    const uint8_t input4[] = {1, 2, 3, 4};
    CPLErrorReset();
    EXPECT_FALSE(comp->pfnFunc(input4, sizeof(input4), nullptr, &out_size,
                               nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid use of API"));

    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input4, sizeof(input4), nullptr, &out_size,
                                 nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid use of API"));
  }
}

TEST(ZarrFiltersTest, QuantizeDecompressorHappyPaths) {
  const CPLCompressor* decomp = ZarrGetQuantizeDecompressor();
  ASSERT_NE(nullptr, decomp);
  EXPECT_STREQ("quantize", decomp->pszId);
  EXPECT_EQ(CCT_FILTER, decomp->eType);

  // <f4 to <f4
  {
    char* options[] = {const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=<f4"), nullptr};
    const float input[2] = {1.5f, 2.5f};
    float output[2] = {0.0f, 0.0f};
    void* out_ptr = output;
    size_t out_size = sizeof(output);

    EXPECT_TRUE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                options, nullptr));
    EXPECT_EQ(sizeof(output), out_size);
    EXPECT_FLOAT_EQ(1.5f, output[0]);
    EXPECT_FLOAT_EQ(2.5f, output[1]);
  }

  // <f4 to <f8
  {
    char* options[] = {const_cast<char*>("DTYPE=<f8"),
                       const_cast<char*>("ASTYPE=<f4"), nullptr};
    const float input[2] = {10.0f, 20.0f};
    double output[2] = {0.0, 0.0};
    void* out_ptr = output;
    size_t out_size = sizeof(output);

    EXPECT_TRUE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                options, nullptr));
    EXPECT_EQ(sizeof(output), out_size);
    EXPECT_DOUBLE_EQ(10.0, output[0]);
    EXPECT_DOUBLE_EQ(20.0, output[1]);
  }

  // <f8 to <f8
  {
    char* options[] = {const_cast<char*>("DTYPE=<f8"),
                       const_cast<char*>("ASTYPE=<f8"), nullptr};
    const double input[2] = {100.0, 200.0};
    double output[2] = {0.0, 0.0};
    void* out_ptr = output;
    size_t out_size = sizeof(output);

    EXPECT_TRUE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                options, nullptr));
    EXPECT_EQ(sizeof(output), out_size);
    EXPECT_DOUBLE_EQ(100.0, output[0]);
    EXPECT_DOUBLE_EQ(200.0, output[1]);
  }
}

TEST(ZarrFiltersTest, QuantizeDecompressorErrors) {
  const CPLCompressor* decomp = ZarrGetQuantizeDecompressor();
  ASSERT_NE(nullptr, decomp);

  const float input[2] = {1.0f, 2.0f};
  float output[2] = {0.0f, 0.0f};
  void* out_ptr = output;
  size_t out_size = sizeof(output);

  const ::WithQuietHandler quiet;

  // DTYPE missing
  {
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("quantize: DTYPE missing"));
  }

  // DTYPE invalid
  {
    char* options[] = {const_cast<char*>("DTYPE=<f2"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Only DTYPE=<f4 or <f8"));
  }

  // ASTYPE missing
  {
    char* options[] = {const_cast<char*>("DTYPE=<f4"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("quantize: ASTYPE missing"));
  }

  // ASTYPE invalid
  {
    char* options[] = {const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=<f2"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Only ASTYPE=<f4 or <f8"));
  }

  // input_size not multiple of sizeof(ASTYPE)
  {
    char* options[] = {const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=<f4"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(
        decomp->pfnFunc(input, 3, &out_ptr, &out_size, options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("multiple of sizeof(ASTYPE)"));
  }

  // Output buffer too small
  {
    char* options[] = {const_cast<char*>("DTYPE=<f8"),
                       const_cast<char*>("ASTYPE=<f4"), nullptr};
    out_size = 4;
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too small output size"));
  }

  // Invalid API
  {
    char* options[] = {const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=<f4"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), nullptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid use of API"));
  }
}

TEST(ZarrFiltersTest, FixedScaleOffsetDecompressorHappyPaths) {
  const CPLCompressor* decomp = ZarrGetFixedScaleOffsetDecompressor();
  ASSERT_NE(nullptr, decomp);
  EXPECT_STREQ("fixedscaleoffset", decomp->pszId);
  EXPECT_EQ(CCT_FILTER, decomp->eType);

  // ASTYPE=|u1, DTYPE=<f4, SCALE=10, OFFSET=5 -> val = (input / 10.0) + 5.0
  {
    char* options[] = {const_cast<char*>("OFFSET=5.0"),
                       const_cast<char*>("SCALE=10.0"),
                       const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=|u1"), nullptr};
    const uint8_t input[2] = {20, 50};
    float output[2] = {0.0f, 0.0f};
    void* out_ptr = output;
    size_t out_size = sizeof(output);

    EXPECT_TRUE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                options, nullptr));
    EXPECT_EQ(sizeof(output), out_size);
    EXPECT_FLOAT_EQ(7.0f, output[0]);
    EXPECT_FLOAT_EQ(10.0f, output[1]);
  }

  // ASTYPE=<u2, DTYPE=<f8, SCALE=2, OFFSET=1 -> val = (input / 2.0) + 1.0
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"),
                       const_cast<char*>("SCALE=2.0"),
                       const_cast<char*>("DTYPE=<f8"),
                       const_cast<char*>("ASTYPE=<u2"), nullptr};
    const uint16_t input[2] = {10, 20};
    double output[2] = {0.0, 0.0};
    void* out_ptr = output;
    size_t out_size = sizeof(output);

    EXPECT_TRUE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                options, nullptr));
    EXPECT_EQ(sizeof(output), out_size);
    EXPECT_DOUBLE_EQ(6.0, output[0]);
    EXPECT_DOUBLE_EQ(11.0, output[1]);
  }

  // ASTYPE=<u4, DTYPE=<f4
  {
    char* options[] = {const_cast<char*>("OFFSET=0.0"),
                       const_cast<char*>("SCALE=1.0"),
                       const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=<u4"), nullptr};
    const uint32_t input[1] = {42};
    float output[1] = {0.0f};
    void* out_ptr = output;
    size_t out_size = sizeof(output);

    EXPECT_TRUE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                options, nullptr));
    EXPECT_EQ(sizeof(output), out_size);
    EXPECT_FLOAT_EQ(42.0f, output[0]);
  }
}

TEST(ZarrFiltersTest, FixedScaleOffsetDecompressorErrors) {
  const CPLCompressor* decomp = ZarrGetFixedScaleOffsetDecompressor();
  ASSERT_NE(nullptr, decomp);

  const uint8_t input[4] = {1, 2, 3, 4};
  float output[4] = {0};
  void* out_ptr = output;
  size_t out_size = sizeof(output);

  const ::WithQuietHandler quiet;

  // OFFSET missing
  {
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("fixedscaleoffset: OFFSET missing"));
  }

  // SCALE missing
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("fixedscaleoffset: SCALE missing"));
  }

  // SCALE == 0
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"),
                       const_cast<char*>("SCALE=0.0"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("SCALE = 0 is invalid"));
  }

  // DTYPE missing
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"),
                       const_cast<char*>("SCALE=1.0"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("fixedscaleoffset: DTYPE missing"));
  }

  // DTYPE invalid
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"),
                       const_cast<char*>("SCALE=1.0"),
                       const_cast<char*>("DTYPE=<i4"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("DTYPE"));
  }

  // ASTYPE missing
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"),
                       const_cast<char*>("SCALE=1.0"),
                       const_cast<char*>("DTYPE=<f4"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("fixedscaleoffset: ASTYPE missing"));
  }

  // ASTYPE invalid
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"),
                       const_cast<char*>("SCALE=1.0"),
                       const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=<i2"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("ASTYPE"));
  }

  // input_size not multiple of ASTYPE size
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"),
                       const_cast<char*>("SCALE=1.0"),
                       const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=<u2"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(
        decomp->pfnFunc(input, 3, &out_ptr, &out_size, options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("multiple of"));
  }

  // Output buffer too small
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"),
                       const_cast<char*>("SCALE=1.0"),
                       const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=|u1"), nullptr};
    out_size = 2;  // input is 4 bytes -> output needs 16 bytes
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), &out_ptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too small output size"));
  }

  // Invalid API
  {
    char* options[] = {const_cast<char*>("OFFSET=1.0"),
                       const_cast<char*>("SCALE=1.0"),
                       const_cast<char*>("DTYPE=<f4"),
                       const_cast<char*>("ASTYPE=|u1"), nullptr};
    CPLErrorReset();
    EXPECT_FALSE(decomp->pfnFunc(input, sizeof(input), nullptr, &out_size,
                                 options, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid use of API"));
  }
}

// TODO: b/335317901 - Remaining uncovered lines in zarr_filters.cpp are 32-bit
// SIZEOF_VOIDP < 8 overflow guards.

}  // namespace
}  // namespace autotest2
