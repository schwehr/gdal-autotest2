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
// Tests CODA's Expat-backed XML parser, schemaless type inference, .codadef
// schema matching, namespace/attribute handling, multi-block mmap/fd reading,
// and error paths (libcoda/coda-xml-parser.c).
//
// google3 build caveats:
//   - libcoda defines XML_DTD=1, XML_NS=1, XML_LARGE_SIZE=1, and
//     XML_CONTEXT_BYTES=1024 in config.h while linking third_party/cpp/expat;
//     because CODA registers XML_PARAM_ENTITY_PARSING_ALWAYS without an
//     ExternalEntityRefHandler, Expat does not invoke not_standalone_handler().
//   - coda-xml-parser.c is compiled into //third_party/stcorp_coda; direct
//     coda_xml_parse() error tests include coda_internal_prelude.h before
//     extern "C" { coda-bin-internal.h, coda-xml-internal.h }.
//   - Verified 100.0% reachable coverage ceiling (458/512 = 89.5%, 54 missed):
//     41 malloc/realloc/Expat OOM branches, 3 Expat not_standalone_handler()
//     lines (315, 317, 320, unreachable without ExternalEntityRefHandler), and
//     10 unreachable defensive guards on freshly created/indexed schema nodes
//     (153-155, 418-419, 459-461, 873-874).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-xml-parser.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-xml-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-xml.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
#include "third_party/stcorp_coda/libcoda/coda-xml-internal.h"
}  // extern "C"

#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

// Isolates CODA definition path, environment variables, coda_option_use_mmap,
// and thread-local coda_errno across every test.
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

// Reads the string value at `cursor` (which must point to a text node).
std::string ReadCursorString(const coda_cursor& cursor) {
  long length = -1;
  if (coda_cursor_get_string_length(&cursor, &length) != CODA_SUCCESS ||
      length < 0) {
    ADD_FAILURE() << "coda_cursor_get_string_length failed: "
                  << coda_errno_to_string(coda_errno);
    return "";
  }
  std::string value(static_cast<size_t>(length) + 1, '\0');
  EXPECT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, value.data(), length + 1));
  value.resize(static_cast<size_t>(length));
  return value;
}

// Moves a copy of `base` to `path` and reads its string value.
std::string ReadStringAt(const coda_cursor& base, const char* path) {
  coda_cursor cursor = base;
  if (coda_cursor_goto(&cursor, path) != CODA_SUCCESS) {
    ADD_FAILURE() << "coda_cursor_goto(" << path
                  << ") failed: " << coda_errno_to_string(coda_errno);
    return "";
  }
  return ReadCursorString(cursor);
}

// Moves a copy of `base` to attribute `attr_name` and reads its string value.
std::string ReadAttributeAt(const coda_cursor& base, const char* attr_name) {
  coda_cursor cursor = base;
  if (coda_cursor_goto_attributes(&cursor) != CODA_SUCCESS ||
      coda_cursor_goto_record_field_by_name(&cursor, attr_name) !=
          CODA_SUCCESS) {
    ADD_FAILURE() << "goto attribute '" << attr_name
                  << "' failed: " << coda_errno_to_string(coda_errno);
    return "";
  }
  return ReadCursorString(cursor);
}

TEST(CodaXmlParserTest, SchemalessBasicStructureCommentsPiCdataAndEntities) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  const TempFile xml_file("coda_xml_parser_basic.xml");
  constexpr std::string_view kXml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<!-- top-level comment -->
<?custom-pi target="ignored"?>
<root>
  <!-- inline comment -->
  <empty_self/>
  <empty_ws>   	
  </empty_ws>
  <plain>hello world</plain>
  <tagged kind="meta" empty_attr="">with_attr</tagged>
  <entities>&lt;&gt;&amp;&apos;&quot;&#123;&#x41;&#x09;</entities>
  <cdata><![CDATA[raw <tag> & text )]]></cdata>
  <nested>
    <leaf>inner</leaf>
  </nested>
</root>
)xml";
  ASSERT_TRUE(xml_file.WriteText(kXml));

  int64_t recognized_size = -1;
  coda_format recognized_format = coda_format_binary;
  const char* product_class = "sentinel";
  const char* product_type = "sentinel";
  int version = 99;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(xml_file.c_str(), &recognized_size,
                                &recognized_format, &product_class,
                                &product_type, &version));
  EXPECT_EQ(static_cast<int64_t>(kXml.size()), recognized_size);
  EXPECT_EQ(coda_format_xml, recognized_format);
  EXPECT_EQ(nullptr, product_class);
  EXPECT_EQ(nullptr, product_type);
  EXPECT_EQ(-1, version);

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_format format = coda_format_binary;
  ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
  EXPECT_EQ(coda_format_xml, format);

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // Empty and whitespace-only leaf elements stay empty records in schemaless
  // mode.
  for (const char* empty_path : {"/root/empty_self", "/root/empty_ws"}) {
    coda_cursor c = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, empty_path));
    coda_type_class tc = coda_text_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&c, &tc));
    EXPECT_EQ(coda_record_class, tc);
    long num_elements = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &num_elements));
    EXPECT_EQ(0, num_elements);
  }

  // convert_to_text() with and without attributes, entity decoding, and CDATA.
  EXPECT_EQ("hello world", ReadStringAt(root, "/root/plain"));
  EXPECT_EQ("with_attr", ReadStringAt(root, "/root/tagged"));
  EXPECT_EQ("inner", ReadStringAt(root, "/root/nested/leaf"));
  EXPECT_EQ("<>&'\"{A\t", ReadStringAt(root, "/root/entities"));
  EXPECT_EQ("raw <tag> & text )", ReadStringAt(root, "/root/cdata"));

  coda_cursor tagged = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&tagged, "/root/tagged"));
  int has_attrs = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&tagged, &has_attrs));
  EXPECT_EQ(1, has_attrs);
  EXPECT_EQ("meta", ReadAttributeAt(tagged, "kind"));
  EXPECT_EQ("", ReadAttributeAt(tagged, "empty_attr"));
}

TEST(CodaXmlParserTest, Utf8BomAndInternalDoctypeEntity) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  const TempFile xml_file("coda_xml_parser_bom_dtd.xml");
  const std::string xml = std::string("\xEF\xBB\xBF") +
                          R"xml(<?xml version="1.0" standalone="yes"?>
<!DOCTYPE root [
  <!ENTITY greeting "hello_from_entity">
]>
<root><msg>&greeting;</msg></root>
)xml";
  ASSERT_TRUE(xml_file.WriteText(xml));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
  EXPECT_EQ("hello_from_entity", ReadStringAt(root, "/root/msg"));
}

TEST(CodaXmlParserTest, SchemalessRepeatedElementsAndAttributes) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  const TempFile xml_file("coda_xml_parser_repeated.xml");
  // Exercises:
  // - Scalar-to-array conversion on 2nd <item>, and array reuse on 3rd <item>.
  // - Attribute present on all elements (a), missing on 2nd so marked optional
  //   (b), and newly added on 3rd so marked optional during
  //   coda_mem_type_update (c).
  // - Duplicate local attribute name via namespace prefix (a="1" ns:a="2" keeps
  //   "1") and distinct namespaced attribute before unnamespaced (ns:k vs k).
  constexpr std::string_view kXml = R"xml(<?xml version="1.0"?>
<root xmlns:ns="urn:attr-ns">
  <item a="1" b="2" ns:a="ignored" ns:k="ns_first" k="plain_second">first</item>
  <item a="3">second</item>
  <item a="4" c="5">third</item>
  <empty_rep/>
  <empty_rep/>
</root>
)xml";
  ASSERT_TRUE(xml_file.WriteText(kXml));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  coda_cursor items = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&items, "/root/item"));
  coda_type_class tc = coda_text_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&items, &tc));
  EXPECT_EQ(coda_array_class, tc);

  long num_items = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&items, &num_items));
  ASSERT_EQ(3, num_items);

  // Check element values and attributes on item[0], item[1], item[2].
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&items, 0));
  EXPECT_EQ("first", ReadCursorString(items));
  EXPECT_EQ("1", ReadAttributeAt(items, "a"));
  EXPECT_EQ("2", ReadAttributeAt(items, "b"));
  EXPECT_EQ("ns_first", ReadAttributeAt(items, "k"));
  EXPECT_EQ("plain_second", ReadAttributeAt(items, "k_1"));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&items));
  EXPECT_EQ("second", ReadCursorString(items));
  EXPECT_EQ("3", ReadAttributeAt(items, "a"));
  {
    coda_cursor attrs = items;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attrs));
    long idx_b = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_index_from_name(
                                &attrs, "b", &idx_b));
    int avail_b = 1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                                &attrs, idx_b, &avail_b));
    EXPECT_EQ(0, avail_b);
  }

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&items));
  EXPECT_EQ("third", ReadCursorString(items));
  EXPECT_EQ("4", ReadAttributeAt(items, "a"));
  EXPECT_EQ("5", ReadAttributeAt(items, "c"));

  // Repeated empty records also form an array of empty records.
  coda_cursor reps = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&reps, "/root/empty_rep"));
  long num_reps = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&reps, &num_reps));
  EXPECT_EQ(2, num_reps);
}

TEST(CodaXmlParserTest, SchemalessNamespacesDefaultAndPrefixed) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  const TempFile xml_file("coda_xml_parser_ns.xml");
  constexpr std::string_view kXml = R"xml(<?xml version="1.0"?>
<root xmlns="urn:default" xmlns:p="urn:pref">
  <child>in_default</child>
  <p:item>pref_one</p:item>
  <p:item>pref_two</p:item>
  <unscoped xmlns="">no_ns</unscoped>
</root>
)xml";
  ASSERT_TRUE(xml_file.WriteText(kXml));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  coda_cursor c = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root"));
  EXPECT_EQ("urn:default", ReadAttributeAt(c, "xmlns"));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root/child"));
  EXPECT_EQ("in_default", ReadCursorString(c));
  EXPECT_EQ("urn:default", ReadAttributeAt(c, "xmlns"));

  // Repeated prefixed element hits both attribute_index < 0 and >= 0 for
  // "xmlns" in attribute_record_new().
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root/item[0]"));
  EXPECT_EQ("pref_one", ReadCursorString(c));
  EXPECT_EQ("urn:pref", ReadAttributeAt(c, "xmlns"));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root/item[1]"));
  EXPECT_EQ("pref_two", ReadCursorString(c));
  EXPECT_EQ("urn:pref", ReadAttributeAt(c, "xmlns"));

  // Element resetting xmlns="" has no attributes.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root/unscoped"));
  EXPECT_EQ("no_ns", ReadCursorString(c));
  int has_attrs = 1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&c, &has_attrs));
  EXPECT_EQ(0, has_attrs);
}

TEST(CodaXmlParserTest, LargeMultiBlockXmlMmapAndFileDescriptorModes) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // Exceed both DATA_BLOCK_SIZE (4096) and BUFFSIZE (8192) across 3 blocks so
  // both i < num_blocks - 1 and i == num_blocks - 1 execute in coda_xml_parse()
  // and character_data_handler() grows info->value repeatedly via realloc().
  const std::string payload(17000, 'X');
  const std::string xml = std::string(R"xml(<?xml version="1.0"?>
<root><big>)xml") + payload +
                          R"xml(</big><tail>done</tail></root>
)xml";

  const TempFile xml_file("coda_xml_parser_large.xml");
  ASSERT_TRUE(xml_file.WriteText(xml));

  for (int use_mmap : {1, 0}) {
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    EXPECT_EQ(payload, ReadStringAt(root, "/root/big"));
    EXPECT_EQ("done", ReadStringAt(root, "/root/tail"));
  }
}

TEST(CodaXmlParserTest, SchemaMatchedXmlTypesAttributesArraysAndTime) {
  const ScopedXmlTestEnv test_env;

  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="XmlSchemaClass">
  <cd:ProductType name="XmlSchemaType">
    <cd:ProductDefinition id="XmlSchemaProd" format="xml" version="1">
      <cd:DetectionRule>
        <cd:MatchData path="/doc@kind">schema_test</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="XmlSchemaProd" format="xml">
  <cd:Record>
    <cd:Field name="doc">
      <cd:Record>
        <cd:Attribute name="kind"><cd:Text/></cd:Attribute>
        <cd:Attribute name="xmlns"><cd:Text/></cd:Attribute>
        <cd:Attribute name="opt_attr"><cd:Optional/><cd:Text/></cd:Attribute>
        <cd:Attribute name="ns_attr"><cd:Optional/><cd:Text/></cd:Attribute>
        <cd:Attribute name="full_ns_attr" real_name="urn:attr-ns exact"><cd:Optional/><cd:Text/></cd:Attribute>
        <cd:Field name="count"><cd:Integer format="ascii"/></cd:Field>
        <cd:Field name="ratio"><cd:Float format="ascii"/></cd:Field>
        <cd:Field name="label"><cd:Text/></cd:Field>
        <cd:Field name="stamp">
          <cd:Time timeformat='time(str(.),"yyyy-MM-dd")'><cd:Text/></cd:Time>
        </cd:Field>
        <cd:Field name="ns_without_xmlns">
          <cd:Record>
            <cd:Attribute name="tag"><cd:Text/></cd:Attribute>
            <cd:Field name="v"><cd:Text/></cd:Field>
          </cd:Record>
        </cd:Field>
        <cd:Field name="entries">
          <cd:Array>
            <cd:Dimension/>
            <cd:Record>
              <cd:Field name="val"><cd:Integer format="ascii"/></cd:Field>
            </cd:Record>
          </cd:Array>
        </cd:Field>
        <cd:Field name="opt_empty_arr">
          <cd:Optional/>
          <cd:Array>
            <cd:Dimension/>
            <cd:Text/>
          </cd:Array>
        </cd:Field>
        <cd:Field name="fixed_arr">
          <cd:Array>
            <cd:Dimension>2</cd:Dimension>
            <cd:Text/>
          </cd:Array>
        </cd:Field>
      </cd:Record>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 1> entries = {
      {{"products/XmlSchemaProd.xml", kProdXml}}};
  const TempFile codadef("coda_xml_parser_schema.codadef");
  ASSERT_TRUE(codadef.WriteBytes(BuildCodadefBytes(kIndexXml, entries)));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));

  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  const TempFile xml_file("coda_xml_parser_schema_data.xml");
  constexpr std::string_view kDataXml = R"xml(<?xml version="1.0"?>
<doc xmlns="urn:schema" xmlns:a="urn:attr-ns" kind="schema_test" a:ns_attr="stripped_match" a:exact="full_match">
  <count>42</count>
  <ratio>3.5</ratio>
  <label>hello_schema</label>
  <stamp>2000-01-02</stamp>
  <ns_without_xmlns tag="ok"><v>child</v></ns_without_xmlns>
  <entries><val>10</val></entries>
  <entries><val>20</val></entries>
  <fixed_arr>alpha</fixed_arr>
  <fixed_arr>beta</fixed_arr>
</doc>
)xml";
  ASSERT_TRUE(xml_file.WriteText(kDataXml));

  const char* prod_class = nullptr;
  const char* prod_type = nullptr;
  int version = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(xml_file.c_str(), nullptr, nullptr, &prod_class,
                                &prod_type, &version));
  EXPECT_STREQ("XmlSchemaClass", prod_class);
  EXPECT_STREQ("XmlSchemaType", prod_type);
  EXPECT_EQ(1, version);

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  coda_cursor doc = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&doc, "/doc"));
  EXPECT_EQ("schema_test", ReadAttributeAt(doc, "kind"));
  EXPECT_EQ("urn:schema", ReadAttributeAt(doc, "xmlns"));
  EXPECT_EQ("stripped_match", ReadAttributeAt(doc, "ns_attr"));
  EXPECT_EQ("full_match", ReadAttributeAt(doc, "full_ns_attr"));

  {
    coda_cursor attrs = doc;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attrs));
    long opt_idx = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_index_from_name(
                                &attrs, "opt_attr", &opt_idx));
    int opt_avail = 1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                                &attrs, opt_idx, &opt_avail));
    EXPECT_EQ(0, opt_avail);
  }

  {
    coda_cursor c = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/doc/count"));
    int64_t count = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &count));
    EXPECT_EQ(42, count);
  }
  {
    coda_cursor c = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/doc/ratio"));
    double ratio = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &ratio));
    EXPECT_DOUBLE_EQ(3.5, ratio);
  }
  EXPECT_EQ("hello_schema", ReadStringAt(root, "/doc/label"));

  // Special time type in XML (2000-01-02 is 86400s since 2000-01-01).
  {
    coda_cursor c = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/doc/stamp"));
    coda_type_class tc = coda_text_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&c, &tc));
    EXPECT_EQ(coda_special_class, tc);
    double stamp = -1.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &stamp));
    EXPECT_DOUBLE_EQ(86400.0, stamp);
  }

  // Element with namespace whose schema attributes lack "xmlns".
  EXPECT_EQ("child", ReadStringAt(root, "/doc/ns_without_xmlns/v"));
  {
    coda_cursor ns_el = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&ns_el, "/doc/ns_without_xmlns"));
    EXPECT_EQ("ok", ReadAttributeAt(ns_el, "tag"));
  }

  // Dynamic and fixed arrays.
  {
    coda_cursor c = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/doc/entries[0]/val"));
    int64_t v0 = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &v0));
    EXPECT_EQ(10, v0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/doc/entries[1]/val"));
    int64_t v1 = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &v1));
    EXPECT_EQ(20, v1);
  }
  EXPECT_EQ("alpha", ReadStringAt(root, "/doc/fixed_arr[0]"));
  EXPECT_EQ("beta", ReadStringAt(root, "/doc/fixed_arr[1]"));

  // Optional empty array was pruned at end_element_handler().
  long opt_arr_idx = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_index_from_name(
                              &doc, "opt_empty_arr", &opt_arr_idx));
  int opt_arr_avail = 1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &doc, opt_arr_idx, &opt_arr_avail));
  EXPECT_EQ(0, opt_arr_avail);
}

TEST(CodaXmlParserTest, SchemaValidationErrorPaths) {
  const ScopedXmlTestEnv test_env;

  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="XmlErrClass">
  <cd:ProductType name="XmlErrType">
    <cd:ProductDefinition id="XmlErrProd" format="xml" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="XmlErrProd" format="xml">
  <cd:Record>
    <cd:Field name="doc">
      <cd:Record>
        <cd:Attribute name="req_attr"><cd:Text/></cd:Attribute>
        <cd:Field name="scalar">
          <cd:Text/>
        </cd:Field>
        <cd:Field name="leaf_with_attr">
          <cd:Optional/>
          <cd:Text>
            <cd:Attribute name="a"><cd:Text/></cd:Attribute>
          </cd:Text>
        </cd:Field>
        <cd:Field name="fixed4">
          <cd:Optional/>
          <cd:Text><cd:ByteSize>4</cd:ByteSize></cd:Text>
        </cd:Field>
        <cd:Field name="fixed_time">
          <cd:Optional/>
          <cd:Time timeformat='time(str(.),"yyyy-MM-dd")'>
            <cd:Text><cd:ByteSize>10</cd:ByteSize></cd:Text>
          </cd:Time>
        </cd:Field>
        <cd:Field name="pair">
          <cd:Optional/>
          <cd:Array>
            <cd:Dimension>2</cd:Dimension>
            <cd:Text/>
          </cd:Array>
        </cd:Field>
      </cd:Record>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 1> entries = {
      {{"products/XmlErrProd.xml", kProdXml}}};
  const TempFile codadef("coda_xml_parser_errors.codadef");
  ASSERT_TRUE(codadef.WriteBytes(BuildCodadefBytes(kIndexXml, entries)));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));

  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  struct ErrorCase {
    std::string_view name;
    std::string_view xml;
    int expected_errno;
    std::string_view expected_substr;
  };
  constexpr ErrorCase kCases[] = {
      {
          .name = "disallowed_root",
          .xml = R"xml(<?xml version="1.0"?><wrong/>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "not allowed as root element",
      },
      {
          .name = "disallowed_child",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok"><unknown/></doc>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "not allowed within element",
      },
      {
          .name = "repeated_scalar",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok"><scalar>1</scalar><scalar>2</scalar></doc>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "not allowed more than once",
      },
      {
          .name = "disallowed_attr_when_none_defined",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok"><scalar bad="1">a</scalar></doc>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "xml attribute 'bad' is not allowed",
      },
      {
          .name = "disallowed_attr_when_attrs_defined",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok" extra="1"><scalar>a</scalar></doc>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "xml attribute 'extra' is not allowed",
      },
      {
          .name = "missing_mandatory_attr",
          .xml = R"xml(<?xml version="1.0"?><doc><scalar>a</scalar></doc>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "mandatory xml attribute 'req_attr' is missing",
      },
      {
          .name = "missing_mandatory_child",
          .xml = R"xml(<?xml version="1.0"?><doc req_attr="ok"/>)xml",
          .expected_errno = CODA_ERROR_DATA_DEFINITION,
          .expected_substr = "mandatory field 'scalar' is missing",
      },
      {
          .name = "non_whitespace_in_record",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok">oops<scalar>a</scalar></doc>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "non-whitespace character data not allowed",
      },
      {
          .name = "child_inside_leaf_with_attr",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok"><scalar>a</scalar><leaf_with_attr a="1"><nested/></leaf_with_attr></doc>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "mixed content for element 'leaf_with_attr'",
      },
      {
          .name = "fixed_byte_size_mismatch",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok"><scalar>a</scalar><fixed4>too_long</fixed4></doc>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "does not match that of definition",
      },
      {
          .name = "special_time_base_size_mismatch",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok"><scalar>a</scalar><fixed_time>short</fixed_time></doc>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "does not match that of definition",
      },
      {
          .name = "fixed_array_too_few_elements",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok"><scalar>a</scalar><pair>one</pair></doc>)xml",
          .expected_errno = CODA_ERROR_DATA_DEFINITION,
          .expected_substr = "array element '1' is missing",
      },
      {
          .name = "fixed_array_too_many_elements",
          .xml =
              R"xml(<?xml version="1.0"?><doc req_attr="ok"><scalar>a</scalar><pair>1</pair><pair>2</pair><pair>3</pair></doc>)xml",
          .expected_errno = CODA_ERROR_INVALID_INDEX,
          .expected_substr = "array index (2) is not in the range",
      },
  };

  for (const ErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const TempFile xml_file("coda_xml_parser_err_case.xml");
    ASSERT_TRUE(xml_file.WriteText(tc.xml));

    coda_set_error(CODA_SUCCESS, nullptr);
    ScopedProduct product;
    EXPECT_EQ(-1, coda_open_as(xml_file.c_str(), "XmlErrClass", "XmlErrType", 1,
                               product.OutParam()));
    EXPECT_EQ(tc.expected_errno, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr(tc.expected_substr));
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST(CodaXmlParserTest, SchemalessErrorPathsAndMaxDepth) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // 1. Non-existent file fails with CODA_ERROR_FILE_NOT_FOUND.
  {
    const TempFile missing("coda_xml_parser_missing.xml");
    coda_set_error(CODA_SUCCESS, nullptr);
    ScopedProduct product;
    EXPECT_EQ(-1, coda_open(missing.c_str(), product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  // 2. Mixed content in schemaless mode (element has both child and text).
  {
    const TempFile xml_file("coda_xml_parser_mixed.xml");
    ASSERT_TRUE(xml_file.WriteText(
        R"xml(<?xml version="1.0"?><root><child/>mixed</root>)xml"));
    coda_set_error(CODA_SUCCESS, nullptr);
    ScopedProduct product;
    EXPECT_EQ(-1, coda_open(xml_file.c_str(), product.OutParam()));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("mixed content for element 'root'"));
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  // 3. Depth boundary: CODA_CURSOR_MAXDEPTH - 1 (31) element nesting succeeds,
  //    and 32 element nesting exceeds CODA_CURSOR_MAXDEPTH.
  auto make_nested_xml = [](int depth) {
    std::string xml = R"xml(<?xml version="1.0"?>)xml";
    for (int i = 0; i < depth; ++i) {
      xml += "<n" + std::to_string(i) + ">";
    }
    xml += "leaf";
    for (int i = depth - 1; i >= 0; --i) {
      xml += "</n" + std::to_string(i) + ">";
    }
    return xml;
  };
  {
    const TempFile ok_file("coda_xml_parser_depth_ok.xml");
    ASSERT_TRUE(ok_file.WriteText(make_nested_xml(CODA_CURSOR_MAXDEPTH - 1)));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(ok_file.c_str(), product.OutParam()));
  }
  {
    const TempFile deep_file("coda_xml_parser_depth_fail.xml");
    ASSERT_TRUE(deep_file.WriteText(make_nested_xml(CODA_CURSOR_MAXDEPTH)));
    coda_set_error(CODA_SUCCESS, nullptr);
    ScopedProduct product;
    EXPECT_EQ(-1, coda_open(deep_file.c_str(), product.OutParam()));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("exceeds maximum supported hierarchical depth"));
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  // 4. Malformed XML syntax errors trigger CODA_ERROR_XML with line/byte info.
  constexpr std::string_view kMalformedCases[] = {
      R"xml(<?xml version="1.0"?><root><unclosed></root>)xml",
      R"xml(<?xml version="1.0"?><root>&#x1f;</root>)xml",
      R"xml(<?xml version="1.0"?><root><trunc)xml",
      R"xml(<?xml version="1.0"?>)xml",
  };
  for (std::string_view bad_xml : kMalformedCases) {
    const TempFile bad_file("coda_xml_parser_malformed.xml");
    ASSERT_TRUE(bad_file.WriteText(bad_xml));
    coda_set_error(CODA_SUCCESS, nullptr);
    ScopedProduct product;
    EXPECT_EQ(-1, coda_open(bad_file.c_str(), product.OutParam()));
    EXPECT_EQ(CODA_ERROR_XML, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("xml parse error:"));
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("byte offset:"));
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST(CodaXmlParserTest,
     DirectCodaXmlParseHandlesFileReadErrorsAndNullRootType) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // 1. lseek() failure on invalid file descriptor in non-mmap mode.
  {
    coda_bin_product raw{};
    raw.format = coda_format_binary;
    raw.file_size = 64;
    raw.use_mmap = 0;
    raw.fd = -1;

    coda_xml_product xml{};
    xml.format = coda_format_xml;
    xml.raw_product = reinterpret_cast<coda_product*>(&raw);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_xml_parse(&xml));
    EXPECT_EQ(CODA_ERROR_FILE_READ, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("could not move to byte position"));
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  // 2. read() failure on a write-only file descriptor in non-mmap mode.
  {
    const TempFile wronly_file("coda_xml_parser_wronly.xml");
    ASSERT_TRUE(wronly_file.WriteText("<root/>"));
    const int fd = open(wronly_file.c_str(), O_WRONLY);
    ASSERT_GE(fd, 0);
    absl::Cleanup close_fd([fd] { close(fd); });

    coda_bin_product raw{};
    raw.format = coda_format_binary;
    raw.file_size = 7;
    raw.use_mmap = 0;
    raw.fd = fd;

    coda_xml_product xml{};
    xml.format = coda_format_xml;
    xml.raw_product = reinterpret_cast<coda_product*>(&raw);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_xml_parse(&xml));
    EXPECT_EQ(CODA_ERROR_FILE_READ, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("could not read from file"));
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  // 3. product_definition != NULL with root_type == NULL falls back to
  //    update_definition == 1.
  {
    constexpr char kBuf[] = R"xml(<?xml version="1.0"?><doc><k>v</k></doc>)xml";
    coda_bin_product raw{};
    raw.format = coda_format_binary;
    raw.file_size = static_cast<int64_t>(sizeof(kBuf) - 1);
    raw.use_mmap = 1;
    raw.mem_size = raw.file_size;
    raw.mem_ptr = reinterpret_cast<const uint8_t*>(kBuf);

    coda_product_definition def{};
    def.format = coda_format_xml;
    def.root_type = nullptr;

    coda_xml_product xml{};
    xml.format = coda_format_xml;
    xml.product_definition = &def;
    xml.raw_product = reinterpret_cast<coda_product*>(&raw);

    absl::Cleanup cleanup_xml([&xml] {
      if (xml.root_type != nullptr) {
        coda_dynamic_type_delete(xml.root_type);
      }
      if (xml.mem_ptr != nullptr) {
        std::free(xml.mem_ptr);
      }
    });

    ASSERT_EQ(CODA_SUCCESS, coda_xml_parse(&xml));
    ASSERT_NE(nullptr, xml.root_type);
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(
                                &root, reinterpret_cast<coda_product*>(&xml)));
    EXPECT_EQ("v", ReadStringAt(root, "/doc/k"));
  }
}

TEST(CodaXmlParserTest, SchemaRootArrayFieldAndXmlnsAndAttributeFailurePaths) {
  const ScopedXmlTestEnv test_env;

  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="XmlAdvClass">
  <cd:ProductType name="XmlRootArrayType">
    <cd:ProductDefinition id="XmlRootArrayProd" format="xml" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="XmlAdvErrType">
    <cd:ProductDefinition id="XmlAdvErrProd" format="xml" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  // Covers root-level XML array initialization in start_element_handler() when
  // info.record[0] has not pre-allocated field_type[index].
  constexpr std::string_view kRootArrayProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="XmlRootArrayProd" format="xml">
  <cd:Record>
    <cd:Field name="doc">
      <cd:Array>
        <cd:Dimension/>
        <cd:Text/>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  // Covers attribute_record_new() failures on xmlns length mismatch, attribute
  // length mismatch, duplicate attribute after namespace stripping, and
  // end_element_handler() non-time coda_special_class rejection.
  constexpr std::string_view kAdvErrProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="XmlAdvErrProd" format="xml">
  <cd:Record>
    <cd:Field name="xmlns_len">
      <cd:Optional/>
      <cd:Record>
        <cd:Attribute name="xmlns">
          <cd:Text><cd:ByteSize>4</cd:ByteSize></cd:Text>
        </cd:Attribute>
      </cd:Record>
    </cd:Field>
    <cd:Field name="attr_len">
      <cd:Optional/>
      <cd:Record>
        <cd:Attribute name="code">
          <cd:Text><cd:ByteSize>4</cd:ByteSize></cd:Text>
        </cd:Attribute>
      </cd:Record>
    </cd:Field>
    <cd:Field name="attr_dup">
      <cd:Optional/>
      <cd:Record>
        <cd:Attribute name="dup">
          <cd:Text/>
        </cd:Attribute>
      </cd:Record>
    </cd:Field>
    <cd:Field name="cplx_holder">
      <cd:Optional/>
      <cd:Record>
        <cd:Field name="cplx">
          <cd:Complex>
            <cd:Float/>
          </cd:Complex>
        </cd:Field>
      </cd:Record>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 2> entries = {
      {
          {"products/XmlRootArrayProd.xml", kRootArrayProdXml},
          {"products/XmlAdvErrProd.xml", kAdvErrProdXml},
      }};
  const TempFile codadef("coda_xml_parser_adv_errors.codadef");
  ASSERT_TRUE(codadef.WriteBytes(BuildCodadefBytes(kIndexXml, entries)));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));

  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // 1. Root record field of XML array class initializes parent->field_type[0]
  //    on first element in start_element_handler().
  {
    const TempFile root_arr_file("coda_xml_parser_root_arr.xml");
    ASSERT_TRUE(root_arr_file.WriteText(
        R"xml(<?xml version="1.0"?><doc>root_item</doc>)xml"));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open_as(root_arr_file.c_str(), "XmlAdvClass",
                           "XmlRootArrayType", 1, product.OutParam()));

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor arr = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&arr, "/doc"));
    long num_elements = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&arr, &num_elements));
    EXPECT_EQ(1, num_elements);
    EXPECT_EQ("root_item", ReadStringAt(root, "/doc[0]"));
  }

  // 2. Attribute and non-time special-type failure paths in
  //    attribute_record_new() and end_element_handler().
  struct AdvErrorCase {
    std::string_view name;
    std::string_view xml;
    int expected_errno;
    std::string_view expected_substr;
  };
  constexpr AdvErrorCase kCases[] = {
      {
          .name = "xmlns_byte_size_mismatch",
          .xml =
              R"xml(<?xml version="1.0"?><xmlns_len xmlns="urn:too_long"/>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "does not match that of definition",
      },
      {
          .name = "attr_byte_size_mismatch",
          .xml = R"xml(<?xml version="1.0"?><attr_len code="ab"/>)xml",
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "does not match that of definition",
      },
      {
          .name = "duplicate_attr_after_ns_strip",
          .xml =
              R"xml(<?xml version="1.0"?><attr_dup xmlns:a="urn:a" dup="1" a:dup="2"/>)xml",
          .expected_errno = CODA_ERROR_INVALID_ARGUMENT,
          .expected_substr = "field 'dup' is already set",
      },
      {
          .name = "non_time_special_type_in_xml",
          .xml =
              R"xml(<?xml version="1.0"?><cplx_holder><cplx>123</cplx></cplx_holder>)xml",
          .expected_errno = CODA_ERROR_INVALID_ARGUMENT,
          .expected_substr = "definition is not a time type",
      },
  };

  for (const AdvErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const TempFile xml_file("coda_xml_parser_adv_err_case.xml");
    ASSERT_TRUE(xml_file.WriteText(tc.xml));

    coda_set_error(CODA_SUCCESS, nullptr);
    ScopedProduct product;
    EXPECT_EQ(-1, coda_open_as(xml_file.c_str(), "XmlAdvClass", "XmlAdvErrType",
                               1, product.OutParam()));
    EXPECT_EQ(tc.expected_errno, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr(tc.expected_substr));
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST(CodaXmlParserTest, DirectCodaXmlParseRejectsNonXmlRootRecordFormat) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // Covers start_element_handler() when info->record[info->depth] has a
  // non-XML definition format.
  coda_type_record* ascii_root = coda_type_record_new(coda_format_ascii);
  ASSERT_NE(nullptr, ascii_root);
  absl::Cleanup release_root([ascii_root] {
    coda_type_release(reinterpret_cast<coda_type*>(ascii_root));
  });

  constexpr char kBuf[] = R"xml(<?xml version="1.0"?><doc/>)xml";
  coda_bin_product raw{};
  raw.format = coda_format_binary;
  raw.file_size = static_cast<int64_t>(sizeof(kBuf) - 1);
  raw.use_mmap = 1;
  raw.mem_size = raw.file_size;
  raw.mem_ptr = reinterpret_cast<const uint8_t*>(kBuf);

  coda_product_definition def{};
  def.format = coda_format_xml;
  def.root_type = reinterpret_cast<coda_type*>(ascii_root);

  coda_xml_product xml{};
  xml.format = coda_format_xml;
  xml.product_definition = &def;
  xml.raw_product = reinterpret_cast<coda_product*>(&raw);

  absl::Cleanup cleanup_xml([&xml] {
    if (xml.root_type != nullptr) {
      coda_dynamic_type_delete(xml.root_type);
    }
    if (xml.mem_ptr != nullptr) {
      std::free(xml.mem_ptr);
    }
  });

  // TODO(schwehr): Upstream coda-xml-parser.c passes NULL element name to
  // printf("%s") when xml.root_type != NULL at root; avoid pinning "(null)".
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_xml_parse(&xml));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("not allowed inside ascii data"));
  coda_set_error(CODA_SUCCESS, nullptr);
}

// Creates an XML union type with a field expression, which
// coda_mem_record_new() rejects when instantiating an element's or array's
// attributes record.
coda_type_record* CreateUnionWithExprAttributes() {
  coda_type_record* union_attrs = coda_type_union_new(coda_format_xml);
  if (union_attrs == nullptr) {
    return nullptr;
  }
  coda_expression* field_expr = nullptr;
  if (coda_expression_from_string("0", &field_expr) != CODA_SUCCESS) {
    coda_type_release(reinterpret_cast<coda_type*>(union_attrs));
    return nullptr;
  }
  if (coda_type_union_set_field_expression(union_attrs, field_expr) !=
      CODA_SUCCESS) {
    coda_expression_delete(field_expr);
    coda_type_release(reinterpret_cast<coda_type*>(union_attrs));
    return nullptr;
  }
  return union_attrs;
}

// Creates a variable-length XML text array type with `attrs` attached.
coda_type_array* CreateXmlArrayWithAttributes(coda_type_record* attrs) {
  coda_type_array* arr_def = coda_type_array_new(coda_format_xml);
  if (arr_def == nullptr) {
    return nullptr;
  }
  coda_type_text* base_text = coda_type_text_new(coda_format_xml);
  if (base_text == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(arr_def));
    return nullptr;
  }
  const int set_base_rc = coda_type_array_set_base_type(
      arr_def, reinterpret_cast<coda_type*>(base_text));
  coda_type_release(reinterpret_cast<coda_type*>(base_text));
  if (set_base_rc != CODA_SUCCESS ||
      coda_type_array_add_variable_dimension(arr_def, nullptr) !=
          CODA_SUCCESS ||
      coda_type_set_attributes(reinterpret_cast<coda_type*>(arr_def), attrs) !=
          CODA_SUCCESS) {
    coda_type_release(reinterpret_cast<coda_type*>(arr_def));
    return nullptr;
  }
  return arr_def;
}

void ExpectXmlParseUnionAttrError(coda_type_record* root_def,
                                  std::string_view xml_bytes) {
  coda_bin_product raw{};
  raw.format = coda_format_binary;
  raw.file_size = static_cast<int64_t>(xml_bytes.size());
  raw.use_mmap = 1;
  raw.mem_size = raw.file_size;
  raw.mem_ptr = reinterpret_cast<const uint8_t*>(xml_bytes.data());

  coda_product_definition def{};
  def.format = coda_format_xml;
  def.root_type = reinterpret_cast<coda_type*>(root_def);

  coda_xml_product xml{};
  xml.format = coda_format_xml;
  xml.product_definition = &def;
  xml.raw_product = reinterpret_cast<coda_product*>(&raw);

  absl::Cleanup cleanup_xml([&xml] {
    if (xml.root_type != nullptr) {
      coda_dynamic_type_delete(xml.root_type);
    }
    if (xml.mem_ptr != nullptr) {
      std::free(xml.mem_ptr);
    }
  });

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_xml_parse(&xml));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("union definition with field expression"));
  coda_set_error(CODA_SUCCESS, nullptr);
}

TEST(CodaXmlParserTest, DirectCodaXmlParseRejectsUnionWithExprAttributes) {
  const ScopedXmlTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  coda_type_record* union_attrs = CreateUnionWithExprAttributes();
  ASSERT_NE(nullptr, union_attrs);
  absl::Cleanup release_union_attrs([union_attrs] {
    coda_type_release(reinterpret_cast<coda_type*>(union_attrs));
  });

  // 1. Root record instantiation failure in coda_xml_parse().
  {
    coda_type_record* root_def = coda_type_record_new(coda_format_xml);
    ASSERT_NE(nullptr, root_def);
    absl::Cleanup release_root([root_def] {
      coda_type_release(reinterpret_cast<coda_type*>(root_def));
    });
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_attributes(reinterpret_cast<coda_type*>(root_def),
                                       union_attrs));
    ExpectXmlParseUnionAttrError(root_def,
                                 R"xml(<?xml version="1.0"?><doc/>)xml");
  }

  // 2. Root-level XML array allocation failure in start_element_handler().
  {
    coda_type_record* root_def = coda_type_record_new(coda_format_xml);
    ASSERT_NE(nullptr, root_def);
    absl::Cleanup release_root([root_def] {
      coda_type_release(reinterpret_cast<coda_type*>(root_def));
    });
    coda_type_array* arr_def = CreateXmlArrayWithAttributes(union_attrs);
    ASSERT_NE(nullptr, arr_def);
    absl::Cleanup release_arr([arr_def] {
      coda_type_release(reinterpret_cast<coda_type*>(arr_def));
    });
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_create_field(
                  root_def, "doc", reinterpret_cast<coda_type*>(arr_def)));
    ExpectXmlParseUnionAttrError(
        root_def, R"xml(<?xml version="1.0"?><doc>item</doc>)xml");
  }

  // 3. Pre-allocated child XML array failure inside a record element in
  //    start_element_handler() (doc_def has no attributes, so info.attributes
  //    is NULL on abort).
  {
    coda_type_record* root_def = coda_type_record_new(coda_format_xml);
    ASSERT_NE(nullptr, root_def);
    absl::Cleanup release_root([root_def] {
      coda_type_release(reinterpret_cast<coda_type*>(root_def));
    });
    coda_type_record* doc_def = coda_type_record_new(coda_format_xml);
    ASSERT_NE(nullptr, doc_def);
    absl::Cleanup release_doc([doc_def] {
      coda_type_release(reinterpret_cast<coda_type*>(doc_def));
    });
    coda_type_array* arr_def = CreateXmlArrayWithAttributes(union_attrs);
    ASSERT_NE(nullptr, arr_def);
    absl::Cleanup release_arr([arr_def] {
      coda_type_release(reinterpret_cast<coda_type*>(arr_def));
    });
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_create_field(
                  doc_def, "item", reinterpret_cast<coda_type*>(arr_def)));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_create_field(
                  root_def, "doc", reinterpret_cast<coda_type*>(doc_def)));
    ExpectXmlParseUnionAttrError(
        root_def, R"xml(<?xml version="1.0"?><doc><item>x</item></doc>)xml");
  }
}

// TODO(schwehr): Remaining uncovered lines in libcoda/coda-xml-parser.c
// (54/512 = 89.5% ceiling): 41 malloc/realloc OOM branches, 3 Expat
// not_standalone_handler() lines (315, 317, 320, unreachable without an
// ExternalEntityRefHandler), and 10 unreachable defensive checks on freshly
// created/indexed schema fields (153-155, 418-419, 459-461, 873-874).

}  // namespace
}  // namespace stcorp_coda
