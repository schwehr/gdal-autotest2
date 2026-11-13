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
// Tests CODA XML product reopen, schema-guided reopen with definition,
// close, root cursor initialization, format recognition, mmap/fd reading, and
// error paths (libcoda/coda-xml.c).
//
// google3 build caveats:
//   - libcoda defines XML_DTD=1, XML_NS=1, XML_LARGE_SIZE=1, and
//     XML_CONTEXT_BYTES=1024 in config.h while linking //third_party/cpp/expat;
//     because CODA registers XML_PARAM_ENTITY_PARSING_ALWAYS without an
//     ExternalEntityRefHandler, Expat does not invoke not_standalone_handler().
//   - coda_recognize_file() / coda_open() only identify files starting with
//     "<?xml" or UTF-8 BOM + "<?xml" (\xEF\xBB\xBF<?xml) as coda_format_xml;
//     XML lacking an XML declaration falls back to coda_format_binary.
//   - Reachable ceiling on libcoda/coda-xml.c: lines 50-56 (malloc failure for
//     coda_xml_product) and 69-75 (strdup failure for filename) in
//     coda_xml_reopen() are OOM branches unreachable without allocator fault
//     injection. Every reachable line and branch in coda-xml.c is covered.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-xml.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-xml.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-xml-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-xml-parser.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-bin.h"
#include "third_party/stcorp_coda/libcoda/coda-definition.h"
#include "third_party/stcorp_coda/libcoda/coda-xml-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-xml.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

// Isolates CODA environment variables, definition path, mmap option, and
// thread-local coda_errno across tests.
class ScopedXmlTestEnv {
 public:
  ScopedXmlTestEnv()
      : env_def_("CODA_DEFINITION"),
        env_leap_("CODA_LEAP_SECOND_TABLE"),
        saved_use_mmap_(coda_get_option_use_mmap()) {
    env_def_.Unset();
    env_leap_.Unset();
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ~ScopedXmlTestEnv() {
    coda_set_option_use_mmap(saved_use_mmap_);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ScopedXmlTestEnv(const ScopedXmlTestEnv&) = delete;
  ScopedXmlTestEnv& operator=(const ScopedXmlTestEnv&) = delete;

 private:
  ScopedEnvVar env_def_;
  ScopedEnvVar env_leap_;
  int saved_use_mmap_;
};

// Reads a CODA string cursor into std::string using exact length + 1 sizing.
std::string ReadCursorString(const coda_cursor* cursor) {
  long length = -1;
  if (coda_cursor_get_string_length(cursor, &length) != CODA_SUCCESS ||
      length < 0) {
    ADD_FAILURE() << "coda_cursor_get_string_length failed: "
                  << coda_errno_to_string(coda_errno);
    return {};
  }
  std::vector<char> buf(static_cast<size_t>(length) + 1, '\0');
  if (coda_cursor_read_string(cursor, buf.data(), length + 1) != CODA_SUCCESS) {
    ADD_FAILURE() << "coda_cursor_read_string failed: "
                  << coda_errno_to_string(coda_errno);
    return {};
  }
  return std::string(buf.data(), static_cast<size_t>(length));
}

constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="XmlTestClass">
  <cd:ProductType name="XmlItemType">
    <cd:ProductDefinition id="XmlItemProd" format="xml" version="1">
      <cd:DetectionRule>
        <cd:MatchExpression>exists(/item)</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="XmlEmptyType">
    <cd:ProductDefinition id="XmlEmptyProd" format="xml" version="1">
      <cd:DetectionRule>
        <cd:MatchExpression>exists(/empty)</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)xml";

constexpr std::string_view kXmlItemProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="XmlItemProd" format="xml">
  <cd:Record>
    <cd:Field name="item">
      <cd:Record>
        <cd:Attribute name="id">
          <cd:Text/>
        </cd:Attribute>
        <cd:Attribute name="opt_attr">
          <cd:Text/>
          <cd:Optional/>
        </cd:Attribute>
        <cd:Field name="name">
          <cd:Text/>
        </cd:Field>
        <cd:Field name="count">
          <cd:Integer format="ascii">
            <cd:NativeType>int32</cd:NativeType>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="scale">
          <cd:Float format="ascii">
            <cd:NativeType>double</cd:NativeType>
          </cd:Float>
        </cd:Field>
        <cd:Field name="tag">
          <cd:Array>
            <cd:Dimension/>
            <cd:Text/>
          </cd:Array>
          <cd:Optional/>
        </cd:Field>
      </cd:Record>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kXmlEmptyProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="XmlEmptyProd" format="xml">
  <cd:Record>
    <cd:Field name="empty">
      <cd:Record/>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

std::vector<uint8_t> BuildXmlTestCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 2> kEntries =
      {{
          {"products/XmlItemProd.xml", kXmlItemProdXml},
          {"products/XmlEmptyProd.xml", kXmlEmptyProdXml},
      }};
  return BuildCodadefBytes(kIndexXml, kEntries);
}

TEST(CodaXmlTest, RecognizeFileDetectsXmlVariantsAndFallbackToBinary) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Schemaless detection across XML declaration, UTF-8 BOM, comments,
  //    DOCTYPE, and missing-declaration XML.
  struct RecognizeCase {
    std::string_view name;
    std::string_view content;
    coda_format expected_format;
    bool can_open_schemaless;
  };
  constexpr RecognizeCase kCases[] = {
      {.name = "xml_decl_only.xml",
       .content = "<?xml version=\"1.0\"?>\n<root/>\n",
       .expected_format = coda_format_xml,
       .can_open_schemaless = true},
      {.name = "xml_utf8_bom.xml",
       .content = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<root>bom_text</root>\n",
       .expected_format = coda_format_xml,
       .can_open_schemaless = true},
      {.name = "xml_comment_doctype.xml",
       .content = "<?xml version=\"1.0\"?>\n<!-- leading comment -->\n"
                  "<!DOCTYPE root [<!ENTITY msg \"hello\">]>\n"
                  "<root>&msg;</root>\n",
       .expected_format = coda_format_xml,
       .can_open_schemaless = true},
      {.name = "xml_without_decl.xml",
       .content = "<root>missing_declaration</root>\n",
       .expected_format = coda_format_binary,
       .can_open_schemaless = false},
      {.name = "xml_comment_without_decl.xml",
       .content = "<!-- comment --><root/>\n",
       .expected_format = coda_format_binary,
       .can_open_schemaless = false},
      {.name = "xml_empty_file.xml",
       .content = "",
       .expected_format = coda_format_binary,
       .can_open_schemaless = false},
  };

  for (const RecognizeCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const TempFile file(tc.name);
    ASSERT_TRUE(file.WriteText(tc.content));

    int64_t file_size = -1;
    coda_format fmt = coda_format_sp3;
    const char* pclass = "sentinel";
    const char* ptype = "sentinel";
    int version = 99;
    ASSERT_EQ(CODA_SUCCESS, coda_recognize_file(file.c_str(), &file_size, &fmt,
                                                &pclass, &ptype, &version));
    EXPECT_EQ(static_cast<int64_t>(tc.content.size()), file_size);
    EXPECT_EQ(tc.expected_format, fmt);
    EXPECT_EQ(nullptr, pclass);
    EXPECT_EQ(nullptr, ptype);
    EXPECT_EQ(-1, version);

    ScopedProduct product;
    if (tc.can_open_schemaless) {
      ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));
      coda_format open_fmt = coda_format_binary;
      ASSERT_EQ(CODA_SUCCESS,
                coda_get_product_format(product.get(), &open_fmt));
      EXPECT_EQ(coda_format_xml, open_fmt);
    } else {
      coda_set_error(CODA_SUCCESS, nullptr);
      EXPECT_EQ(-1, coda_open(file.c_str(), product.OutParam()));
      EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
      EXPECT_EQ(nullptr, product.get());
    }
  }
}

TEST(CodaXmlTest, RecognizeAndOpenHandleFilesystemAndStatErrors) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Missing file triggers ENOENT -> CODA_ERROR_FILE_NOT_FOUND.
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) / "coda_xml_missing.xml";
  std::error_code ec;
  std::filesystem::remove(missing_path, ec);

  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing_path.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_recognize_file(missing_path.c_str(), nullptr, nullptr,
                                    nullptr, nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);

  // 2. Directory path fails S_IFREG check -> CODA_ERROR_FILE_OPEN.
  const TempDir dir("coda_xml_dir_not_regular");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(dir.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("not a regular file"));

  // 3. Path traversing through a regular file triggers ENOTDIR ->
  //    CODA_ERROR_FILE_OPEN.
  const TempFile regular("coda_xml_not_a_dir.xml");
  ASSERT_TRUE(regular.WriteText("<?xml version=\"1.0\"?><root/>\n"));
  const std::filesystem::path child = regular.path() / "nested.xml";
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(child.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
}

TEST(CodaXmlTest, DirectReopenCloseAndCursorSetProductInMmapAndFdModes) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  constexpr std::string_view kEmptyXml = "<?xml version=\"1.0\"?>\n<empty/>\n";
  constexpr std::string_view kRichXml =
      R"xml(<?xml version="1.0" encoding="UTF-8"?>
<!-- header comment -->
<!DOCTYPE root [<!ENTITY entity_val "expanded">]>
<ns:root xmlns:ns="urn:coda:xml:test" kind="sample">
  <ns:entry>first</ns:entry>
  <ns:entry>&entity_val;</ns:entry>
</ns:root>
)xml";

  const TempFile empty_file("coda_xml_direct_empty.xml");
  ASSERT_TRUE(empty_file.WriteText(kEmptyXml));
  const TempFile rich_file("coda_xml_direct_rich.xml");
  ASSERT_TRUE(rich_file.WriteText(kRichXml));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    // 1. Empty XML element produces root_type != NULL and mem_ptr == NULL,
    //    exercising the mem_ptr == NULL path in coda_xml_close().
    {
      coda_product* raw = nullptr;
      ASSERT_EQ(CODA_SUCCESS,
                coda_bin_open(empty_file.c_str(),
                              static_cast<int64_t>(kEmptyXml.size()), &raw));
      ASSERT_EQ(CODA_SUCCESS, coda_xml_reopen(&raw));
      ScopedProduct xml_holder(raw);

      auto* xml_prod = reinterpret_cast<coda_xml_product*>(raw);
      EXPECT_STREQ(empty_file.c_str(), xml_prod->filename);
      EXPECT_EQ(static_cast<int64_t>(kEmptyXml.size()), xml_prod->file_size);
      EXPECT_EQ(coda_format_xml, xml_prod->format);
      EXPECT_NE(nullptr, xml_prod->root_type);
      EXPECT_EQ(nullptr, xml_prod->product_definition);
      EXPECT_EQ(0, xml_prod->mem_size);
      EXPECT_EQ(nullptr, xml_prod->mem_ptr);
      EXPECT_NE(nullptr, xml_prod->raw_product);

      coda_cursor cursor;
      ASSERT_EQ(CODA_SUCCESS, coda_xml_cursor_set_product(&cursor, raw));
      coda_product* cursor_prod = nullptr;
      int depth = -1;
      coda_format fmt = coda_format_binary;
      coda_type_class type_class = coda_integer_class;
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_get_product_file(&cursor, &cursor_prod));
      EXPECT_EQ(raw, cursor_prod);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&cursor, &depth));
      EXPECT_EQ(0, depth);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&cursor, &fmt));
      EXPECT_EQ(coda_format_xml, fmt);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
      EXPECT_EQ(coda_record_class, type_class);

      // Explicitly invoke coda_xml_close() rather than relying on coda_close().
      ASSERT_EQ(CODA_SUCCESS, coda_xml_close(xml_holder.release()));
    }

    // 2. Populated namespaced XML populates mem_ptr != NULL and exposes
    //    xmlns + custom attributes and repeated child elements.
    {
      coda_product* raw = nullptr;
      ASSERT_EQ(CODA_SUCCESS,
                coda_bin_open(rich_file.c_str(),
                              static_cast<int64_t>(kRichXml.size()), &raw));
      ASSERT_EQ(CODA_SUCCESS, coda_xml_reopen(&raw));
      ScopedProduct xml_holder(raw);

      auto* xml_prod = reinterpret_cast<coda_xml_product*>(raw);
      EXPECT_EQ(coda_format_xml, xml_prod->format);
      EXPECT_NE(nullptr, xml_prod->root_type);
      EXPECT_GT(xml_prod->mem_size, 0);
      EXPECT_NE(nullptr, xml_prod->mem_ptr);

      coda_cursor cursor;
      ASSERT_EQ(CODA_SUCCESS, coda_xml_cursor_set_product(&cursor, raw));

      coda_cursor attr_cursor = cursor;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&attr_cursor, "/root@kind"));
      EXPECT_EQ("sample", ReadCursorString(&attr_cursor));

      coda_cursor entry_cursor = cursor;
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_goto(&entry_cursor, "/root/entry[0]"));
      EXPECT_EQ("first", ReadCursorString(&entry_cursor));

      entry_cursor = cursor;
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_goto(&entry_cursor, "/root/entry[1]"));
      EXPECT_EQ("expanded", ReadCursorString(&entry_cursor));

      ASSERT_EQ(CODA_SUCCESS, coda_xml_close(xml_holder.release()));
    }
  }
}

TEST(CodaXmlTest, DirectXmlCloseHandlesAllNullFields) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // coda_xml_close() calls free(product_file), so allocate via std::calloc to
  // exercise the NULL-pointer branches for filename, root_type, mem_ptr, and
  // raw_product safely under ASan.
  auto* empty_xml_prod =
      static_cast<coda_xml_product*>(std::calloc(1, sizeof(coda_xml_product)));
  ASSERT_NE(nullptr, empty_xml_prod);
  empty_xml_prod->format = coda_format_xml;

  EXPECT_EQ(CODA_SUCCESS,
            coda_xml_close(reinterpret_cast<coda_product*>(empty_xml_prod)));
}

TEST(CodaXmlTest, LargeMultiBlockXmlReadsAcrossBuffersInMmapAndFdModes) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // Exceed BUFFSIZE (8192 bytes in coda-xml-parser.c) to exercise multi-block
  // feeding in both mmap and lseek/read modes.
  constexpr int kNumEntries = 120;
  std::string large_xml = "<?xml version=\"1.0\"?>\n<batch>\n";
  for (int i = 0; i < kNumEntries; ++i) {
    absl::StrAppend(&large_xml, "  <entry idx=\"", i,
                    "\">payload_block_data_0123456789abcdef_0123456789abcdef_",
                    i, "</entry>\n");
  }
  large_xml += "</batch>\n";
  ASSERT_GT(large_xml.size(), size_t{9000});

  const TempFile large_file("coda_xml_large.xml");
  ASSERT_TRUE(large_file.WriteText(large_xml));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(large_file.c_str(), product.OutParam()));

    coda_format fmt = coda_format_binary;
    int64_t file_size = -1;
    const char* pclass = "sentinel";
    const char* ptype = "sentinel";
    int version = 99;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &fmt));
    EXPECT_EQ(coda_format_xml, fmt);
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_file_size(product.get(), &file_size));
    EXPECT_EQ(static_cast<int64_t>(large_xml.size()), file_size);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_class(product.get(), &pclass));
    EXPECT_EQ(nullptr, pclass);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_type(product.get(), &ptype));
    EXPECT_EQ(nullptr, ptype);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_version(product.get(), &version));
    EXPECT_EQ(-1, version);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/batch/entry"));
    long num_elements = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_num_elements(&cursor, &num_elements));
    EXPECT_EQ(kNumEntries, num_elements);

    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&cursor, 0));
    EXPECT_EQ("payload_block_data_0123456789abcdef_0123456789abcdef_0",
              ReadCursorString(&cursor));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(
                                &cursor, kNumEntries - 1));
    EXPECT_EQ("payload_block_data_0123456789abcdef_0123456789abcdef_119",
              ReadCursorString(&cursor));
  }
}

TEST(CodaXmlTest, SchemaGuidedXmlViaCodadefAndReopenWithDefinition) {
  const ScopedXmlTestEnv test_env;
  const TempFile codadef_file("coda_xml_schema.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildXmlTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  constexpr std::string_view kValidItemXml = R"xml(<?xml version="1.0"?>
<item id="rec-42">
  <name>sensor_a</name>
  <count>-17</count>
  <scale>1.25</scale>
  <tag>alpha</tag>
  <tag>beta</tag>
</item>
)xml";
  constexpr std::string_view kValidEmptyXml =
      "<?xml version=\"1.0\"?>\n<empty/>\n";

  const TempFile item_file("coda_xml_item.xml");
  ASSERT_TRUE(item_file.WriteText(kValidItemXml));
  const TempFile empty_file("coda_xml_empty_schema.xml");
  ASSERT_TRUE(empty_file.WriteText(kValidEmptyXml));

  // 1. coda_recognize_file() resolves the XML product class/type/version.
  {
    int64_t file_size = -1;
    coda_format fmt = coda_format_binary;
    const char* pclass = nullptr;
    const char* ptype = nullptr;
    int version = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_recognize_file(item_file.c_str(), &file_size, &fmt, &pclass,
                                  &ptype, &version));
    EXPECT_EQ(static_cast<int64_t>(kValidItemXml.size()), file_size);
    EXPECT_EQ(coda_format_xml, fmt);
    EXPECT_STREQ("XmlTestClass", pclass);
    EXPECT_STREQ("XmlItemType", ptype);
    EXPECT_EQ(1, version);
  }

  // 2. Public coda_open() in both mmap and fd modes applies XmlItemProd and
  //    parses typed integer, double, text, attribute, and optional array
  //    fields.
  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(item_file.c_str(), product.OutParam()));

    coda_format fmt = coda_format_binary;
    const char* pclass = nullptr;
    const char* ptype = nullptr;
    int version = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &fmt));
    EXPECT_EQ(coda_format_xml, fmt);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_class(product.get(), &pclass));
    EXPECT_STREQ("XmlTestClass", pclass);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_type(product.get(), &ptype));
    EXPECT_STREQ("XmlItemType", ptype);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_version(product.get(), &version));
    EXPECT_EQ(1, version);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

    coda_cursor c = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/item@id"));
    EXPECT_EQ("rec-42", ReadCursorString(&c));

    c = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/item/name"));
    EXPECT_EQ("sensor_a", ReadCursorString(&c));

    c = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/item/count"));
    int32_t count_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &count_val));
    EXPECT_EQ(-17, count_val);

    c = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/item/scale"));
    double scale_val = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &scale_val));
    EXPECT_DOUBLE_EQ(1.25, scale_val);

    c = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/item/tag[0]"));
    EXPECT_EQ("alpha", ReadCursorString(&c));
    c = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/item/tag[1]"));
    EXPECT_EQ("beta", ReadCursorString(&c));
  }

  // 3. Direct coda_xml_reopen_with_definition() with mem_ptr != NULL
  // (item_file)
  //    and with mem_ptr == NULL (empty_file).
  coda_product_definition* item_def = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "XmlTestClass", "XmlItemType", 1, &item_def));
  ASSERT_NE(nullptr, item_def);
  if (!item_def->initialized) {
    ASSERT_EQ(CODA_SUCCESS, coda_read_product_definition(item_def));
  }

  coda_product_definition* empty_def = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "XmlTestClass", "XmlEmptyType", 1, &empty_def));
  ASSERT_NE(nullptr, empty_def);
  if (!empty_def->initialized) {
    ASSERT_EQ(CODA_SUCCESS, coda_read_product_definition(empty_def));
  }

  {
    coda_product* raw = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(item_file.c_str(),
                            static_cast<int64_t>(kValidItemXml.size()), &raw));
    ASSERT_EQ(CODA_SUCCESS, coda_xml_reopen(&raw));
    ScopedProduct xml_holder(raw);

    auto* xml_prod = reinterpret_cast<coda_xml_product*>(raw);
    ASSERT_NE(nullptr, xml_prod->mem_ptr);

    ASSERT_EQ(CODA_SUCCESS, coda_xml_reopen_with_definition(&raw, item_def));
    EXPECT_EQ(item_def, xml_prod->product_definition);
    EXPECT_NE(nullptr, xml_prod->root_type);
    EXPECT_NE(nullptr, xml_prod->mem_ptr);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_xml_cursor_set_product(&cursor, raw));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/item/count"));
    int32_t count_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &count_val));
    EXPECT_EQ(-17, count_val);
  }

  {
    coda_product* raw = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(empty_file.c_str(),
                            static_cast<int64_t>(kValidEmptyXml.size()), &raw));
    ASSERT_EQ(CODA_SUCCESS, coda_xml_reopen(&raw));
    ScopedProduct xml_holder(raw);

    auto* xml_prod = reinterpret_cast<coda_xml_product*>(raw);
    EXPECT_EQ(nullptr, xml_prod->mem_ptr);

    ASSERT_EQ(CODA_SUCCESS, coda_xml_reopen_with_definition(&raw, empty_def));
    EXPECT_EQ(empty_def, xml_prod->product_definition);
    EXPECT_NE(nullptr, xml_prod->root_type);
    EXPECT_EQ(nullptr, xml_prod->mem_ptr);
  }
}

TEST(CodaXmlTest, XmlParseAndSchemaValidationErrorPaths) {
  const ScopedXmlTestEnv test_env;
  const TempFile codadef_file("coda_xml_errors.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildXmlTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Schemaless parse errors through coda_open() and direct
  // coda_xml_reopen().
  std::string deep_xml = "<?xml version=\"1.0\"?>\n";
  for (int i = 0; i < CODA_CURSOR_MAXDEPTH; ++i) {
    absl::StrAppend(&deep_xml, "<n", i, ">");
  }
  for (int i = CODA_CURSOR_MAXDEPTH - 1; i >= 0; --i) {
    absl::StrAppend(&deep_xml, "</n", i, ">");
  }
  deep_xml += "\n";

  struct ParseErrorCase {
    std::string_view name;
    std::string content;
    int expected_errno;
  };
  const ParseErrorCase kParseErrors[] = {
      {.name = "unclosed_tag.xml",
       .content = "<?xml version=\"1.0\"?>\n<root><child>",
       .expected_errno = CODA_ERROR_XML},
      {.name = "malformed_after_text.xml",
       .content = "<?xml version=\"1.0\"?>\n<root a=\"v\"><x>abc</x><bad>",
       .expected_errno = CODA_ERROR_XML},
      {.name = "mismatched_closing_tag.xml",
       .content = "<?xml version=\"1.0\"?>\n<root><child></other></root>\n",
       .expected_errno = CODA_ERROR_XML},
      {.name = "mixed_content.xml",
       .content = "<?xml version=\"1.0\"?>\n<root><child/>text</root>\n",
       .expected_errno = CODA_ERROR_PRODUCT},
      {.name = "exceeds_max_depth.xml",
       .content = deep_xml,
       .expected_errno = CODA_ERROR_PRODUCT},
  };

  for (const ParseErrorCase& tc : kParseErrors) {
    SCOPED_TRACE(tc.name);
    const TempFile file(tc.name);
    ASSERT_TRUE(file.WriteText(tc.content));

    // Public coda_open() path.
    {
      ScopedProduct product;
      coda_set_error(CODA_SUCCESS, nullptr);
      EXPECT_EQ(-1, coda_open(file.c_str(), product.OutParam()));
      EXPECT_EQ(tc.expected_errno, coda_errno);
      EXPECT_EQ(nullptr, product.get());
    }

    // Direct coda_xml_reopen() path (coda_xml_reopen frees the input product
    // via coda_close() when coda_xml_parse() fails).
    {
      coda_product* raw = nullptr;
      ASSERT_EQ(CODA_SUCCESS,
                coda_bin_open(file.c_str(),
                              static_cast<int64_t>(tc.content.size()), &raw));
      coda_set_error(CODA_SUCCESS, nullptr);
      const int reopen_rc = coda_xml_reopen(&raw);
      EXPECT_EQ(-1, reopen_rc);
      EXPECT_EQ(tc.expected_errno, coda_errno);
      if (reopen_rc == CODA_SUCCESS) {
        coda_close(raw);
      }
    }
  }

  // 2. Schema-guided mismatches via coda_open_as() and direct
  //    coda_xml_reopen_with_definition() (exercises error return at line 108
  //    and root_type == NULL + mem_ptr == NULL cleanup in coda_xml_close()).
  coda_product_definition* item_def = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "XmlTestClass", "XmlItemType", 1, &item_def));
  ASSERT_NE(nullptr, item_def);
  if (!item_def->initialized) {
    ASSERT_EQ(CODA_SUCCESS, coda_read_product_definition(item_def));
  }

  constexpr std::array<std::string_view, 4> kSchemaMismatches = {
      "<?xml version=\"1.0\"?>\n<other>bad_root</other>\n",
      "<?xml version=\"1.0\"?>\n"
      "<item><name>missing_id_attr</name><count>1</count><scale>1.0</scale></"
      "item>\n",
      "<?xml version=\"1.0\"?>\n"
      "<item id=\"1\" "
      "extra=\"bad\"><name>x</name><count>1</count><scale>1.0</scale></item>\n",
      "<?xml version=\"1.0\"?>\n"
      "<item "
      "id=\"1\">non_ws_in_record<name>x</name><count>1</count><scale>1.0</"
      "scale></item>\n",
  };

  for (size_t i = 0; i < kSchemaMismatches.size(); ++i) {
    SCOPED_TRACE(i);
    const std::string fname = absl::StrCat("coda_xml_mismatch_", i, ".xml");
    const TempFile file(fname);
    ASSERT_TRUE(file.WriteText(kSchemaMismatches[i]));

    ScopedProduct open_as_prod;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open_as(file.c_str(), "XmlTestClass", "XmlItemType", 1,
                               open_as_prod.OutParam()));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    EXPECT_EQ(nullptr, open_as_prod.get());

    coda_product* raw = nullptr;
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_bin_open(file.c_str(),
                      static_cast<int64_t>(kSchemaMismatches[i].size()), &raw));
    ASSERT_EQ(CODA_SUCCESS, coda_xml_reopen(&raw));
    ScopedProduct xml_holder(raw);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_xml_reopen_with_definition(&raw, item_def));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    EXPECT_EQ(nullptr, reinterpret_cast<coda_xml_product*>(raw)->root_type);
    if (i == 0 || i == 1) {
      EXPECT_EQ(nullptr, reinterpret_cast<coda_xml_product*>(raw)->mem_ptr);
    } else {
      EXPECT_NE(nullptr, reinterpret_cast<coda_xml_product*>(raw)->mem_ptr);
    }
    // ScopedProduct calls coda_close() -> coda_xml_close().
  }
}

// TODO(schwehr): 91.3% reachable line coverage in libcoda/coda-xml.c (63/69);
// remaining 6 missed lines (52, 54-55, 71, 73-74) are malloc/strdup OOM
// branches in coda_xml_reopen().

}  // namespace
}  // namespace stcorp_coda
