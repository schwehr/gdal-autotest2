// Copyright 2018 Google Inc. All Rights Reserved.
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

// Test GDAL's usage of the expact XML parser.
//
// Full expat testing should happen in expat, so this tries to just cover
// ogr_expat.h and some basics of expact to make sure that it is vaguely sane.

#include "third_party/gdal/ogr/ogr_expat.h"

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/cpp/expat/lib/expat.h"
#include "third_party/cpp/expat/lib/expat_external.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

TEST(ExpatTest, Noop) {
  // Just create and destroy with no operations to check for memory issues.
  XML_Parser parser = OGRCreateExpatXMLParser();
  ASSERT_NE(nullptr, parser);
  XML_ParserFree(parser);
}

// Noop callbacks
void XMLCALL start_cb(void *user_data, const char *el, const char **attr) {}
void XMLCALL end_cb(void *user_data, const char *el) {}
void XMLCALL data_cb(void *user_data, const char *data, int len) {}

TEST(ExpatTest, SimpleEmpty) {
  // Parse an empty string.

  XML_Parser parser = OGRCreateExpatXMLParser();
  auto parser_cleaner = absl::MakeCleanup([parser] { XML_ParserFree(parser); });

  constexpr char kData[] = "";

  XML_SetElementHandler(parser, start_cb, end_cb);
  XML_SetCharacterDataHandler(parser, data_cb);

  EXPECT_EQ(XML_STATUS_OK, XML_Parse(parser, kData, sizeof(kData), false));

  EXPECT_EQ(XML_STATUS_ERROR, XML_Parse(parser, kData, sizeof(kData), true));
  EXPECT_EQ(XML_ERROR_INVALID_TOKEN, XML_GetErrorCode(parser));
  EXPECT_EQ(1, XML_GetCurrentLineNumber(parser));
  EXPECT_EQ(0, XML_GetCurrentColumnNumber(parser));
}

TEST(ExpatTest, MemoryAllocationLimit) {
  XML_Parser parser = OGRCreateExpatXMLParser();
  ASSERT_NE(nullptr, parser);
  auto parser_cleaner = absl::MakeCleanup([parser] { XML_ParserFree(parser); });

  // Exceeding 10MB default limit fails and emits an error
  {
    WithQuietHandler quiet_handler;
    void* buf = XML_GetBuffer(parser, 10000005);
    EXPECT_EQ(nullptr, buf);
  }

  // With OGR_EXPAT_UNLIMITED_MEM_ALLOC set to YES, larger allocations succeed
  CPLSetConfigOption("OGR_EXPAT_UNLIMITED_MEM_ALLOC", "YES");
  auto cleanup_opt = absl::MakeCleanup(
      [] { CPLSetConfigOption("OGR_EXPAT_UNLIMITED_MEM_ALLOC", nullptr); });

  void* buf = XML_GetBuffer(parser, 10000005);
  EXPECT_NE(nullptr, buf);
}

TEST(ExpatTest, CharacterEncodings) {
  // Test Windows-1252
  {
    XML_Parser parser = OGRCreateExpatXMLParser();
    ASSERT_NE(nullptr, parser);
    auto parser_cleaner =
        absl::MakeCleanup([parser] { XML_ParserFree(parser); });

    const char kXml[] =
        "<?xml version=\"1.0\" "
        "encoding=\"WINDOWS-1252\"?><root>\x80\x99\xa0</root>";
    EXPECT_EQ(XML_STATUS_OK, XML_Parse(parser, kXml, sizeof(kXml) - 1, true));
  }

  // Test ISO-8859-15
  {
    XML_Parser parser = OGRCreateExpatXMLParser();
    ASSERT_NE(nullptr, parser);
    auto parser_cleaner =
        absl::MakeCleanup([parser] { XML_ParserFree(parser); });

    const char kXml[] =
        "<?xml version=\"1.0\" "
        "encoding=\"ISO-8859-15\"?><root>\xa4\xa6\xb4\xb8\xbc\xbd\xbe</root>";
    EXPECT_EQ(XML_STATUS_OK, XML_Parse(parser, kXml, sizeof(kXml) - 1, true));
  }

  // Test unhandled encoding
  {
    WithQuietHandler quiet_handler;
    XML_Parser parser = OGRCreateExpatXMLParser();
    ASSERT_NE(nullptr, parser);
    auto parser_cleaner =
        absl::MakeCleanup([parser] { XML_ParserFree(parser); });

    const char kXml[] =
        "<?xml version=\"1.0\" encoding=\"UNKNOWN-ENC\"?><root>test</root>";
    EXPECT_EQ(XML_STATUS_ERROR,
              XML_Parse(parser, kXml, sizeof(kXml) - 1, true));
    EXPECT_EQ(XML_ERROR_UNKNOWN_ENCODING, XML_GetErrorCode(parser));
  }
}

}  // namespace
}  // namespace autotest2
