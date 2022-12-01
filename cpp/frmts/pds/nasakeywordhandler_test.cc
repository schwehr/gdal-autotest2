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
// See also:
//   https://pds.jpl.nasa.gov/tools/standards-reference.shtml
//   https://pds.jpl.nasa.gov/documents/sr/StdRef_20090227_v3.8.pdf
//   https://github.com/OSGeo/gdal/blob/master/gdal/frmts/pds
//   https://github.com/OSGeo/gdal/blob/master/gdal/ogr/ogrsf_frmts/pds
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/pds.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_pds.py
//   https://github.com/nasa/VICAR
//   https://isis.astrogeology.usgs.gov/

#include "frmts/pds/nasakeywordhandler.h"

#include <string.h>
#include <memory>

#include "logging.h"
#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/strings/str_cat.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogrsf_frmts/geojson/libjson/json_object.h"
#include "port/cpl_port.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr int kSuccess = 0;

TEST(NasaKeywordHandlerTest, PdsSeekTooFar) {
  constexpr char kPds[] = "END\n";
  auto bytes = reinterpret_cast<GByte *>(const_cast<char *>(kPds));
  constexpr char kFilename[] = "/vsimem/a.pds";

  // Do not take ownership.
  VSILFILE *dst = VSIFileFromMemBuffer(kFilename, bytes, strlen(kPds), FALSE);
  CHECK_NOTNULL(dst);
  auto dst_closer = absl::MakeCleanup([dst] { VSIFCloseL(dst); });

  auto handler = absl::make_unique<NASAKeywordHandler>();
  {
    VSILFILE *src = VSIFOpenL(kFilename, "r");
    auto src_closer = absl::MakeCleanup([src] { VSIFCloseL(src); });

    // See past the end of the file.
    EXPECT_FALSE(handler->Ingest(src, 100));
  }

  // Note that nasakeywordhandler never sets the GDAL error state.
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
  EXPECT_EQ(0, CPLGetErrorCounter());
}

std::unique_ptr<NASAKeywordHandler> MakeHandler(const char *data) {
  constexpr char kFilename[] = "/vsimem/a.pds";
  auto bytes = reinterpret_cast<GByte *>(const_cast<char *>(data));

  // Do not take ownership.
  VSILFILE *dst = VSIFileFromMemBuffer(kFilename, bytes, strlen(data), FALSE);
  CHECK_NOTNULL(dst);
  auto dst_closer = absl::MakeCleanup([dst] { VSIFCloseL(dst); });

  auto handler = absl::make_unique<NASAKeywordHandler>();
  {
    VSILFILE *src = VSIFOpenL(kFilename, "r");
    auto src_closer = absl::MakeCleanup([src] { VSIFCloseL(src); });

    handler->Ingest(src, 0);  // Ignore failures.
  }
  return handler;
}

std::vector<string> PdsToKeywords(const char *data) {
  auto handler = MakeHandler(data);
  char **keyword_list = handler->GetKeywordList();
  return CslToVector(keyword_list);
}

TEST(NasaKeywordHandlerTest, PdsEmpty) {
  constexpr char kPds[] = "";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_TRUE(keywords.empty());

  // Note that nasakeywordhandler never sets the GDAL error state.
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
  EXPECT_EQ(0, CPLGetErrorCounter());
}

TEST(NasaKeywordHandlerTest, PdsEnd) {
  constexpr char kPds[] = "END";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsSimple) {
  constexpr char kPds[] = "FOO = BAR\nEND\n";
  auto handler = MakeHandler(kPds);
  char **keyword_list = handler->GetKeywordList();
  auto keywords = CslToVector(keyword_list);
  EXPECT_THAT(keywords, testing::ElementsAre("FOO=BAR"));

  EXPECT_STREQ("BAR", handler->GetKeyword("FOO", nullptr));
  EXPECT_EQ(nullptr, handler->GetKeyword("DoesNotExist", nullptr));
  EXPECT_STREQ("baz", handler->GetKeyword("DoesNotExist", "baz"));

  EXPECT_EQ(0, CPLGetErrorCounter());
}

TEST(NasaKeywordHandlerTest, PdsCrazyName) {
  constexpr char kPds[] = "!@#$%& = BAR\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("!@#$%&=BAR"));
}

TEST(NasaKeywordHandlerTest, PdsSimpleInt) {
  constexpr char kPds[] = "a = 1\nEND\n";
  auto handler = MakeHandler(kPds);
  char **keyword_list = handler->GetKeywordList();
  auto keywords = CslToVector(keyword_list);
  EXPECT_THAT(keywords, testing::ElementsAre("a=1"));

  EXPECT_STREQ("1", handler->GetKeyword("a", nullptr));
}

TEST(NasaKeywordHandlerTest, PdsSimpleDouble) {
  constexpr char kPds[] = "a = 2.3\nEND\n";
  auto handler = MakeHandler(kPds);
  char **keyword_list = handler->GetKeywordList();
  auto keywords = CslToVector(keyword_list);
  EXPECT_THAT(keywords, testing::ElementsAre("a=2.3"));

  EXPECT_STREQ("2.3", handler->GetKeyword("a", nullptr));
}

TEST(NasaKeywordHandlerTest, PdsSimpleString) {
  constexpr char kPds[] = "a = \"b c\"\nEND\n";
  auto handler = MakeHandler(kPds);
  char **keyword_list = handler->GetKeywordList();
  auto keywords = CslToVector(keyword_list);
  EXPECT_THAT(keywords, testing::ElementsAre("a=\"b c\""));

  EXPECT_STREQ("\"b c\"", handler->GetKeyword("a", nullptr));
}

TEST(NasaKeywordHandlerTest, PdsSimpleStringSingleQuotes) {
  constexpr char kPds[] = "a = 'bc'\nEND\n";
  auto handler = MakeHandler(kPds);
  char **keyword_list = handler->GetKeywordList();
  auto keywords = CslToVector(keyword_list);
  EXPECT_THAT(keywords, testing::ElementsAre("a='bc'"));

  EXPECT_STREQ("'bc'", handler->GetKeyword("a", nullptr));
}

TEST(NasaKeywordHandlerTest, PdsSimpleStringSingleQuotesEofInStr) {
  constexpr char kPds[] = "a = 'bc";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsSimpleStringExtraWhiteSpace) {
  constexpr char kPds[] = " \t a \t=\t \"b\"\t \r\n\t\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("a=\"b\""));
}

TEST(NasaKeywordHandlerTest, PdsSimpleStringTruncated) {
  constexpr char kPds[] = "a = \"b ";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsTwoLineString) {
  constexpr char kPds[] = "a = \"b\nc\"\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("a=\"b\\nc\""));
}

TEST(NasaKeywordHandlerTest, PdsTwoLineStringR) {
  constexpr char kPds[] = "a = \"b\rc\"\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("a=\"b\\rc\""));
}

TEST(NasaKeywordHandlerTest, PdsEmptyList) {
  constexpr char kPds[] = "a = ()\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("a=()"));
}

TEST(NasaKeywordHandlerTest, PdsListOfInts) {
  constexpr char kPds[] = "a = (4,2)\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("a=(4,2)"));
}

TEST(NasaKeywordHandlerTest, PdsListOfIntsCurly) {
  constexpr char kPds[] = "a={4,2}\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("a={4,2}"));
}

TEST(NasaKeywordHandlerTest, PdsListBadBracing) {
  constexpr char kPds[] = "a = (5,3}\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsListBadBracingReversed) {
  constexpr char kPds[] = "a = {5,3)\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsListOfStrings) {
  constexpr char kPds[] = "a = (\"b\", \"c\")\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("a=(\"b\",\"c\")"));
}

TEST(NasaKeywordHandlerTest, PdsListOfSymbols) {
  constexpr char kPds[] = "a = (B, C)\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("a=(B,C)"));
}

TEST(NasaKeywordHandlerTest, PdsListOfLists) {
  constexpr char kPds[] = "a = (B, (C, D), {E, F})\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("a=(B,(C,D),{E,F})"));
}

TEST(NasaKeywordHandlerTest, PdsGroup) {
  constexpr char kPds[] = "GROUP = A\n  B = 1\nEND_GROUP = A\nEND\n";
  auto handler = MakeHandler(kPds);
  char **keyword_list = handler->GetKeywordList();
  auto keywords = CslToVector(keyword_list);
  EXPECT_THAT(keywords, testing::ElementsAre("A.B=1"));

  EXPECT_EQ(nullptr, handler->GetKeyword("A", nullptr));
  EXPECT_STREQ("1", handler->GetKeyword("A.B", nullptr));
}

TEST(NasaKeywordHandlerTest, PdsGroupIntName) {
  constexpr char kPds[] = "GROUP = 1\n  B = 2\nEND_GROUP = 1\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("1.B=2"));
}

TEST(NasaKeywordHandlerTest, PdsGroupMismatch) {
  // The parser does not care about mismatches.
  constexpr char kPds[] = "GROUP = A\n  B = 1\nEND_GROUP = B\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("A.B=1"));
}

TEST(NasaKeywordHandlerTest, PdsGroupCloseAsObject) {
  // The parser does not care about mismatches.
  constexpr char kPds[] = "GROUP = A\n  B = 1\nEND_OBJECT A\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("A.B=1"));
}

TEST(NasaKeywordHandlerTest, PdsGroupQuoteName) {
  constexpr char kPds[] = "GROUP = \"A\"\n  B = 1\nEND_GROUP = \"A\"\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("\"A\".B=1"));
}

TEST(NasaKeywordHandlerTest, PdsGroupNoClose) {
  constexpr char kPds[] = "GROUP = A\n  B = 1\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("A.B=1"));
}

TEST(NasaKeywordHandlerTest, PdsGroupIsArrayNotClosed) {
  constexpr char kPds[] = "GROUP=(";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsGroupFuzzerReadGroupFail) {
  constexpr char kPds[] = "E=()< !";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("E=() <!"));
}

TEST(NasaKeywordHandlerTest, PdsObject) {
  constexpr char kPds[] = "OBJECT = C\n  D = 2\nEND_OBJECT = A\nEND\n";
  auto handler = MakeHandler(kPds);
  char **keyword_list = handler->GetKeywordList();
  auto keywords = CslToVector(keyword_list);
  EXPECT_THAT(keywords, testing::ElementsAre("C.D=2"));

  EXPECT_STREQ("2", handler->GetKeyword("C.D", nullptr));
}

TEST(NasaKeywordHandlerTest, PdsLargerThan512Bytes) {
  const string pds = absl::StrCat(string(1000, ' '), "\nEND\n");
  auto keywords = PdsToKeywords(pds.c_str());
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsLargerThan512BytesEmbedEnd) {
  const string pds =
      absl::StrCat(string(521, ' '), "\nEND\n", string(521, ' '));
  auto keywords = PdsToKeywords(pds.c_str());
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsMultiEnd) {
  constexpr char kPds[] = "OBJECT=b\nEND\nOBJECT=Table\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsFuzzerCraziness) {
  constexpr char kPds[] = "S=.>>\rE:=>\rE=. <>\rE:.=,\rE:. =,> E:.\r=F\rE:. =,";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("S=.>>", "E:. <>", "E:.=,"));
}

TEST(NasaKeywordHandlerTest, PdsPointerInt) {
  constexpr char kPds[] = "^IMAGE = 4\nEND\n123";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("^IMAGE=4"));
}

TEST(NasaKeywordHandlerTest, PdsPointerString) {
  constexpr char kPds[] = "^TABLE = \"A.TAB\"\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("^TABLE=\"A.TAB\""));
}

TEST(NasaKeywordHandlerTest, PdsUnits) {
  constexpr char kPds[] = "A=1     <METER>\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("A=1 <METER>"));
}

TEST(NasaKeywordHandlerTest, PdsBitMask) {
  constexpr char kPds[] = "BIT_MASK = 2#0011111111111111#\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("BIT_MASK=2#0011111111111111#"));
}

TEST(NasaKeywordHandlerTest, PdsComments) {
  constexpr char kPds[] =
      "/* Comment */\nA = B /* Comment */\n/* comment */\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("A=B"));
}

TEST(NasaKeywordHandlerTest, PdsEofInComment) {
  constexpr char kPds[] =      "/* Comment";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_TRUE(keywords.empty());
}

TEST(NasaKeywordHandlerTest, PdsCommentsBroken) {
  constexpr char kPds[] =
      "/* Comment */\nA = /* comment */ B /* Comment */\n/* comment */\nEND\n";
  auto keywords = PdsToKeywords(kPds);

  // Whoops...
  EXPECT_THAT(keywords, testing::ElementsAre("A=END"));
}

TEST(NasaKeywordHandlerTest, PdsHashComments) {
  constexpr char kPds[] =  "A=B\n# Comment\nC=D # Comment\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("A=B", "C=D"));
}

TEST(NasaKeywordHandlerTest, PdsLineExtensionWithMinus) {
  constexpr char kPds[] = "A-\n = B\nEND\n";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("A=B"));
}

TEST(NasaKeywordHandlerTest, PdsCommaCurly) {
  constexpr char kPds[] = ",={}";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre(",={}"));
}

TEST(NasaKeywordHandlerTest, PdsFuzzerForReadPairWordCoverage) {
  constexpr char kPds[] = "\xd7\x3d\x7b\x2e";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("\xD7={."));
}

TEST(NasaKeywordHandlerTest, PdsPartialUnit) {
  constexpr char kPds[] = "T=O <";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("T=O <"));
}

TEST(NasaKeywordHandlerTest, PdsFuzzerReadGroupCoverage) {
  constexpr char kPds[] = "O=d GROUP=O End";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("O=d"));
}

TEST(NasaKeywordHandlerTest, PdsFuzzerReadGroupCoverage2) {
  constexpr char kPds[] = "O=d GROUP=O End GROUP=O End";
  auto keywords = PdsToKeywords(kPds);
  EXPECT_THAT(keywords, testing::ElementsAre("O=d"));
}

TEST(NasaKeywordHandlerTest, PdsDeepListParen) {
  constexpr int depth = 1000;
  const string data =
      absl::StrCat("a=", string(depth, '('), "1", string(depth, ')'));
  auto keywords = PdsToKeywords(data.c_str());

  EXPECT_EQ(1, keywords.size());
  EXPECT_EQ(3 + depth * 2, keywords[0].size());
}

TEST(NasaKeywordHandlerTest, PdsDeepListBraces) {
  constexpr int depth = 1000;
  const string data =
      absl::StrCat("a=", string(depth, '{'), "1", string(depth, '}'));
  auto keywords = PdsToKeywords(data.c_str());

  EXPECT_EQ(1, keywords.size());
  EXPECT_EQ(3 + depth * 2, keywords[0].size());
}

TEST(NasaKeywordHandlerTest, PdsAllTheFeatures) {
  // Try to use all the features in one PDS header.
  // Avoids invalid constructs.
  // Does not cover all that is in the PDS specification.
  constexpr char kPds[] = R"pds(PDS_VERSION_ID = PDS3
    A = FOO
    /* C style comment */
    # shell style comment
    b = 1
    b_with_units = 2 <meters>
    b_only_units = <seconds>
    B = 3.45
    C = "bar"
    D = "foo
      bar"
    E = 'baz'
    G = 'qux
    quz'
    H = ()
    I = (1, 2)
    J = (4, (5, 6))
    K = ("a", "b")
    L = ({}, (), 6.7)
    /* Same goes for braces as for parens */
    M = {}
    GROUP N
      GROUP O
        P = "tutu"
      END_GROUP
      Q = "toto"
    END_GROUP N
    ^R = THING_POINTED_TO

    OBJECT S
      T = 42
    END_OBJECT S
END
  )pds";

  auto keywords = PdsToKeywords(kPds);
  EXPECT_EQ(15, keywords.size());
}

}  // namespace
}  // namespace autotest2
