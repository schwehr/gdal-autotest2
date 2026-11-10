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
// Unit tests for LercNS::Predictor
// (third_party/lerc/LercLib/fpl_Predictor.cpp).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_Predictor.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_Predictor.cpp

#include "third_party/lerc/LercLib/fpl_Predictor.h"

#include "testing/base/public/gunit.h"

namespace lerc {
namespace {

using ::LercNS::Predictor;
using ::LercNS::PredictorType;

TEST(FplPredictorTest, MapsCodesDeltasAndMaxByteDeltas) {
  struct PredictorCase {
    PredictorType type;
    unsigned char code;
    int int_delta;
    int max_byte_delta;
  };
  constexpr PredictorCase kCases[] = {
      {.type = PredictorType::PREDICTOR_NONE,
       .code = 0,
       .int_delta = 0,
       .max_byte_delta = 5},
      {.type = PredictorType::PREDICTOR_DELTA1,
       .code = 1,
       .int_delta = 1,
       .max_byte_delta = 4},
      {.type = PredictorType::PREDICTOR_ROWS_COLS,
       .code = 2,
       .int_delta = 2,
       .max_byte_delta = 3},
      {.type = PredictorType::PREDICTOR_UNKNOWN,
       .code = 255,
       .int_delta = 0,
       .max_byte_delta = 5},
  };

  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.code, Predictor::getCode(tc.type));
    EXPECT_EQ(tc.type, Predictor::getType(static_cast<char>(tc.code)));
    EXPECT_EQ(tc.int_delta, Predictor::getIntDelta(tc.type));
    EXPECT_EQ(tc.max_byte_delta, Predictor::getMaxByteDelta(tc.type));
  }

  EXPECT_EQ(PredictorType::PREDICTOR_UNKNOWN, Predictor::getType(3));
  EXPECT_EQ(PredictorType::PREDICTOR_UNKNOWN,
            Predictor::getType(static_cast<char>(127)));
}

TEST(FplPredictorTest, FromDeltaAndCrossCoversAllValidAndInvalidPairs) {
  struct DeltaCrossCase {
    int delta;
    bool cross;
    PredictorType expected;
  };
  constexpr DeltaCrossCase kCases[] = {
      {.delta = -1,
       .cross = false,
       .expected = PredictorType::PREDICTOR_UNKNOWN},
      {.delta = -1,
       .cross = true,
       .expected = PredictorType::PREDICTOR_UNKNOWN},
      {.delta = 0, .cross = false, .expected = PredictorType::PREDICTOR_NONE},
      {.delta = 0, .cross = true, .expected = PredictorType::PREDICTOR_NONE},
      {.delta = 1, .cross = false, .expected = PredictorType::PREDICTOR_DELTA1},
      {.delta = 1, .cross = true, .expected = PredictorType::PREDICTOR_UNKNOWN},
      {.delta = 2,
       .cross = false,
       .expected = PredictorType::PREDICTOR_UNKNOWN},
      {.delta = 2,
       .cross = true,
       .expected = PredictorType::PREDICTOR_ROWS_COLS},
      {.delta = 3,
       .cross = false,
       .expected = PredictorType::PREDICTOR_UNKNOWN},
      {.delta = 3, .cross = true, .expected = PredictorType::PREDICTOR_UNKNOWN},
  };

  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected, Predictor::fromDeltaAndCross(tc.delta, tc.cross))
        << "delta=" << tc.delta << " cross=" << tc.cross;
  }
}

}  // namespace
}  // namespace lerc
