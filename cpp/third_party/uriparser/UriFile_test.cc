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
// Test UriFile.c
//
// A = ASCII
//
// Many of these test examples show very strange behavior.

#include <stddef.h>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "uriparser/Uri.h"
#include "uriparser/UriBase.h"

namespace {

TEST(UriUnixFilenameToUriStringA, Nullptr) {
  const char uri[] = "/bin/bash";
  std::vector<char> buf(10);

  ASSERT_EQ(URI_ERROR_NULL, uriUnixFilenameToUriStringA(nullptr, &buf[0]));
  ASSERT_EQ(URI_ERROR_NULL, uriUnixFilenameToUriStringA(uri, nullptr));
  ASSERT_EQ(URI_ERROR_NULL, uriUnixFilenameToUriStringA(nullptr, nullptr));
}

size_t UnixFilenameToUriStringSize(const char *filename) {
  // "file://" plus each character could be expanded to a 3 character percent
  // representation and finishing up with a NUL termination sentinal.
  return 7 + 3 * strlen(filename) + 1;
}

void CheckUriUnixFilenameToUriStringA(const char *filename, const char *uri) {
  std::vector<char> buf(UnixFilenameToUriStringSize(filename));
  ASSERT_EQ(URI_SUCCESS, uriUnixFilenameToUriStringA(filename, &buf[0]));
  EXPECT_STREQ(uri, &buf[0]) << "For: \"" << filename << "\"";
}

TEST(UriUnixFilenameToUriStringA, WhiteSpace) {
  CheckUriUnixFilenameToUriStringA("", "");
  CheckUriUnixFilenameToUriStringA(" ", "%20");
  CheckUriUnixFilenameToUriStringA("a b", "a%20b");
  CheckUriUnixFilenameToUriStringA("\t", "%09");
  CheckUriUnixFilenameToUriStringA("\v", "%0B");
  CheckUriUnixFilenameToUriStringA("\n", "%0A");
  CheckUriUnixFilenameToUriStringA("\r", "%0D");
  CheckUriUnixFilenameToUriStringA("\r\n", "%0D%0A");
}

TEST(UriUnixFilenameToUriStringA, Slashes) {
  CheckUriUnixFilenameToUriStringA("/", "file:///");
  CheckUriUnixFilenameToUriStringA("/a", "file:///a");
  CheckUriUnixFilenameToUriStringA("/b/", "file:///b/");
  CheckUriUnixFilenameToUriStringA("c", "c");
  CheckUriUnixFilenameToUriStringA("d/e", "d/e");
  CheckUriUnixFilenameToUriStringA("f/", "f/");

  CheckUriUnixFilenameToUriStringA("//", "file:////");

  // DOS style backslash.
  CheckUriUnixFilenameToUriStringA("\\", "%5C");
}

TEST(UriUnixFilenameToUriStringA, Dots) {
  // "." and ".." are not interpreted in any way.
  CheckUriUnixFilenameToUriStringA(".", ".");
  CheckUriUnixFilenameToUriStringA("..", "..");
  CheckUriUnixFilenameToUriStringA("...", "...");
  CheckUriUnixFilenameToUriStringA("/.", "file:///.");
  CheckUriUnixFilenameToUriStringA("/./", "file:///./");
  CheckUriUnixFilenameToUriStringA("/..", "file:///..");
  CheckUriUnixFilenameToUriStringA("/../", "file:///../");
  CheckUriUnixFilenameToUriStringA("/../a", "file:///../a");
  CheckUriUnixFilenameToUriStringA("/../.b", "file:///../.b");
  CheckUriUnixFilenameToUriStringA("../.c", "../.c");
  CheckUriUnixFilenameToUriStringA("/.././d.e", "file:///.././d.e");
}

TEST(UriUnixFilenameToUriStringA, WrongWay) {
  CheckUriUnixFilenameToUriStringA("file://", "file%3A//");
}

size_t UriStringToUnixFilenameSize(const char *uri) {
  // Skip removing the 7.
  return strlen(uri) + 1;
}

TEST(UriUriStringToUnixFilenameA, Nullptr) {
  const char uri[] = "/a/b";
  std::vector<char> buf(UriStringToUnixFilenameSize(uri));
  EXPECT_EQ(URI_ERROR_NULL, uriUriStringToUnixFilenameA(nullptr, &buf[0]));
  EXPECT_EQ(URI_ERROR_NULL, uriUriStringToUnixFilenameA(uri, nullptr));
  EXPECT_EQ(URI_ERROR_NULL, uriUriStringToUnixFilenameA(nullptr, nullptr));
}

void CheckUriUriStringToUnixFilenameA(const char *uri, const char *filename) {
  std::vector<char> buf(UriStringToUnixFilenameSize(uri));
  ASSERT_EQ(URI_SUCCESS, uriUriStringToUnixFilenameA(uri, &buf[0]));
  EXPECT_STREQ(filename, &buf[0]) << uri;
}

TEST(UriUriStringToUnixFilenameA, NoFile) {
  CheckUriUriStringToUnixFilenameA("", "");
  CheckUriUriStringToUnixFilenameA("a", "a");
  CheckUriUriStringToUnixFilenameA("%0A", "\n");
}

TEST(UriUriStringToUnixFilenameA, WithFile) {
  CheckUriUriStringToUnixFilenameA("file://", "");
  CheckUriUriStringToUnixFilenameA("file:///", "/");
  CheckUriUriStringToUnixFilenameA("file://a", "a");
  CheckUriUriStringToUnixFilenameA("file:///b", "/b");
  CheckUriUriStringToUnixFilenameA("file:///c/", "/c/");
  CheckUriUriStringToUnixFilenameA("file:///d/e", "/d/e");
}

TEST(UriUriStringToUnixFilenameA, Dots) {
  CheckUriUriStringToUnixFilenameA("file://.", ".");
  CheckUriUriStringToUnixFilenameA("file://..", "..");
  CheckUriUriStringToUnixFilenameA("file:///.", "/.");
  CheckUriUriStringToUnixFilenameA("file:///..", "/..");
}

TEST(UriWindowsFilenameToUriStringA, Nullptr) {
  const char uri[] = "c:/foo";
  std::vector<char> buf(10);

  ASSERT_EQ(URI_ERROR_NULL, uriWindowsFilenameToUriStringA(nullptr, &buf[0]));
  ASSERT_EQ(URI_ERROR_NULL, uriWindowsFilenameToUriStringA(uri, nullptr));
  ASSERT_EQ(URI_ERROR_NULL, uriWindowsFilenameToUriStringA(nullptr, nullptr));
}

size_t WindowsFilenameToUriStringSize(const char *filename) {
  // "file:///" plus each character could be expanded to a 3 character percent
  // representation and finishing up with a NUL termination sentinal.
  return 8 + 3 * strlen(filename) + 1;
}

void CheckUriWindowsFilenameToUriStringA(const char *filename,
                                         const char *uri) {
  std::vector<char> buf(WindowsFilenameToUriStringSize(filename));
  ASSERT_EQ(URI_SUCCESS, uriWindowsFilenameToUriStringA(filename, &buf[0]));
  EXPECT_STREQ(uri, &buf[0]) << "For: \"" << filename << "\"";
}

TEST(UriWindowsFilenameToUriStringA, WhiteSpace) {
  CheckUriWindowsFilenameToUriStringA("", "");
  CheckUriWindowsFilenameToUriStringA(" ", "%20");
  CheckUriWindowsFilenameToUriStringA("a b", "a%20b");
  CheckUriWindowsFilenameToUriStringA("\t", "%09");
  CheckUriWindowsFilenameToUriStringA("\v", "%0B");
  CheckUriWindowsFilenameToUriStringA("\n", "%0A");
  CheckUriWindowsFilenameToUriStringA("\r", "%0D");
  CheckUriWindowsFilenameToUriStringA("\r\n", "%0D%0A");
}

TEST(UriWindowsFilenameToUriStringA, Slashes) {
  // DOS style backslash.
  CheckUriWindowsFilenameToUriStringA("\\", "/");
  CheckUriWindowsFilenameToUriStringA("\\\\", "file://");  // ?
  CheckUriWindowsFilenameToUriStringA("a:\\", "file:///a:/");
  CheckUriWindowsFilenameToUriStringA("b:\\c", "file:///b:/c");

  CheckUriWindowsFilenameToUriStringA("a:\\\\", "file:///a://");

  // Unix
  CheckUriWindowsFilenameToUriStringA("/", "%2F");
  CheckUriWindowsFilenameToUriStringA("/a", "%2Fa");
  CheckUriWindowsFilenameToUriStringA("/b/", "%2Fb%2F");
  CheckUriWindowsFilenameToUriStringA("c", "c");
  CheckUriWindowsFilenameToUriStringA("d/e", "d%2Fe");
  CheckUriWindowsFilenameToUriStringA("f/", "f%2F");
}

size_t UriStringToWindowsFilenameSize(const char *uri) {
  // Skip removing the 5.
  return strlen(uri) + 1;
}

TEST(UriUriStringToWindowsFilenameA, Nullptr) {
  const char uri[] = "/a/b";
  std::vector<char> buf(UriStringToWindowsFilenameSize(uri));
  EXPECT_EQ(URI_ERROR_NULL, uriUriStringToWindowsFilenameA(nullptr, &buf[0]));
  EXPECT_EQ(URI_ERROR_NULL, uriUriStringToWindowsFilenameA(uri, nullptr));
  EXPECT_EQ(URI_ERROR_NULL, uriUriStringToWindowsFilenameA(nullptr, nullptr));
}

void CheckUriUriStringToWindowsFilenameA(const char *uri,
                                         const char *filename) {
  std::vector<char> buf(UriStringToWindowsFilenameSize(uri));
  ASSERT_EQ(URI_SUCCESS, uriUriStringToWindowsFilenameA(uri, &buf[0]));
  EXPECT_STREQ(filename, &buf[0]) << "For: \"" << uri << "\"";
}

TEST(UriUriStringToWindowsFilenameA, NoFile) {
  CheckUriUriStringToWindowsFilenameA("", "");
  CheckUriUriStringToWindowsFilenameA("a", "a");
  CheckUriUriStringToWindowsFilenameA("%0A", "\n");
}

TEST(UriUriStringToWindowsFilenameA, WithFile) {
  CheckUriUriStringToWindowsFilenameA("file://", "\\\\");
  CheckUriUriStringToWindowsFilenameA("file:///", "");
  CheckUriUriStringToWindowsFilenameA("file://a", "\\\\a");
  CheckUriUriStringToWindowsFilenameA("file:///b", "b");
  CheckUriUriStringToWindowsFilenameA("file:///c/", "c\\");
  CheckUriUriStringToWindowsFilenameA("file:///d/e", "d\\e");
}

TEST(UriUriStringToWindowsFilenameA, Dots) {
  CheckUriUriStringToWindowsFilenameA("file://.", "\\\\.");
  CheckUriUriStringToWindowsFilenameA("file://..", "\\\\..");
  CheckUriUriStringToWindowsFilenameA("file:///.", ".");
  CheckUriUriStringToWindowsFilenameA("file:///..", "..");
}

TEST(UriUriStringToWindowsFilenameA, Slashes) {
  // DOS style backslash.
  CheckUriUriStringToWindowsFilenameA("/", "\\");
  CheckUriUriStringToWindowsFilenameA("file://", "\\\\");
  CheckUriUriStringToWindowsFilenameA("file:///a:/", "a:\\");
  CheckUriUriStringToWindowsFilenameA("file:///b:/c", "b:\\c");

  CheckUriWindowsFilenameToUriStringA("file:///a://",
                                      "file%3A%2F%2F%2Fa%3A%2F%2F");

  // Unix
  CheckUriUriStringToWindowsFilenameA("%2F", "\\");
  CheckUriUriStringToWindowsFilenameA("%2Fa", "\\a");
  CheckUriUriStringToWindowsFilenameA("%2Fb%2F", "\\b\\");
  CheckUriUriStringToWindowsFilenameA("c", "c");
  CheckUriUriStringToWindowsFilenameA("d/e", "d\\e");
  CheckUriUriStringToWindowsFilenameA("f/", "f\\");
}

}  // namespace
