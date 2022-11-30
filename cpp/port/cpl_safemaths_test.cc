//
// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// This is a complete rewrite of a file licensed as follows:
//
// Copyright (c) 2006, Mateusz Loskot <mateusz@loskot.net>
// Copyright (c) 2008-2012, Even Rouault <even dot rouault at mines-paris dot
// org>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Library General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with this library; if not, write to the
// Free Software Foundation, Inc., 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.

// Tests for GDAL's Safe Math API.
//
// See also:
//   Test 25 and 26:
//     https://github.com/OSGeo/gdal/blob/master/autotest/cpp/test_cpl.cpp

#include "port/cpl_port.h"
#include "port/cpl_safemaths.hpp"

#include <limits>

#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"

namespace autotest2 {
namespace {

// Rewrite of autotest's test 25.
TEST(SafeMathsTest, TestInt) {
  EXPECT_EQ((CPLSM(-2) + CPLSM(3)).v(), 1);
  EXPECT_EQ((CPLSM(-2) + CPLSM(1)).v(), -1);
  EXPECT_EQ((CPLSM(-2) + CPLSM(-1)).v(), -3);
  EXPECT_EQ((CPLSM(2) + CPLSM(-3)).v(), -1);
  EXPECT_EQ((CPLSM(2) + CPLSM(-1)).v(), 1);
  EXPECT_EQ((CPLSM(2) + CPLSM(1)).v(), 3);
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::max() - 1) + CPLSM(1)).v(),
            std::numeric_limits<int>::max());
  EXPECT_EQ((CPLSM(1) + CPLSM(std::numeric_limits<int>::max() - 1)).v(),
            std::numeric_limits<int>::max());
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::max()) + CPLSM(-1)).v(),
            std::numeric_limits<int>::max() - 1);
  EXPECT_EQ((CPLSM(-1) + CPLSM(std::numeric_limits<int>::max())).v(),
            std::numeric_limits<int>::max() - 1);
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::min() + 1) + CPLSM(-1)).v(),
            std::numeric_limits<int>::min());
  EXPECT_EQ((CPLSM(-1) + CPLSM(std::numeric_limits<int>::min() + 1)).v(),
            std::numeric_limits<int>::min());

  EXPECT_THROW((CPLSM(std::numeric_limits<int>::max()) + CPLSM(1)).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(1) + CPLSM(std::numeric_limits<int>::max())).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(std::numeric_limits<int>::min()) + CPLSM(-1)).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(-1) + CPLSM(std::numeric_limits<int>::min())).v(),
               CPLSafeIntOverflow);

  EXPECT_EQ((CPLSM(-2) - CPLSM(1)).v(), -3);
  EXPECT_EQ((CPLSM(-2) - CPLSM(-1)).v(), -1);
  EXPECT_EQ((CPLSM(-2) - CPLSM(-3)).v(), 1);
  EXPECT_EQ((CPLSM(2) - CPLSM(-1)).v(), 3);
  EXPECT_EQ((CPLSM(2) - CPLSM(1)).v(), 1);
  EXPECT_EQ((CPLSM(2) - CPLSM(3)).v(), -1);
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::max()) - CPLSM(1)).v(),
            std::numeric_limits<int>::max() - 1);
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::min() + 1) - CPLSM(1)).v(),
            std::numeric_limits<int>::min());
  EXPECT_EQ((CPLSM(0) - CPLSM(std::numeric_limits<int>::min() + 1)).v(),
            std::numeric_limits<int>::max());
  EXPECT_EQ((CPLSM(0) - CPLSM(std::numeric_limits<int>::max())).v(),
            -std::numeric_limits<int>::max());
  EXPECT_THROW((CPLSM(std::numeric_limits<int>::min()) - CPLSM(1)).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(0) - CPLSM(std::numeric_limits<int>::min())).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(std::numeric_limits<int>::min()) - CPLSM(1)).v(),
               CPLSafeIntOverflow);

  EXPECT_EQ((CPLSM(std::numeric_limits<int>::min() + 1) * CPLSM(-1)).v(),
            std::numeric_limits<int>::max());
  EXPECT_EQ((CPLSM(-1) * CPLSM(std::numeric_limits<int>::min() + 1)).v(),
            std::numeric_limits<int>::max());
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::min()) * CPLSM(1)).v(),
            std::numeric_limits<int>::min());
  EXPECT_EQ((CPLSM(1) * CPLSM(std::numeric_limits<int>::min())).v(),
            std::numeric_limits<int>::min());
  EXPECT_EQ((CPLSM(1) * CPLSM(std::numeric_limits<int>::max())).v(),
            std::numeric_limits<int>::max());
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::min() / 2) * CPLSM(2)).v(),
            std::numeric_limits<int>::min());
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::max() / 2) * CPLSM(2)).v(),
            std::numeric_limits<int>::max() - 1);
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::max() / 2 + 1) * CPLSM(-2)).v(),
            std::numeric_limits<int>::min());
  EXPECT_EQ((CPLSM(0) * CPLSM(std::numeric_limits<int>::min())).v(), 0);
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::min()) * CPLSM(0)).v(), 0);
  EXPECT_EQ((CPLSM(0) * CPLSM(std::numeric_limits<int>::max())).v(), 0);
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::max()) * CPLSM(0)).v(), 0);
  EXPECT_THROW((CPLSM(std::numeric_limits<int>::max() / 2 + 1) * CPLSM(2)).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(2) * CPLSM(std::numeric_limits<int>::max() / 2 + 1)).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(std::numeric_limits<int>::min()) * CPLSM(-1)).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(std::numeric_limits<int>::min()) * CPLSM(2)).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(2) * CPLSM(std::numeric_limits<int>::min())).v(),
               CPLSafeIntOverflow);

  EXPECT_EQ((CPLSM(4) / CPLSM(2)).v(), 2);
  EXPECT_EQ((CPLSM(4) / CPLSM(-2)).v(), -2);
  EXPECT_EQ((CPLSM(-4) / CPLSM(2)).v(), -2);
  EXPECT_EQ((CPLSM(-4) / CPLSM(-2)).v(), 2);
  EXPECT_EQ((CPLSM(0) / CPLSM(2)).v(), 0);
  EXPECT_EQ((CPLSM(0) / CPLSM(-2)).v(), 0);
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::max()) / CPLSM(1)).v(),
            std::numeric_limits<int>::max());
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::max()) / CPLSM(-1)).v(),
            -std::numeric_limits<int>::max());
  EXPECT_EQ((CPLSM(std::numeric_limits<int>::min()) / CPLSM(1)).v(),
            std::numeric_limits<int>::min());
  EXPECT_THROW((CPLSM(-1) * CPLSM(std::numeric_limits<int>::min())).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(std::numeric_limits<int>::min()) / CPLSM(-1)).v(),
               CPLSafeIntOverflow);
  EXPECT_THROW((CPLSM(1) / CPLSM(0)).v(), CPLSafeIntOverflowDivisionByZero);
}

// Rewrite of autotest's test 26.
TEST(SafeMathsTest, TestUnsignedInt) {
  EXPECT_EQ(CPLSM_TO_UNSIGNED(1).v(), 1U);
  EXPECT_THROW(CPLSM_TO_UNSIGNED(-1), CPLSafeIntOverflow);
  EXPECT_EQ((CPLSM(2U) + CPLSM(3U)).v(), 5U);
  EXPECT_EQ(
      (CPLSM(std::numeric_limits<unsigned int>::max() - 1) + CPLSM(1U)).v(),
      std::numeric_limits<unsigned int>::max());
  EXPECT_THROW(
      (CPLSM(std::numeric_limits<unsigned int>::max()) + CPLSM(1U)).v(),
      CPLSafeIntOverflow);

  EXPECT_EQ((CPLSM(4U) - CPLSM(3U)).v(), 1U);
  EXPECT_EQ((CPLSM(4U) - CPLSM(4U)).v(), 0U);
  EXPECT_EQ((CPLSM(std::numeric_limits<unsigned int>::max()) - CPLSM(1U)).v(),
            std::numeric_limits<unsigned int>::max() - 1);
  EXPECT_THROW((CPLSM(4U) - CPLSM(5U)).v(), CPLSafeIntOverflow);

  EXPECT_EQ((CPLSM(0U) * CPLSM(std::numeric_limits<unsigned int>::max())).v(),
            0U);
  EXPECT_EQ((CPLSM(std::numeric_limits<unsigned int>::max()) * CPLSM(0U)).v(),
            0U);
  EXPECT_EQ((CPLSM(std::numeric_limits<unsigned int>::max()) * CPLSM(1U)).v(),
            std::numeric_limits<unsigned int>::max());
  EXPECT_EQ((CPLSM(1U) * CPLSM(std::numeric_limits<unsigned int>::max())).v(),
            std::numeric_limits<unsigned int>::max());
  EXPECT_THROW(
      (CPLSM(std::numeric_limits<unsigned int>::max()) * CPLSM(2U)).v(),
      CPLSafeIntOverflow);
  EXPECT_THROW(
      (CPLSM(2U) * CPLSM(std::numeric_limits<unsigned int>::max())).v(),
      CPLSafeIntOverflow);

  EXPECT_EQ((CPLSM(4U) / CPLSM(2U)).v(), 2U);
  EXPECT_EQ((CPLSM(std::numeric_limits<unsigned int>::max()) / CPLSM(1U)).v(),
            std::numeric_limits<unsigned int>::max());
  EXPECT_THROW((CPLSM(1U) / CPLSM(0U)).v(), CPLSafeIntOverflowDivisionByZero);
}

}  // namespace
}  // namespace autotest2
