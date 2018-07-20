// Copyright 2018 Google Inc. All Rights Reserved.
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
// Test UriQuery.c
//
// A = ASCII

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "gmock.h"
#include "gtest/gtest.h"
#include "uriparser/Uri.h"
#include "uriparser/UriBase.h"

namespace {

typedef std::vector<std::pair<string, string>> Queries;

// Convert a linked list of key/value pairs to a vector of string std::pairs.
Queries ToVector(UriQueryListA *query_list) {
  Queries result;
  if (query_list == nullptr) return result;
  for (UriQueryListA *entry = query_list; entry != nullptr;
       entry = entry->next) {
    result.push_back(make_pair(entry->key, entry->value));
  }
  uriFreeQueryListA(query_list);
  return result;
}

UriQueryListA *MakeQueryListEntry(const string &key, const string &value) {
  UriQueryListA *query_list =
      static_cast<UriQueryListA *>(malloc(sizeof(UriQueryListA)));
  query_list->key = strdup(key.c_str());
  query_list->value = strdup(value.c_str());
  query_list->next = nullptr;

  return query_list;
}

UriQueryListA *MakeQueryList(const Queries &queries) {
  CHECK(!queries.empty());
  UriQueryListA *query_list = nullptr;
  UriQueryListA *current = nullptr;
  for (const auto &query : queries) {
    const string key = query.first;
    const string value = query.second;
    if (query_list == nullptr) {
      query_list = MakeQueryListEntry(key, value);
      current = query_list;
      continue;
    }
    UriQueryListA *next = MakeQueryListEntry(key, value);
    current->next = next;
  }
  return query_list;
}

TEST(UriComposeQueryCharsRequiredATest, QueryListNullptr) {
  UriQueryListA *query_list = nullptr;
  int chars_required = -1;
  EXPECT_EQ(URI_ERROR_NULL,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  EXPECT_EQ(-1, chars_required);
}

TEST(UriComposeQueryCharsRequiredATest, CharsRequiredNullptr) {
  const Queries queries{{"a", "b"}};
  auto query_list = MakeQueryList(queries);
  int *chars_required_ptr = nullptr;
  EXPECT_EQ(URI_ERROR_NULL,
            uriComposeQueryCharsRequiredA(query_list, chars_required_ptr));
  uriFreeQueryListA(query_list);
}

TEST(UriComposeQueryCharsRequiredATest, One) {
  const Queries queries{{"a", "b"}};
  auto query_list = MakeQueryList(queries);
  int chars_required = -1;
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(14, chars_required);
}

TEST(UriComposeQueryCharsRequiredATest, OneMultiChar) {
  const Queries queries{{"ab", "cd"}};
  auto query_list = MakeQueryList(queries);
  int chars_required = -1;
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(26, chars_required);
}

TEST(UriComposeQueryCharsRequiredATest, Space) {
  const Queries queries{{"a", "b "}};
  auto query_list = MakeQueryList(queries);
  int chars_required = -1;
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(20, chars_required);
}

TEST(UriComposeQueryCharsRequiredATest, Plus) {
  const Queries queries{{"a", "b+"}};
  auto query_list = MakeQueryList(queries);
  int chars_required = -1;
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(20, chars_required);
}

TEST(UriComposeQueryCharsRequiredATest, Newline) {
  const Queries queries{{"a", "\n"}};
  auto query_list = MakeQueryList(queries);
  int chars_required = -1;
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(14, chars_required);
}

TEST(UriComposeQueryCharsRequiredATest, NormLineBreak) {
  const Queries queries{{"a", "b%0D%0Ac"}};
  auto query_list = MakeQueryList(queries);
  int chars_required = -1;
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(56, chars_required);
}

TEST(UriComposeQueryCharsRequiredATest, KeyMissing) {
  const Queries queries{{"", "b"}};
  auto query_list = MakeQueryList(queries);
  int chars_required = -1;
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(8, chars_required);
}

TEST(UriComposeQueryCharsRequiredATest, ValueMissing) {
  const Queries queries{{"a", ""}};
  auto query_list = MakeQueryList(queries);
  int chars_required = -1;
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(8, chars_required);
}

TEST(UriComposeQueryCharsRequiredATest, Two) {
  const Queries queries{{"a", "b"}, {"c", "d"}};
  auto query_list = MakeQueryList(queries);
  int chars_required = -1;
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryCharsRequiredA(query_list, &chars_required));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(28, chars_required);
}

// TODO(schwehr): uriComposeQueryCharsRequiredExA

TEST(UriComposeQueryATest, DestNullptr) {
  Queries queries{{"a", "b"}};
  int chars_written = -1;
  auto query_list = MakeQueryList(queries);
  // Pretend there is space.
  constexpr int output_size = 100;
  EXPECT_EQ(URI_ERROR_NULL,
            uriComposeQueryA(nullptr, query_list, output_size, &chars_written));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(-1, chars_written);
}

TEST(UriComposeQueryATest, QueryListNullptr) {
  std::vector<char> buf(100, 0);
  int chars_written = -1;
  UriQueryListA *query_list = nullptr;
  EXPECT_EQ(URI_ERROR_NULL,
            uriComposeQueryA(&buf[0], query_list, buf.size(), &chars_written));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(-1, chars_written);
  EXPECT_STREQ("", &buf[0]);
}

TEST(UriComposeQueryATest, One) {
  const Queries queries{{"a", "b"}};
  std::vector<char> buf(100, 0);
  int chars_written = -1;
  auto query_list = MakeQueryList(queries);
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryA(&buf[0], query_list, buf.size(), &chars_written));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(4, chars_written);
  EXPECT_STREQ("a=b", &buf[0]);
}

TEST(UriComposeQueryATest, OneKeyEmpty) {
  Queries queries{{"", "b"}};
  std::vector<char> buf(100, 0);
  int chars_written = -1;
  auto query_list = MakeQueryList(queries);
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryA(&buf[0], query_list, buf.size(), &chars_written));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(3, chars_written);
  EXPECT_STREQ("=b", &buf[0]);
}

TEST(UriComposeQueryATest, OneValueEmpty) {
  Queries queries{{"d", ""}};
  std::vector<char> buf(100, 0);
  int chars_written = -1;
  auto query_list = MakeQueryList(queries);
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryA(&buf[0], query_list, buf.size(), &chars_written));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(3, chars_written);
  EXPECT_STREQ("d=", &buf[0]);
}

TEST(UriComposeQueryATest, Two) {
  Queries queries{{"a", "b"}, {"c", "d"}};
  std::vector<char> buf(100, 0);
  int chars_written = -1;
  auto query_list = MakeQueryList(queries);
  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryA(&buf[0], query_list, buf.size(), &chars_written));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(8, chars_written);
  EXPECT_STREQ("a=b&c=d", &buf[0]);
}

TEST(UriComposeQueryATest, UriErrorOutputTooLarge_NoSpace) {
  Queries queries{{"a", "b"}};
  std::vector<char> buf(100, 0);
  int chars_written = -1;
  auto query_list = MakeQueryList(queries);
  // Give uriComposeQueryA no space to work with
  constexpr int output_size = 0;
  EXPECT_EQ(URI_ERROR_OUTPUT_TOO_LARGE,
            uriComposeQueryA(&buf[0], query_list, output_size, &chars_written));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(-1, chars_written);
  EXPECT_STREQ("", &buf[0]);
}

TEST(UriComposeQueryATest, UriErrorOutputTooLarge_PartialSpace) {
  Queries queries{{"a", "b"}, {"c", "d"}};
  std::vector<char> buf(100, 0);
  int chars_written = -1;
  auto query_list = MakeQueryList(queries);
  constexpr char expected_output[] = "a=b";
  const int output_size = sizeof(expected_output);
  EXPECT_EQ(URI_ERROR_OUTPUT_TOO_LARGE,
            uriComposeQueryA(&buf[0], query_list, output_size, &chars_written));
  uriFreeQueryListA(query_list);
  EXPECT_EQ(-1, chars_written);
  EXPECT_STREQ("", &buf[0]);
}

TEST(UriComposeQueryExATest, Newline) {
  Queries queries{{"a\n", "b\r"}, {"c\r\n", "d\n\r"}};
  std::vector<char> buf(100, 0);
  int chars_written = -1;
  auto query_list = MakeQueryList(queries);
  constexpr UriBool space_to_plus = URI_FALSE;
  constexpr UriBool normalize_breaks_true = URI_TRUE;

  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryExA(&buf[0], query_list, buf.size(), &chars_written,
                               space_to_plus, normalize_breaks_true));
  const char expected_true[] = "a%0D%0A=b%0D%0A&c%0D%0A=d%0D%0A%0D%0A";
  EXPECT_EQ(sizeof(expected_true), chars_written);
  EXPECT_STREQ(expected_true, &buf[0]);

  constexpr UriBool normalize_breaks_false = URI_FALSE;

  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryExA(&buf[0], query_list, buf.size(), &chars_written,
                               space_to_plus, normalize_breaks_false));
  const char expected_false[] = "a%0A=b%0D&c%0D%0A=d%0A%0D";
  EXPECT_EQ(sizeof(expected_false), chars_written);
  EXPECT_STREQ(expected_false, &buf[0]);

  uriFreeQueryListA(query_list);
}

TEST(UriComposeQueryExATest, Space) {
  Queries queries{{" a b ", "\tc"}};
  std::vector<char> buf(100, 0);
  int chars_written = -1;
  auto query_list = MakeQueryList(queries);
  constexpr UriBool space_to_plus_true = URI_TRUE;
  constexpr UriBool normalize_breaks = URI_FALSE;

  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryExA(&buf[0], query_list, buf.size(), &chars_written,
                               space_to_plus_true, normalize_breaks));
  const char expected_true[] = "+a+b+=%09c";
  EXPECT_EQ(sizeof(expected_true), chars_written);
  EXPECT_STREQ(expected_true, &buf[0]);

  constexpr UriBool space_to_plus_false = URI_FALSE;

  EXPECT_EQ(URI_SUCCESS,
            uriComposeQueryExA(&buf[0], query_list, buf.size(), &chars_written,
                               space_to_plus_false, normalize_breaks));
  const char expected_false[] = "%20a%20b%20=%09c";
  EXPECT_EQ(sizeof(expected_false), chars_written);
  EXPECT_STREQ(expected_false, &buf[0]);

  uriFreeQueryListA(query_list);
}

TEST(UriComposeQueryMallocATest, DestNullptr) {
  Queries queries{{"a", "b"}};
  auto query_list = MakeQueryList(queries);

  EXPECT_EQ(URI_ERROR_NULL, uriComposeQueryMallocA(nullptr, query_list));
  uriFreeQueryListA(query_list);
}

TEST(UriComposeQueryMallocATest, One) {
  Queries queries{{"a", "b"}};
  auto query_list = MakeQueryList(queries);

  char *dest = nullptr;

  EXPECT_EQ(URI_SUCCESS, uriComposeQueryMallocA(&dest, query_list));
  uriFreeQueryListA(query_list);
  const char expected[] = "a=b";
  EXPECT_STREQ(expected, dest);

  free(dest);
}

TEST(UriFreeQueryListATest, Nullptr) {
  // Called to make sure this doesn't crash.
  uriFreeQueryListA(nullptr);
}

TEST(UriDissectQueryMallocATest, NullQueryList) {
  int item_count = -1;
  constexpr char query[] = "";

  EXPECT_EQ(URI_ERROR_NULL, uriDissectQueryMallocA(nullptr, &item_count, query,
                                                   query + sizeof(query)));
  // item_count was never set.
  EXPECT_EQ(-1, item_count);
}

TEST(UriDissectQueryMallocATest, NullFirst) {
  UriQueryListA *query_list = nullptr;
  int item_count = -1;
  constexpr char query[] = "";

  EXPECT_EQ(URI_ERROR_NULL,
            uriDissectQueryMallocA(&query_list, &item_count, nullptr,
                                   query + sizeof(query)));
  EXPECT_EQ(-1, item_count);
  EXPECT_EQ(nullptr, query_list);
}

TEST(UriDissectQueryMallocATest, NullAfterLast) {
  UriQueryListA *query_list = nullptr;
  int item_count = -1;
  constexpr char query[] = "";

  EXPECT_EQ(URI_ERROR_NULL,
            uriDissectQueryMallocA(&query_list, &item_count, query, nullptr));
  EXPECT_EQ(-1, item_count);
  EXPECT_EQ(nullptr, query_list);
}

TEST(UriDissectQueryMallocATest, FirstAfterLastSwapped) {
  UriQueryListA *query_list = nullptr;
  int item_count = -1;
  constexpr char query[] = "a=b";

  EXPECT_EQ(URI_ERROR_RANGE_INVALID,
            uriDissectQueryMallocA(&query_list, &item_count,
                                   query + sizeof(query), query));
  EXPECT_EQ(-1, item_count);
  EXPECT_EQ(nullptr, query_list);
}

TEST(UriDissectQueryMallocATest, Empty) {
  UriQueryListA *query_list = nullptr;
  int item_count = 0;
  constexpr char query[] = "";

  EXPECT_EQ(URI_SUCCESS, uriDissectQueryMallocA(&query_list, &item_count, query,
                                                query + sizeof(query)));
  EXPECT_EQ(1, item_count);
  EXPECT_STREQ("", query_list->key);
  EXPECT_EQ(nullptr, query_list->value);
  EXPECT_EQ(nullptr, query_list->next);
  uriFreeQueryListA(query_list);
}

TEST(UriDissectQueryMallocATest, OnePair) {
  UriQueryListA *query_list = nullptr;
  int item_count = -1;
  constexpr char query[] = "a=b";

  EXPECT_EQ(URI_SUCCESS, uriDissectQueryMallocA(&query_list, &item_count, query,
                                                query + sizeof(query)));
  EXPECT_EQ(1, item_count);
  auto queries = ToVector(query_list);
  EXPECT_THAT(queries, testing::ElementsAre(std::make_pair("a", "b")));
}

TEST(UriDissectQueryMallocATest, OnePairWithSpaces) {
  UriQueryListA *query_list = nullptr;
  int item_count = -1;
  constexpr char query[] = " a = b ";

  EXPECT_EQ(URI_SUCCESS, uriDissectQueryMallocA(&query_list, &item_count, query,
                                                query + sizeof(query)));
  EXPECT_EQ(1, item_count);
  auto queries = ToVector(query_list);
  EXPECT_THAT(queries, testing::ElementsAre(std::make_pair(" a ", " b ")));
}

TEST(UriDissectQueryMallocATest, NullCounterParameter) {
  UriQueryListA *query_list = nullptr;
  constexpr char query[] = "a=b";

  EXPECT_EQ(URI_SUCCESS, uriDissectQueryMallocA(&query_list, nullptr, query,
                                                query + sizeof(query)));
  auto queries = ToVector(query_list);
  EXPECT_THAT(queries, testing::ElementsAre(std::make_pair("a", "b")));
}

TEST(UriDissectQueryMallocATest, MissingKey) {
  UriQueryListA *query_list = nullptr;
  int item_count = -1;
  constexpr char query[] = "=c";

  EXPECT_EQ(URI_SUCCESS, uriDissectQueryMallocA(&query_list, &item_count, query,
                                                query + sizeof(query)));
  EXPECT_EQ(1, item_count);
  auto queries = ToVector(query_list);
  EXPECT_THAT(queries, testing::ElementsAre(std::make_pair("", "c")));
}

TEST(UriDissectQueryMallocATest, MissingValue) {
  UriQueryListA *query_list = nullptr;
  int item_count = -1;
  constexpr char query[] = "d=";

  EXPECT_EQ(URI_SUCCESS, uriDissectQueryMallocA(&query_list, &item_count, query,
                                                query + sizeof(query)));
  EXPECT_EQ(1, item_count);
  auto queries = ToVector(query_list);
  EXPECT_THAT(queries, testing::ElementsAre(std::make_pair("d", "")));
}

TEST(UriDissectQueryMallocATest, MoreThanOneKeyValuePair) {
  UriQueryListA *query_list = nullptr;
  int item_count = 0;
  constexpr char query[] = "one=ONE&two=TWO";

  EXPECT_EQ(URI_SUCCESS, uriDissectQueryMallocA(&query_list, &item_count, query,
                                                query + sizeof(query)));
  EXPECT_EQ(2, item_count);
  auto queries = ToVector(query_list);
  EXPECT_THAT(queries, testing::ElementsAre(std::make_pair("one", "ONE"),
                                            std::make_pair("two", "TWO")));
}

TEST(UriDissectQueryMallocATest, MoreThanOneKeyValuePairWithMissingSides) {
  UriQueryListA *query_list = nullptr;
  int item_count = 0;
  constexpr char query[] = "a=b&=c&d=&e=f";

  EXPECT_EQ(URI_SUCCESS, uriDissectQueryMallocA(&query_list, &item_count, query,
                                                query + sizeof(query)));
  EXPECT_EQ(4, item_count);
  auto queries = ToVector(query_list);
  EXPECT_THAT(queries, testing::ElementsAre(
                           std::make_pair("a", "b"), std::make_pair("", "c"),
                           std::make_pair("d", ""), std::make_pair("e", "f")));
}

}  // namespace
