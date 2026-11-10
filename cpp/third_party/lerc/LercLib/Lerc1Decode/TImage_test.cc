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
// Unit tests for LercNS::CntZ, Image, and TImage<Element>
// (third_party/lerc/LercLib/Lerc1Decode/TImage.hpp).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc1Decode/Image.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc1Decode/TImage.hpp

#include "third_party/lerc/LercLib/Lerc1Decode/TImage.hpp"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/lerc/LercLib/Defines.h"
#include "third_party/lerc/LercLib/Lerc1Decode/Image.h"

#ifndef HAVE_LERC1_DECODE
#error "Lerc1Decode tests require HAVE_LERC1_DECODE"
#endif

namespace lerc {
namespace {

using ::LercNS::CntZ;
using ::LercNS::Image;
using ::LercNS::TImage;

template <typename Element, Image::Type kImgType>
class ConcreteImage final : public TImage<Element> {
 public:
  ConcreteImage() { this->type_ = kImgType; }
  ConcreteImage(const ConcreteImage& other) = default;
  ConcreteImage& operator=(const ConcreteImage& other) = default;

  std::string getTypeString() const override { return "ConcreteImage"; }

  bool ImageMetadataNotEqual(const Image& other) const {
    return Image::operator!=(other);
  }
};

TEST(TImageTest, CntZComparisonAndAccumulation) {
  CntZ a{1.0f, 10.5f};
  const CntZ b{1.0f, 10.5f};
  const CntZ diff_cnt{0.0f, 10.5f};
  const CntZ diff_z{1.0f, -2.5f};

  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a == diff_cnt);
  EXPECT_FALSE(a == diff_z);

  EXPECT_FALSE(a != b);
  EXPECT_TRUE(a != diff_cnt);
  EXPECT_TRUE(a != diff_z);

  a += CntZ{2.0f, -0.5f};
  EXPECT_FLOAT_EQ(3.0f, a.cnt);
  EXPECT_FLOAT_EQ(10.0f, a.z);
}

TEST(TImageTest, DefaultStateResizeAccessorsAndClear) {
  ConcreteImage<float, Image::FLOAT> img;
  const auto& const_img = img;

  EXPECT_TRUE(img.isType(Image::FLOAT));
  EXPECT_FALSE(img.isType(Image::CNT_Z));
  EXPECT_EQ(Image::FLOAT, img.getType());
  EXPECT_EQ("ConcreteImage", img.getTypeString());
  EXPECT_EQ(0, img.getWidth());
  EXPECT_EQ(0, img.getHeight());
  EXPECT_EQ(0, img.getSize());
  EXPECT_EQ(nullptr, img.getData());
  EXPECT_EQ(nullptr, const_img.getData());
  EXPECT_FALSE(img.isInside(0, 0));

  EXPECT_FALSE(img.resize(0, 3));
  EXPECT_FALSE(img.resize(3, 0));
  EXPECT_FALSE(img.resize(-2, 4));
  EXPECT_FALSE(img.resize(4, -2));

  ASSERT_TRUE(img.resize(3, 2));
  EXPECT_EQ(3, img.getWidth());
  EXPECT_EQ(2, img.getHeight());
  EXPECT_EQ(6, img.getSize());
  EXPECT_TRUE(img.isInside(0, 0));
  EXPECT_TRUE(img.isInside(1, 2));
  EXPECT_FALSE(img.isInside(-1, 0));
  EXPECT_FALSE(img.isInside(2, 0));
  EXPECT_FALSE(img.isInside(0, -1));
  EXPECT_FALSE(img.isInside(0, 3));

  // Fast-path no-op when dimensions match existing allocation.
  const float* initial_ptr = const_img.getData();
  ASSERT_NE(nullptr, initial_ptr);
  EXPECT_TRUE(img.resize(3, 2));
  EXPECT_EQ(initial_ptr, const_img.getData());

  img.setPixel(0, 0, 1.5f);
  img(1, 2) = -7.25f;
  img.getData()[1] = 2.0f;

  EXPECT_FLOAT_EQ(1.5f, const_img.getPixel(0, 0));
  EXPECT_FLOAT_EQ(2.0f, const_img(0, 1));
  EXPECT_FLOAT_EQ(-7.25f, const_img(1, 2));

  // Resize to new dimensions reallocates storage and updates geometry.
  ASSERT_TRUE(img.resize(2, 4));
  EXPECT_EQ(2, img.getWidth());
  EXPECT_EQ(4, img.getHeight());
  EXPECT_EQ(8, img.getSize());

  img.clear();
  EXPECT_EQ(0, img.getWidth());
  EXPECT_EQ(0, img.getHeight());
  EXPECT_EQ(0, img.getSize());
  EXPECT_EQ(nullptr, const_img.getData());
}

TEST(TImageTest, CopyAssignSelfAssignEmptyAssignAndEquality) {
  ConcreteImage<CntZ, Image::CNT_Z> src;
  ASSERT_TRUE(src.resize(2, 2));
  src.setPixel(0, 0, CntZ{1.0f, 3.0f});
  src.setPixel(0, 1, CntZ{0.0f, 0.0f});
  src.setPixel(1, 0, CntZ{1.0f, 4.0f});
  src.setPixel(1, 1, CntZ{1.0f, 5.0f});

  // 1. Copy construction preserves metadata and pixel values deeply.
  ConcreteImage<CntZ, Image::CNT_Z> copy_constructed(src);
  EXPECT_TRUE(copy_constructed == src);
  EXPECT_FALSE(copy_constructed != src);
  EXPECT_NE(src.getData(), copy_constructed.getData());

  // 2. Pixel mismatch triggers operator!=.
  copy_constructed(1, 1).z = 99.0f;
  EXPECT_FALSE(copy_constructed == src);
  EXPECT_TRUE(copy_constructed != src);

  // 3. Copy assignment copies pixel payload back and restores equality.
  copy_constructed = src;
  EXPECT_TRUE(copy_constructed == src);

  // 4. Self-assignment (via reference alias) is a safe no-op.
  ConcreteImage<CntZ, Image::CNT_Z>& src_alias = src;
  src = src_alias;
  EXPECT_TRUE(src == copy_constructed);

  // 5. Assigning an empty 0x0 image leaves destination unchanged because
  // resize(0, 0) returns false early in TImage::operator=.
  const ConcreteImage<CntZ, Image::CNT_Z> empty_src;
  copy_constructed = empty_src;
  EXPECT_EQ(2, copy_constructed.getWidth());
  EXPECT_TRUE(copy_constructed == src);

  // 6. Dimension mismatch or image type mismatch fails Image comparison.
  ConcreteImage<CntZ, Image::CNT_Z> wider;
  ASSERT_TRUE(wider.resize(3, 2));
  EXPECT_TRUE(src != wider);
  EXPECT_TRUE(src.ImageMetadataNotEqual(wider));

  ConcreteImage<CntZ, Image::FLOAT> wrong_type;
  ASSERT_TRUE(wrong_type.resize(2, 2));
  EXPECT_FALSE(src == wrong_type);
}

}  // namespace
}  // namespace lerc
