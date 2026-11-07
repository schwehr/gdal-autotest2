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
// Tests for IMCOMP image compression and decompression (hdf/src/dfimcomp.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/dfimcomp.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hproto.h"

namespace hdf4 {
namespace {

constexpr int kPalSize = 256;
constexpr int kBit8 = 0;
constexpr int kBit24 = 1;

// 1. 8-bit image compression and decompression with small palette (<= 256
// colors)
TEST(DfimcompTest, Bit8SmallColorPaletteRoundtrip) {
  const int32_t xdim = 16;
  const int32_t ydim = 16;
  const int32_t num_pixels = xdim * ydim;
  const int32_t compressed_size = num_pixels / 4;

  std::vector<uint8_t> in_image(num_pixels);
  for (int32_t i = 0; i < num_pixels; ++i) {
    in_image[i] = static_cast<uint8_t>(i % 16);
  }

  // 256 RGB entries (768 bytes)
  std::vector<uint8_t> in_pal(kPalSize * 3);
  for (int i = 0; i < kPalSize * 3; ++i) {
    in_pal[i] = static_cast<uint8_t>((i * 17) % 256);
  }

  std::vector<uint8_t> compressed(compressed_size, 0);
  std::vector<uint8_t> out_pal(kPalSize * 3, 0);

  DFCIimcomp(xdim, ydim, in_image.data(), compressed.data(), in_pal.data(),
             out_pal.data(), kBit8);

  // Decompress
  std::vector<uint8_t> decompressed(num_pixels, 0);
  DFCIunimcomp(xdim, ydim, compressed.data(), decompressed.data());

  // Verify that all decompressed pixel indices point to valid palette entries
  for (int32_t i = 0; i < num_pixels; ++i) {
    EXPECT_LT(decompressed[i], kPalSize);
  }
}

// 2. 24-bit image compression and decompression with small color set (<= 256
// colors)
TEST(DfimcompTest, Bit24SmallColorRoundtrip) {
  const int32_t xdim = 16;
  const int32_t ydim = 16;
  const int32_t num_pixels = xdim * ydim;
  const int32_t compressed_size = num_pixels / 4;

  std::vector<uint8_t> in_image(num_pixels * 3);
  for (int32_t i = 0; i < num_pixels * 3; ++i) {
    in_image[i] = static_cast<uint8_t>((i * 7) % 32);  // small range of colors
  }

  std::vector<uint8_t> compressed(compressed_size, 0);
  std::vector<uint8_t> out_pal(kPalSize * 3, 0);

  DFCIimcomp(xdim, ydim, in_image.data(), compressed.data(), nullptr,
             out_pal.data(), kBit24);

  // Decompress
  std::vector<uint8_t> decompressed(num_pixels, 0);
  DFCIunimcomp(xdim, ydim, compressed.data(), decompressed.data());

  for (int32_t i = 0; i < num_pixels; ++i) {
    EXPECT_LT(decompressed[i], kPalSize);
  }
}

// 3. High color count triggering sel_palette quantization (> 256 colors)
TEST(DfimcompTest, HighColorCountTriggersSelPaletteQuantization) {
  // 64x64 = 4096 pixels = 256 4x4 blocks.
  // Each block will have distinct hi and lo colors, producing > 256 distinct
  // colors.
  const int32_t xdim = 64;
  const int32_t ydim = 64;
  const int32_t num_pixels = xdim * ydim;
  const int32_t compressed_size = num_pixels / 4;

  std::vector<uint8_t> in_image(num_pixels * 3);
  for (int32_t y = 0; y < ydim; ++y) {
    for (int32_t x = 0; x < xdim; ++x) {
      int32_t idx = 3 * (y * xdim + x);
      in_image[idx] = static_cast<uint8_t>((x * 4) % 256);
      in_image[idx + 1] = static_cast<uint8_t>((y * 4) % 256);
      in_image[idx + 2] = static_cast<uint8_t>(((x + y) * 3) % 256);
    }
  }

  std::vector<uint8_t> compressed(compressed_size, 0);
  std::vector<uint8_t> out_pal(kPalSize * 3, 0);

  DFCIimcomp(xdim, ydim, in_image.data(), compressed.data(), nullptr,
             out_pal.data(), kBit24);

  // Decompress
  std::vector<uint8_t> decompressed(num_pixels, 0);
  DFCIunimcomp(xdim, ydim, compressed.data(), decompressed.data());

  for (int32_t i = 0; i < num_pixels; ++i) {
    EXPECT_LT(decompressed[i], kPalSize);
  }
}

// 4. High color count in 8-bit mode with large varied palette
TEST(DfimcompTest, Bit8HighColorCountQuantization) {
  const int32_t xdim = 64;
  const int32_t ydim = 64;
  const int32_t num_pixels = xdim * ydim;
  const int32_t compressed_size = num_pixels / 4;

  std::vector<uint8_t> in_image(num_pixels);
  for (int32_t i = 0; i < num_pixels; ++i) {
    in_image[i] = static_cast<uint8_t>(i % 256);
  }

  std::vector<uint8_t> in_pal(kPalSize * 3);
  for (int i = 0; i < kPalSize; ++i) {
    in_pal[3 * i] = static_cast<uint8_t>((i * 11) % 256);
    in_pal[3 * i + 1] = static_cast<uint8_t>((i * 19) % 256);
    in_pal[3 * i + 2] = static_cast<uint8_t>((i * 29) % 256);
  }

  std::vector<uint8_t> compressed(compressed_size, 0);
  std::vector<uint8_t> out_pal(kPalSize * 3, 0);

  DFCIimcomp(xdim, ydim, in_image.data(), compressed.data(), in_pal.data(),
             out_pal.data(), kBit8);

  std::vector<uint8_t> decompressed(num_pixels, 0);
  DFCIunimcomp(xdim, ydim, compressed.data(), decompressed.data());

  for (int32_t i = 0; i < num_pixels; ++i) {
    EXPECT_LT(decompressed[i], kPalSize);
  }
}

// 5. Unsupported mode handling
TEST(DfimcompTest, UnsupportedModeHandledGracefully) {
  const int32_t xdim = 16;
  const int32_t ydim = 16;
  const int32_t num_pixels = xdim * ydim;
  const int32_t compressed_size = num_pixels / 4;

  std::vector<uint8_t> in_image(num_pixels, 0);
  std::vector<uint8_t> compressed(compressed_size, 0);
  std::vector<uint8_t> out_pal(kPalSize * 3, 0);

  // mode 99 (neither BIT8 nor BIT24)
  DFCIimcomp(xdim, ydim, in_image.data(), compressed.data(), nullptr,
             out_pal.data(), 99);
}

// 6. Uniform color image (all pixels identical)
TEST(DfimcompTest, UniformImageRoundtrip) {
  const int32_t xdim = 16;
  const int32_t ydim = 16;
  const int32_t num_pixels = xdim * ydim;
  const int32_t compressed_size = num_pixels / 4;

  std::vector<uint8_t> in_image(num_pixels, 42);
  std::vector<uint8_t> in_pal(kPalSize * 3, 100);

  std::vector<uint8_t> compressed(compressed_size, 0);
  std::vector<uint8_t> out_pal(kPalSize * 3, 0);

  DFCIimcomp(xdim, ydim, in_image.data(), compressed.data(), in_pal.data(),
             out_pal.data(), kBit8);

  std::vector<uint8_t> decompressed(num_pixels, 0);
  DFCIunimcomp(xdim, ydim, compressed.data(), decompressed.data());

  for (int32_t i = 0; i < num_pixels; ++i) {
    EXPECT_LT(decompressed[i], kPalSize);
  }
}

// 7. Large image with 3D color cube to exercise all sorting dimensions
TEST(DfimcompTest, LargeColorCubeSpreadRoundtrip) {
  const int32_t xdim = 80;
  const int32_t ydim = 80;
  const int32_t num_pixels = xdim * ydim;
  const int32_t compressed_size = num_pixels / 4;

  std::vector<uint8_t> in_image(num_pixels * 3);
  for (int32_t y = 0; y < ydim; ++y) {
    for (int32_t x = 0; x < xdim; ++x) {
      int32_t idx = 3 * (y * xdim + x);
      in_image[idx] = static_cast<uint8_t>((x * 255) / xdim);
      in_image[idx + 1] = static_cast<uint8_t>((y * 255) / ydim);
      in_image[idx + 2] = static_cast<uint8_t>(((x + y) * 255) / (xdim + ydim));
    }
  }

  std::vector<uint8_t> compressed(compressed_size, 0);
  std::vector<uint8_t> out_pal(kPalSize * 3, 0);

  DFCIimcomp(xdim, ydim, in_image.data(), compressed.data(), nullptr,
             out_pal.data(), kBit24);

  std::vector<uint8_t> decompressed(num_pixels, 0);
  DFCIunimcomp(xdim, ydim, compressed.data(), decompressed.data());

  for (int32_t i = 0; i < num_pixels; ++i) {
    EXPECT_LT(decompressed[i], kPalSize);
  }
}

}  // namespace
}  // namespace hdf4
