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
// Tests for GMLReader::ResolveXlinks (resolvexlinks.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/resolvexlinks.cpp

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gml/gmlreaderp.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlutils.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_http.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(ResolveXlinksTest, NullSourceFileFails) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  bool bIsTemp = false;
  WithQuietHandler quiet_handler;
  EXPECT_FALSE(
      reader->ResolveXlinks("/vsimem/out.gml", &bIsTemp, nullptr, true));
}

TEST(ResolveXlinksTest, NonexistentSourceFileFails) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile("/vsimem/nonexistent.gml");

  bool bIsTemp = false;
  WithQuietHandler quiet_handler;
  EXPECT_FALSE(
      reader->ResolveXlinks("/vsimem/out.gml", &bIsTemp, nullptr, true));
}

constexpr std::string_view kInternalXlinksGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml" xmlns:xlink="http://www.w3.org/1999/xlink">
  <TargetNode gml:id="target1">
    <prop>TargetContent</prop>
  </TargetNode>
  <TargetNode2 gml:id="target2">
    <prop>Target2Content</prop>
  </TargetNode2>
  <Ref1 xlink:href="#target1"/>
  <Ref2 xlink:href="#target2"/>
</root>
)xml";

TEST(ResolveXlinksTest, InternalXlinksSuccess) {
  constexpr char kInPath[] = "/vsimem/resolvexlinks_test/internal.gml";
  constexpr char kOutPath[] = "/vsimem/resolvexlinks_test/internal_out.gml";
  VsiMemTempWrapper file_in(kInPath, std::string(kInternalXlinksGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kInPath);

  bool bIsTemp = false;
  EXPECT_TRUE(reader->ResolveXlinks(kOutPath, &bIsTemp, nullptr, true));
  EXPECT_FALSE(bIsTemp);
  EXPECT_STREQ(kOutPath, reader->GetSourceFileName());

  // Verify resolved XML in kOutPath.
  CPLXMLNode* tree = CPLParseXMLFile(kOutPath);
  ASSERT_NE(nullptr, tree);
  absl::Cleanup tree_closer = [tree] { CPLDestroyXMLNode(tree); };

  CPLXMLNode* ref1 = CPLGetXMLNode(tree, "=root.Ref1.TargetNode");
  ASSERT_NE(nullptr, ref1);
  EXPECT_STREQ("TargetContent", CPLGetXMLValue(ref1, "prop", nullptr));

  VSIUnlink(kOutPath);
}

constexpr std::string_view kExternalTargetGml = R"xml(
<ext_root xmlns:gml="http://www.opengis.net/gml" xmlns:xlink="http://www.w3.org/1999/xlink">
  <UnusedBranch><desc>trimmed</desc></UnusedBranch>
  <ExtTarget gml:id="ext1">
    <relLink xlink:href="sub/deep.gml#d1"/>
    <hashLink xlink:href="#ext2"/>
    <httpLink xlink:href="http://example.com/foo.gml#x"/>
  </ExtTarget>
  <ExtTarget2 gml:id="ext2">
    <val>two</val>
  </ExtTarget2>
</ext_root>
)xml";

constexpr std::string_view kExternalMainGml = R"xml(
<main_root xmlns:gml="http://www.opengis.net/gml" xmlns:xlink="http://www.w3.org/1999/xlink">
  <RefExt1 xlink:href="ext.gml#ext1"/>
  <RefExt2 xlink:href="ext.gml#ext2"/>
  <SkippedRef xlink:href="#ext1"/>
</main_root>
)xml";

TEST(ResolveXlinksTest, ExternalFileXlinksAndSkipElements) {
  constexpr char kExtPath[] = "/vsimem/resolvexlinks_test/ext.gml";
  constexpr char kMainPath[] = "/vsimem/resolvexlinks_test/main.gml";
  constexpr char kOutPath[] = "/vsimem/resolvexlinks_test/ext_out.gml";
  VsiMemTempWrapper file_ext(kExtPath, std::string(kExternalTargetGml));
  VsiMemTempWrapper file_main(kMainPath, std::string(kExternalMainGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kMainPath);

  const char* skip_elements[] = {"SkippedRef", nullptr};
  bool bIsTemp = false;
  // With non-strict and skipped elements, returns warning but success.
  WithQuietHandler quiet_handler;
  EXPECT_TRUE(reader->ResolveXlinks(kOutPath, &bIsTemp,
                                    const_cast<char**>(skip_elements), false));
  EXPECT_FALSE(bIsTemp);

  VSIUnlink(kOutPath);
}

static CPLHTTPResult* MockHttpFetch(const char* pszURL,
                                    CSLConstList /*papszOptions*/,
                                    GDALProgressFunc /*pfnProgress*/,
                                    void* /*pProgressArg*/,
                                    CPLHTTPFetchWriteFunc /*pfnWrite*/,
                                    void* /*pWriteArg*/, void* /*pUserData*/) {
  auto* result =
      static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));
  if (strstr(pszURL, "remote.gml") != nullptr) {
    constexpr std::string_view kRemoteXml =
        "<remote_root xmlns:gml=\"http://www.opengis.net/gml\"><RemoteTarget "
        "gml:id=\"rem1\"><rprop>RemoteData</rprop></RemoteTarget></"
        "remote_root>";
    result->nDataLen = static_cast<int>(kRemoteXml.size());
    result->pabyData = static_cast<GByte*>(CPLMalloc(result->nDataLen + 1));
    memcpy(result->pabyData, kRemoteXml.data(), result->nDataLen);
    result->pabyData[result->nDataLen] = '\0';
    result->nStatus = 0;
  } else {
    result->nStatus = 1;
    result->pszErrBuf = CPLStrdup("Simulated 404");
  }
  return result;
}

TEST(ResolveXlinksTest, HttpFetchResolution) {
  constexpr std::string_view kHttpMainGml = R"xml(
<http_root xmlns:gml="http://www.opengis.net/gml" xmlns:xlink="http://www.w3.org/1999/xlink">
  <RefRemote xlink:href="http://example.com/remote.gml#rem1"/>
</http_root>
)xml";
  constexpr char kMainPath[] = "/vsimem/resolvexlinks_test/http_main.gml";
  constexpr char kOutPath[] = "/vsimem/resolvexlinks_test/http_out.gml";
  VsiMemTempWrapper file_main(kMainPath, std::string(kHttpMainGml));

  CPLHTTPPushFetchCallback(MockHttpFetch, nullptr);
  absl::Cleanup pop_cb = [] { CPLHTTPPopFetchCallback(); };

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kMainPath);

  // In environments without CURL, CPLHTTPEnabled() is false.
  // In strict mode, unresolved HTTP fetch fails.
  bool bIsTemp = false;
  {
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(reader->ResolveXlinks(kOutPath, &bIsTemp, nullptr, true));
  }

  // In non-strict mode, unresolved HTTP fetch logs a warning and succeeds.
  {
    auto reader_nonstrict =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader_nonstrict);
    reader_nonstrict->SetSourceFile(kMainPath);
    WithQuietHandler quiet_handler;
    EXPECT_TRUE(
        reader_nonstrict->ResolveXlinks(kOutPath, &bIsTemp, nullptr, false));
    VSIUnlink(kOutPath);
  }
}

TEST(ResolveXlinksTest, MalformedHrefStrictVsNonStrict) {
  constexpr std::string_view kMalformedGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml" xmlns:xlink="http://www.w3.org/1999/xlink">
  <RefBad xlink:href="no_hash_symbol"/>
  <RefMissing xlink:href="#missing_id"/>
</root>
)xml";
  constexpr char kInPath[] = "/vsimem/resolvexlinks_test/malformed.gml";
  constexpr char kOutPath[] = "/vsimem/resolvexlinks_test/malformed_out.gml";
  VsiMemTempWrapper file_in(kInPath, std::string(kMalformedGml));

  // Strict mode: fails immediately.
  {
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kInPath);
    bool bIsTemp = false;
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(reader->ResolveXlinks(kOutPath, &bIsTemp, nullptr, true));
  }

  // Non-strict mode: warns and continues.
  {
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kInPath);
    bool bIsTemp = false;
    WithQuietHandler quiet_handler;
    EXPECT_TRUE(reader->ResolveXlinks(kOutPath, &bIsTemp, nullptr, false));
    VSIUnlink(kOutPath);
  }
}

TEST(ResolveXlinksTest, TempFileAndUnwritableOutputs) {
  CPLSetConfigOption("CPL_TMPDIR", "/vsimem");
  absl::Cleanup reset_tmpdir = [] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
  };

  constexpr char kInPath[] = "/vsimem/resolvexlinks_test/simple.gml";
  constexpr std::string_view kSimpleXml = "<root><elem>val</elem></root>";
  VsiMemTempWrapper file_in(kInPath, std::string(kSimpleXml));

  // Unwritable output and temp dir fails completely.
  {
    CPLSetConfigOption("CPL_TMPDIR", "/proc/unwritable_dir");
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kInPath);
    bool bIsTemp = false;
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(reader->ResolveXlinks("/proc/unwritable_dir/out.gml", &bIsTemp,
                                       nullptr, true));
    CPLSetConfigOption("CPL_TMPDIR", "/vsimem");
  }

  // /vsigzip/ path triggers temp file creation in /vsimem.
  {
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kInPath);
    bool bIsTemp = false;
    EXPECT_TRUE(reader->ResolveXlinks("/vsigzip//vsimem/out.gml.gz", &bIsTemp,
                                      nullptr, true));
    EXPECT_TRUE(bIsTemp);
    // Cleanup generated temp file.
    const char* pszTempFile = reader->GetSourceFileName();
    if (pszTempFile != nullptr && std::string_view(pszTempFile) != kInPath) {
      VSIUnlink(pszTempFile);
    }
  }
}

}  // namespace
}  // namespace autotest2
