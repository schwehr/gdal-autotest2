// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for third_party/geos/src/io/StringTokenizer.cpp.
//
// google3 notes:
// - StringTokenizer stores a const std::string& reference without copying;
//   keep the input std::string alive longer than the StringTokenizer.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/StringTokenizer.cpp

#include "third_party/geos/include/geos/io/StringTokenizer.h"

#include <cmath>
#include <string>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::io::StringTokenizer;

TEST(StringTokenizerTest, EmptyAndWhitespaceOnlyReturnEof) {
  const std::string empty;
  StringTokenizer empty_tok(empty);
  EXPECT_EQ(empty_tok.peekNextToken(), StringTokenizer::TT_EOF);
  EXPECT_EQ(empty_tok.nextToken(), StringTokenizer::TT_EOF);
  EXPECT_DOUBLE_EQ(empty_tok.getNVal(), 0.0);
  EXPECT_EQ(empty_tok.getSVal(), "");

  const std::string spaces = "  \t\r\n  ";
  StringTokenizer space_tok(spaces);
  EXPECT_EQ(space_tok.peekNextToken(), StringTokenizer::TT_EOF);
  EXPECT_EQ(space_tok.nextToken(), StringTokenizer::TT_EOF);
  // Repeating nextToken() at end hits the end-of-string fast path.
  EXPECT_EQ(space_tok.nextToken(), StringTokenizer::TT_EOF);
}

TEST(StringTokenizerTest, PunctuationAndPeekSemantics) {
  const std::string input = " ( , ) ";
  StringTokenizer tok(input);

  EXPECT_EQ(tok.peekNextToken(), '(');
  EXPECT_EQ(tok.nextToken(), '(');
  EXPECT_EQ(tok.peekNextToken(), ',');
  EXPECT_EQ(tok.nextToken(), ',');
  EXPECT_EQ(tok.peekNextToken(), ')');
  EXPECT_EQ(tok.nextToken(), ')');
  EXPECT_EQ(tok.peekNextToken(), StringTokenizer::TT_EOF);
  EXPECT_EQ(tok.nextToken(), StringTokenizer::TT_EOF);
}

TEST(StringTokenizerTest, WordsNumbersAndValueResetting) {
  const std::string input = "POLYGON((10 -2.5e1, 0 0)) trailing";
  StringTokenizer tok(input);

  EXPECT_EQ(tok.peekNextToken(), StringTokenizer::TT_WORD);
  EXPECT_EQ(tok.getSVal(), "POLYGON");
  EXPECT_EQ(tok.nextToken(), StringTokenizer::TT_WORD);
  EXPECT_EQ(tok.getSVal(), "POLYGON");
  EXPECT_DOUBLE_EQ(tok.getNVal(), 0.0);

  EXPECT_EQ(tok.nextToken(), '(');
  EXPECT_EQ(tok.nextToken(), '(');

  EXPECT_EQ(tok.peekNextToken(), StringTokenizer::TT_NUMBER);
  EXPECT_DOUBLE_EQ(tok.getNVal(), 10.0);
  EXPECT_EQ(tok.nextToken(), StringTokenizer::TT_NUMBER);
  EXPECT_DOUBLE_EQ(tok.getNVal(), 10.0);
  EXPECT_EQ(tok.getSVal(), "");

  EXPECT_EQ(tok.nextToken(), StringTokenizer::TT_NUMBER);
  EXPECT_DOUBLE_EQ(tok.getNVal(), -25.0);

  EXPECT_EQ(tok.nextToken(), ',');
  EXPECT_EQ(tok.nextToken(), StringTokenizer::TT_NUMBER);
  EXPECT_DOUBLE_EQ(tok.getNVal(), 0.0);
  EXPECT_EQ(tok.nextToken(), StringTokenizer::TT_NUMBER);
  EXPECT_DOUBLE_EQ(tok.getNVal(), 0.0);

  EXPECT_EQ(tok.nextToken(), ')');
  EXPECT_EQ(tok.nextToken(), ')');

  // Last word reaches end of string without a delimiter. Note peekNextToken()
  // still skips leading whitespace to classify the token type as TT_WORD.
  EXPECT_EQ(tok.peekNextToken(), StringTokenizer::TT_WORD);
  EXPECT_EQ(tok.nextToken(), StringTokenizer::TT_WORD);
  EXPECT_EQ(tok.getSVal(), "trailing");
  EXPECT_EQ(tok.nextToken(), StringTokenizer::TT_EOF);
}

TEST(StringTokenizerTest, TrailingNumberAtEndOfInputAndPartialNumericWord) {
  const std::string trailing_num = "  42.25";
  StringTokenizer num_tok(trailing_num);
  EXPECT_EQ(num_tok.peekNextToken(), StringTokenizer::TT_NUMBER);
  EXPECT_DOUBLE_EQ(num_tok.getNVal(), 42.25);
  EXPECT_EQ(num_tok.nextToken(), StringTokenizer::TT_NUMBER);
  EXPECT_DOUBLE_EQ(num_tok.getNVal(), 42.25);

  const std::string mixed = "12abc";
  StringTokenizer mixed_tok(mixed);
  EXPECT_EQ(mixed_tok.peekNextToken(), StringTokenizer::TT_WORD);
  EXPECT_EQ(mixed_tok.getSVal(), "12abc");
  EXPECT_EQ(mixed_tok.nextToken(), StringTokenizer::TT_WORD);
  EXPECT_EQ(mixed_tok.getSVal(), "12abc");

  const std::string nan_tok_str = "NaN";
  StringTokenizer nan_tok(nan_tok_str);
  EXPECT_EQ(nan_tok.nextToken(), StringTokenizer::TT_NUMBER);
  EXPECT_TRUE(std::isnan(nan_tok.getNVal()));
}

}  // namespace
}  // namespace geos_google
