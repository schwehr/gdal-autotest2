// Tests for minixml's ability to work with XML files and data structures.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_minixml.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_minixml.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/port/cpl_minixml.h"

#include <cstring>
#include <set>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory_closer.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

// Manage a tree of XML nodes so that all nodes are freed when the instance goes
// out of scope.  Only the top level node should be in a CPLXMLTreeCloser.
class CPLXMLTreeCloser {
 public:
  explicit CPLXMLTreeCloser(CPLXMLNode* tree) : tree_(tree) {}
  // CPLDestroyXMLNode handles nullptr.
  ~CPLXMLTreeCloser() { CPLDestroyXMLNode(tree_); }

  // Modifying the contents pointed to by the return is allowed.
  CPLXMLNode* get() const { return tree_; }
  CPLXMLNode* operator->() const { return get(); }

 private:
  CPLXMLNode* tree_;
};

// Tests sanitizing a string into a safe XML element name.
TEST(CplMiniXmlTest, CleanXMLElementName) {
  // Null pointer is a NOP.  Ensure that a nullptr does not cause a crash
  // or leak memory.
  CPLCleanXMLElementName(nullptr);

  char empty[] = "";
  CPLCleanXMLElementName(empty);

  char single_char_str[] = "a";
  CPLCleanXMLElementName(single_char_str);
  ASSERT_STREQ("a", single_char_str);

  // These ASCII character numbers should be transformed into an underscore.
  std::set<int> invalid_set = {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
      22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
      40, 41, 42, 43, 44, 45,
      // Valid: 46 ('.')
      47,
      // Valid: 48..57 ('0'..'9')
      58, 59, 60, 61, 62, 63, 64,
      // Valid: 65..90 ('A'..'Z')
      91, 92, 93, 94, 95, 96,
      // Valid: 97..122 ('a'..'z')
      123, 124, 125, 126, 127
      // Valid: 128..255 (non-printable ASCII characters)
  };

  for (int i = 0; i < 256; i++) {
    char test_char = static_cast<char>(i);
    single_char_str[0] = test_char;
    CPLCleanXMLElementName(single_char_str);

    char expected_char_str[2] = "_";  // Until proven otherwise.
    if (invalid_set.find(i) == invalid_set.end()) {
      expected_char_str[0] = test_char;
    }
    ASSERT_STREQ(expected_char_str, single_char_str);
  }

  std::string unchanged(
      ".0123456789"
      "abcdefghijklmnopqrstuvwxyz"
      "_"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  CPLMemoryCloser<char> result(CPLStrdup(unchanged.c_str()));
  CPLCleanXMLElementName(result.get());
  ASSERT_STREQ(unchanged.c_str(), result.get());

  std::string changed(
      "\a\b\f\r\n\t\v !\"#$%&'()*+,-/"
      ":;<=>?@"
      "[\\]^`"
      "{|}~");
  CPLMemoryCloser<char> result_changed(CPLStrdup(changed.c_str()));
  CPLCleanXMLElementName(result_changed.get());
  ASSERT_STREQ("______________________________________", result_changed.get());
}

// Tests creating a single XML node.
TEST(CplMiniXmlTest, CPLCreateXMLNodeSingle) {
  // Without a tag name.
  CPLXMLTreeCloser element(CPLCreateXMLNode(nullptr, CXT_Element, nullptr));
  ASSERT_EQ(CXT_Element, element->eType);
  ASSERT_STREQ("", element->pszValue);
  ASSERT_EQ(nullptr, element->psNext);
  ASSERT_EQ(nullptr, element->psChild);
  CPLMemoryCloser<char> xml_element(CPLSerializeXMLTree(element.get()));
  ASSERT_STREQ("< />\n", xml_element.get());

  // With a tag name.
  CPLXMLTreeCloser element_named(
      CPLCreateXMLNode(nullptr, CXT_Element, "a_name"));
  ASSERT_STREQ("a_name", element_named->pszValue);
  CPLMemoryCloser<char> xml_element_named(
      CPLSerializeXMLTree(element_named.get()));
  ASSERT_STREQ("<a_name />\n", xml_element_named.get());

  CPLXMLTreeCloser text(CPLCreateXMLNode(nullptr, CXT_Text, nullptr));
  ASSERT_EQ(CXT_Text, text->eType);
  CPLMemoryCloser<char> xml_text(CPLSerializeXMLTree(text.get()));
  ASSERT_STREQ("", xml_text.get());

  CPLXMLTreeCloser attribute(CPLCreateXMLNode(nullptr, CXT_Attribute, nullptr));
  ASSERT_EQ(CXT_Attribute, attribute->eType);
  // An attribute cannot be serialized without a child CXT_Text.

  CPLXMLTreeCloser comment(CPLCreateXMLNode(nullptr, CXT_Comment, nullptr));
  ASSERT_EQ(CXT_Comment, comment->eType);
  CPLMemoryCloser<char> xml_comment(CPLSerializeXMLTree(comment.get()));
  ASSERT_STREQ("<!---->\n", xml_comment.get());

  CPLXMLTreeCloser literal(CPLCreateXMLNode(nullptr, CXT_Literal, nullptr));
  ASSERT_EQ(CXT_Literal, literal->eType);
  CPLMemoryCloser<char> xml_literal(CPLSerializeXMLTree(literal.get()));
  ASSERT_STREQ("\n", xml_literal.get());
}

// Tests creating and destroying a small XML node tree.
TEST(CplMiniXmlTest, CPLCreateXMLNodeSmallTree) {
  CPLXMLTreeCloser root(CPLCreateXMLNode(nullptr, CXT_Element, "the root"));
  CPLCreateXMLNode(root.get(), CXT_Literal, "2nd level leaf node - literal");
  CPLCreateXMLNode(root.get(), CXT_Text, "text leaf node");
  CPLXMLNode* attrib = CPLCreateXMLNode(root.get(), CXT_Attribute, "attrib");
  CPLCreateXMLNode(attrib, CXT_Text, "text for attrib");

  ASSERT_EQ(CXT_Literal, root->psChild->eType);

  CPLMemoryCloser<char> xml(CPLSerializeXMLTree(root.get()));
  ASSERT_STREQ(
      "<the root attrib=\"text for attrib\">\n"
      "  2nd level leaf node - literal\n"
      "text leaf node</the root>\n",
      xml.get());
}

TEST(CplMiniXmlTest, ParseValidXml) {
  CPLXMLTreeCloser null_tree(CPLParseXMLString(nullptr));
  EXPECT_EQ(nullptr, null_tree.get());

  CPLXMLTreeCloser empty_tree(CPLParseXMLString(""));
  ASSERT_EQ(nullptr, null_tree.get());
}

TEST(CplMiniXmlTest, ParseOne) {
  CPLXMLTreeCloser root(CPLParseXMLString("<a/>"));
  ASSERT_NE(nullptr, root.get());
  EXPECT_EQ(CXT_Element, root->eType);
  EXPECT_STREQ("a", root->pszValue);
  EXPECT_EQ(nullptr, root->psNext);
  EXPECT_EQ(nullptr, root->psChild);
}

TEST(CplMiniXmlTest, ParseOneAttribute) {
  CPLXMLTreeCloser root(CPLParseXMLString("<a foo=\"2\"/>"));
  ASSERT_NE(nullptr, root.get());
  EXPECT_EQ(CXT_Element, root->eType);
  EXPECT_STREQ("a", root->pszValue);
  EXPECT_EQ(nullptr, root->psNext);
  ASSERT_NE(nullptr, root->psChild);

  CPLXMLNode* attribute = root->psChild;
  EXPECT_EQ(CXT_Attribute, attribute->eType);
  EXPECT_STREQ("foo", attribute->pszValue);
  EXPECT_EQ(nullptr, attribute->psNext);
  ASSERT_NE(nullptr, attribute->psChild);

  CPLXMLNode* value = attribute->psChild;
  EXPECT_EQ(CXT_Text, value->eType);
  EXPECT_STREQ("2", value->pszValue);
  EXPECT_EQ(nullptr, value->psNext);
  EXPECT_EQ(nullptr, value->psChild);
}

void CheckXmlNode(CPLXMLNode* node, CPLXMLNodeType e_type, const char* value,
                  bool has_next, bool has_child) {
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(e_type, node->eType);
  EXPECT_STREQ(value, node->pszValue);

  if (has_next)
    EXPECT_NE(nullptr, node->psNext);
  else
    EXPECT_EQ(nullptr, node->psNext);

  if (has_child)
    EXPECT_NE(nullptr, node->psChild);
  else
    EXPECT_EQ(nullptr, node->psChild);
}

TEST(CplMiniXmlTest, GetXmlNode) {
  CPLXMLTreeCloser root(
      CPLParseXMLString("<a b='c'> <d e='f'> <g h='i'/> </d> <j/> </a>"));
  ASSERT_NE(nullptr, root.get());
  ASSERT_EQ(nullptr, CPLGetXMLNode(root.get(), "does_not_exist"));

  EXPECT_EQ(nullptr, CPLGetXMLNode(root.get(), "a"));

  CPLXMLNode* a = CPLGetXMLNode(root.get(), "=a");
  CheckXmlNode(a, CXT_Element, "a", false, true);

  CPLXMLNode* b = CPLGetXMLNode(root.get(), "b");
  CheckXmlNode(b, CXT_Attribute, "b", true, true);

  b = CPLGetXMLNode(root.get(), "=a.b");
  CheckXmlNode(b, CXT_Attribute, "b", true, true);

  CPLXMLNode* d = CPLGetXMLNode(root.get(), "d");
  CheckXmlNode(d, CXT_Element, "d", true, true);

  CPLXMLNode* e = CPLGetXMLNode(root.get(), "d.e");
  CheckXmlNode(e, CXT_Attribute, "e", true, true);

  CPLXMLNode* g = CPLGetXMLNode(root.get(), "d.g");
  CheckXmlNode(g, CXT_Element, "g", false, true);

  CPLXMLNode* h = CPLGetXMLNode(root.get(), "d.g.h");
  CheckXmlNode(h, CXT_Attribute, "h", false, true);

  CPLXMLNode* j = CPLGetXMLNode(root.get(), "j");
  CheckXmlNode(j, CXT_Element, "j", false, false);
}

TEST(CplMiniXmlTest, Search) {
  CPLXMLTreeCloser root(
      CPLParseXMLString("<a b='c'> <d e='f'> <g h='i'/> </d> <j/> </a>"));

  const CPLXMLNode* root_const = nullptr;
  EXPECT_EQ(nullptr, CPLSearchXMLNode(root_const, "z"));
  EXPECT_EQ(nullptr, CPLSearchXMLNode(root.get(), "zz"));

  EXPECT_STREQ("a", CPLSearchXMLNode(root.get(), "a")->pszValue);
  EXPECT_STREQ("a", CPLSearchXMLNode(root.get(), "=a")->pszValue);
  CheckXmlNode(CPLSearchXMLNode(root.get(), "b"), CXT_Attribute, "b", true,
               true);

  EXPECT_STREQ("g", CPLSearchXMLNode(root.get(), "g")->pszValue);
  CheckXmlNode(CPLSearchXMLNode(root.get(), "h"), CXT_Attribute, "h", false,
               true);

  EXPECT_STREQ("j", CPLSearchXMLNode(root.get(), "j")->pszValue);
}

TEST(CplMiniXmlTest, SerializeSingleSelfClosing) {
  CPLXMLTreeCloser root(CPLParseXMLString("<a/>"));
  CPLMemoryCloser<char> tree(CPLSerializeXMLTree(root.get()));
  EXPECT_STREQ("<a />\n", tree.get());
}

TEST(CplMiniXmlTest, SerializeSingle) {
  CPLXMLTreeCloser root(CPLParseXMLString("<a></a>"));
  CPLMemoryCloser<char> tree(CPLSerializeXMLTree(root.get()));
  EXPECT_STREQ("<a />\n", tree.get());
}

TEST(CplMiniXmlTest, SerializeVariety) {
  CPLXMLTreeCloser root(
      CPLParseXMLString("<a b='c'> <d> <g h='i'/> text </d> <j/> </a>"));
  CPLMemoryCloser<char> tree(CPLSerializeXMLTree(root.get()));
  EXPECT_STREQ(
      "<a b=\"c\">\n  <d>\n    <g h=\"i\" />\ntext   </d>\n  <j />\n</a>\n",
      tree.get());
}

// TODO(schwehr): Test CPLAddXMLChild().
// TODO(schwehr): Test CPLRemoveXMLChild().
// TODO(schwehr): Test CPLCreateXMLElementAndValue().
// TODO(schwehr): Test CPLAddXMLAttributeAndValue().
// TODO(schwehr): Test CPLCloneXMLTree().
// TODO(schwehr): Test CPLSetXMLValue().
// TODO(schwehr): Test CPLStripXMLNamespace().
// TODO(schwehr): Test CPLCleanXMLElementName().
// TODO(schwehr): Test CPLParseXMLFile().
// TODO(schwehr): Test CPLSerializeXMLTreeToFile().

TEST(CplMiniXmlTest, ParseCommentsDoctypeCdataAndPI) {
  // UTF-8 BOM, XML PI, PI with unquoted text token (schematron style),
  // DOCTYPE with internal subset and quotes, comments, CDATA, long tokens,
  // newlines after unquoted token, and entities.
  const char* kXml =
      "\xEF\xBB\xBF"
      "<?xml version=\"1.0\" encoding='UTF-8'?>\n"
      "<?valbuddy_schematron test.sch?>\n"
      "<!-- Top level comment -->\n"
      "<!DOCTYPE root_element_with_long_name_1234567890 [\n"
      "  <!ENTITY nbsp \"&#xA0;\">\n"
      "  <!ENTITY apos_ent 'single'>\n"
      "  <!ELEMENT root_element_with_long_name_1234567890 ANY>\n"
      "]>\n"
      "<root_element_with_long_name_1234567890 "
      "long_attribute_name_1234567890=\"&lt;&gt;&amp;&quot;&apos;\" "
      "single_attr='&lt;&gt;&amp;&quot;&apos;'>\n"
      "  <!-- Inner comment -->\n"
      "  <![CDATA[Some <raw> & \"cdata\" content]]>\n"
      "  <child>Text with &lt;&gt;&amp;&quot;&apos; and newline\n"
      "</child>\n"
      "</root_element_with_long_name_1234567890>\n"
      "<second_root/>\n";

  CPLXMLTreeCloser root(CPLParseXMLString(kXml));
  ASSERT_NE(nullptr, root.get());

  CPLMemoryCloser<char> serialized(CPLSerializeXMLTree(root.get()));
  ASSERT_NE(nullptr, serialized.get());
  EXPECT_NE(nullptr, strstr(serialized.get(), "<?xml version=\"1.0\""));
  EXPECT_NE(nullptr,
            strstr(serialized.get(), "<?valbuddy_schematron test.sch?>"));
  EXPECT_NE(nullptr, strstr(serialized.get(), "<!-- Top level comment -->"));
  EXPECT_NE(nullptr, strstr(serialized.get(), "<!-- Inner comment -->"));
  EXPECT_NE(nullptr, strstr(serialized.get(),
                            "Some &lt;raw&gt; &amp; \"cdata\" content"));
  EXPECT_NE(nullptr, strstr(serialized.get(), "<second_root />"));

  // Also test simple DOCTYPE without '['.
  CPLXMLTreeCloser doc2(
      CPLParseXMLString("<!DOCTYPE html SYSTEM 'about:legacy-compat'><html/>"));
  ASSERT_NE(nullptr, doc2.get());
}

TEST(CplMiniXmlTest, ParseErrorsAndWarnings) {
  WithQuietHandler quiet;

  // Null or empty input.
  EXPECT_EQ(nullptr, CPLParseXMLString(nullptr));
  EXPECT_EQ(nullptr, CPLParseXMLString(""));

  // UnreadChar on newline after unquoted attribute value warning.
  {
    CPLXMLTreeCloser tree(CPLParseXMLString("<a attr=unquoted_val\n/>"));
    ASSERT_NE(nullptr, tree.get());
    EXPECT_STREQ("unquoted_val", CPLGetXMLValue(tree.get(), "attr", ""));
  }

  // Case mismatch in closing tag triggers warning.
  {
    CPLXMLTreeCloser tree(CPLParseXMLString("<foo>text</FOO>"));
    ASSERT_NE(nullptr, tree.get());
  }

  // Unclosed comments are parsed until EOF into a comment node.
  {
    CPLXMLTreeCloser c1(CPLParseXMLString("<!-- unclosed comment"));
    EXPECT_NE(nullptr, c1.get());
    CPLXMLTreeCloser c2(CPLParseXMLString("<!-- unclosed -"));
    EXPECT_NE(nullptr, c2.get());
    CPLXMLTreeCloser c3(CPLParseXMLString("<!-- unclosed --"));
    EXPECT_NE(nullptr, c3.get());
  }

  // Unclosed or invalid DOCTYPE.
  EXPECT_EQ(nullptr, CPLParseXMLString("<!DOCTYPE"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<!DOCTYPE root \"unclosed"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<!DOCTYPE root 'unclosed"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<!DOCTYPE root [ <!ELEMENT a>"));

  // Unclosed CDATA and unrecognized <! token.
  EXPECT_EQ(nullptr, CPLParseXMLString("<a><![CDATA[unclosed</a>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a><![CDATA[unclosed]</a>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a><![CDATA[unclosed]]</a>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a><!BOGUS></a>"));

  // Unclosed or malformed processing instructions.
  EXPECT_EQ(nullptr, CPLParseXMLString("<?xml version=\"1.0\""));
  EXPECT_EQ(nullptr, CPLParseXMLString("<?xml attr=?>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a?>"));

  // Unterminated quotes.
  EXPECT_EQ(nullptr, CPLParseXMLString("<a b=\"unterminated>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a b='unterminated>"));

  // Malformed tags and attributes.
  EXPECT_EQ(nullptr, CPLParseXMLString("< ="));
  EXPECT_EQ(nullptr, CPLParseXMLString("<"));
  EXPECT_EQ(nullptr, CPLParseXMLString("</bar>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a></bar></a>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a></a extra>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a 'unexpected'>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a b c='d'/>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a b=/>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a b=>"));
  EXPECT_EQ(nullptr, CPLParseXMLString("<a><b>"));
}

TEST(CplMiniXmlTest, TreeManipulationAndGetters) {
  {
    CPLXMLTreeCloser null_val_node(
        CPLCreateXMLNode(nullptr, CXT_Element, nullptr));
    ASSERT_NE(nullptr, null_val_node.get());
    EXPECT_STREQ("", null_val_node->pszValue);
  }
  EXPECT_EQ(nullptr, CPLGetXMLNode(static_cast<CPLXMLNode*>(nullptr), "a"));
  EXPECT_EQ(nullptr, CPLGetXMLValue(nullptr, "a", nullptr));
  EXPECT_STREQ("def", CPLGetXMLValue(nullptr, "a", "def"));

  CPLXMLTreeCloser root(CPLCreateXMLNode(nullptr, CXT_Element, "root"));
  ASSERT_NE(nullptr, root.get());

  // Create a CXT_Text child first on root, then add CXT_Attribute children
  // to exercise the attribute-before-text insertion path in _CPLCreateXMLNode
  // and CPLAddXMLChild.
  CPLXMLNode* text_node = CPLCreateXMLNode(root.get(), CXT_Text, "root_text");
  ASSERT_NE(nullptr, text_node);
  CPLXMLNode* attr1 = CPLCreateXMLNode(root.get(), CXT_Attribute, "attr1");
  CPLCreateXMLNode(attr1, CXT_Text, "val1");
  CPLXMLNode* attr2 = CPLCreateXMLNode(root.get(), CXT_Attribute, "attr2");
  CPLCreateXMLNode(attr2, CXT_Text, "val2");

  // Also test CPLAddXMLChild with a new attribute when first child is attribute
  // and later child is text.
  CPLXMLNode* attr3 = CPLCreateXMLNode(nullptr, CXT_Attribute, "attr3");
  CPLCreateXMLNode(attr3, CXT_Text, "val3");
  CPLAddXMLChild(root.get(), attr3);

  // Also test CPLAddXMLChild with an attribute when psChild is directly
  // CXT_Text.
  CPLXMLTreeCloser text_first(CPLCreateXMLNode(nullptr, CXT_Element, "tf"));
  CPLAddXMLChild(text_first.get(),
                 CPLCreateXMLNode(nullptr, CXT_Text, "hello"));
  CPLXMLNode* tf_attr = CPLCreateXMLNode(nullptr, CXT_Attribute, "k");
  CPLCreateXMLNode(tf_attr, CXT_Text, "v");
  CPLAddXMLChild(text_first.get(), tf_attr);
  EXPECT_STREQ("v", CPLGetXMLValue(text_first.get(), "k", ""));
  EXPECT_STREQ("hello", CPLGetXMLValue(text_first.get(), "", ""));
  EXPECT_STREQ("hello", CPLGetXMLValue(text_first.get(), nullptr, ""));

  // Multiple text children on text_first -> CPLGetXMLValue returns default.
  CPLAddXMLChild(text_first.get(),
                 CPLCreateXMLNode(nullptr, CXT_Text, "world"));
  EXPECT_STREQ("fallback", CPLGetXMLValue(text_first.get(), "", "fallback"));

  // CPLCreateXMLElementAndValue and CPLAddXMLAttributeAndValue.
  CPLXMLNode* child1 =
      CPLCreateXMLElementAndValue(root.get(), "child1", "val_child1");
  ASSERT_NE(nullptr, child1);
  CPLAddXMLAttributeAndValue(child1, "c_attr", "c_val");
  EXPECT_STREQ("val_child1", CPLGetXMLValue(root.get(), "child1", ""));
  EXPECT_STREQ("c_val", CPLGetXMLValue(root.get(), "child1.c_attr", ""));
  EXPECT_STREQ("def", CPLGetXMLValue(root.get(), "nonexistent", "def"));

  // CPLAddXMLSibling.
  CPLAddXMLSibling(nullptr, nullptr);
  CPLXMLNode* child2 = CPLCreateXMLNode(nullptr, CXT_Element, "child2");
  CPLAddXMLSibling(child1, child2);
  EXPECT_EQ(child2, CPLGetXMLNode(root.get(), "child2"));
  EXPECT_EQ(root.get(), CPLGetXMLNode(root.get(), "=root"));
  EXPECT_EQ(nullptr, CPLGetXMLNode(root.get(), "=other"));

  // CPLSearchXMLNode across siblings.
  EXPECT_EQ(child2, CPLSearchXMLNode(child1, "=child2"));
  EXPECT_EQ(child1, CPLSearchXMLNode(root.get(), "=child1"));
  EXPECT_EQ(nullptr, CPLSearchXMLNode(root.get(), "=nonexistent"));

  // CPLRemoveXMLChild.
  EXPECT_FALSE(CPLRemoveXMLChild(nullptr, child1));
  EXPECT_FALSE(CPLRemoveXMLChild(root.get(), nullptr));
  // Remove first child (attr1).
  EXPECT_TRUE(CPLRemoveXMLChild(root.get(), attr1));
  CPLDestroyXMLNode(attr1);
  // Remove a middle/last child (child2).
  EXPECT_TRUE(CPLRemoveXMLChild(root.get(), child2));
  EXPECT_FALSE(CPLRemoveXMLChild(root.get(), child2));
  CPLDestroyXMLNode(child2);

  // CPLCloneXMLTree and CPLXMLNodeGetRAMUsageEstimate.
  EXPECT_EQ(nullptr, CPLCloneXMLTree(nullptr));
  EXPECT_GT(CPLXMLNodeGetRAMUsageEstimate(root.get()), 0U);

  CPLXMLTreeCloser cloned(CPLCloneXMLTree(root.get()));
  ASSERT_NE(nullptr, cloned.get());
  EXPECT_STREQ("val_child1", CPLGetXMLValue(cloned.get(), "child1", ""));
}

TEST(CplMiniXmlTest, SetXMLValueAndStripNamespace) {
  CPLXMLTreeCloser root(
      CPLParseXMLString("<ns:root xmlns=\"http://example.com\" "
                        "xmlns:ns=\"http://ns.example.com\" "
                        "other:attr=\"1\" ns:attr=\"2\">"
                        "<ns:child existing_attr=\"old\">old_text</ns:child>"
                        "<ns:empty_child attr=\"a\"/>"
                        "<other:keep/>"
                        "<colon_end:/>"
                        "</ns:root>"));
  ASSERT_NE(nullptr, root.get());

  // Update existing text in ns:child.
  EXPECT_TRUE(CPLSetXMLValue(root.get(), "ns:child", "new_text"));
  EXPECT_STREQ("new_text", CPLGetXMLValue(root.get(), "ns:child", ""));

  // Update existing attribute with and without '#'.
  EXPECT_TRUE(
      CPLSetXMLValue(root.get(), "ns:child.#existing_attr", "new_attr_1"));
  EXPECT_STREQ("new_attr_1",
               CPLGetXMLValue(root.get(), "ns:child.existing_attr", ""));
  EXPECT_TRUE(
      CPLSetXMLValue(root.get(), "ns:child.existing_attr", "new_attr_2"));
  EXPECT_STREQ("new_attr_2",
               CPLGetXMLValue(root.get(), "ns:child.existing_attr", ""));

  // Create new attribute via '#'.
  EXPECT_TRUE(CPLSetXMLValue(root.get(), "ns:child.#created_attr", "created"));
  EXPECT_STREQ("created",
               CPLGetXMLValue(root.get(), "ns:child.created_attr", ""));

  // Set text on element that had no text child (only attribute).
  EXPECT_TRUE(CPLSetXMLValue(root.get(), "ns:empty_child", "filled"));
  EXPECT_STREQ("filled", CPLGetXMLValue(root.get(), "ns:empty_child", ""));

  // Create intermediate elements via path.
  EXPECT_TRUE(CPLSetXMLValue(root.get(), "a.b.c", "deep_val"));
  EXPECT_STREQ("deep_val", CPLGetXMLValue(root.get(), "a.b.c", ""));

  // Strip specific namespace "ns" without recursion, then with recursion.
  CPLStripXMLNamespace(nullptr, "ns", 1);
  CPLStripXMLNamespace(root.get(), "ns", 0);
  EXPECT_STREQ("root", root->pszValue);
  CPLStripXMLNamespace(root.get(), "ns", 1);
  EXPECT_NE(nullptr, CPLGetXMLNode(root.get(), "=root.child"));
  EXPECT_NE(nullptr, CPLGetXMLNode(root.get(), "=root.other:keep"));

  // Strip all namespaces (pszNameSpace = nullptr).
  CPLStripXMLNamespace(root.get(), nullptr, 1);
  EXPECT_NE(nullptr, CPLGetXMLNode(root.get(), "=root.keep"));
  EXPECT_NE(nullptr, CPLGetXMLNode(root.get(), "=root.xmlns"));
  EXPECT_NE(nullptr, CPLGetXMLNode(root.get(), "=root.ns"));
}

TEST(CplMiniXmlTest, SerializeEdgeCasesAndFileIO) {
  CPLMemoryCloser<char> empty_tree(CPLSerializeXMLTree(nullptr));
  EXPECT_STREQ("", empty_tree.get());

  // Build a tree with CXT_Literal, CXT_Comment,text with newline, etc.
  CPLXMLTreeCloser root(CPLCreateXMLNode(nullptr, CXT_Element, "root"));
  CPLCreateXMLNode(root.get(), CXT_Comment, " a comment ");
  CPLXMLNode* lit_elem = CPLCreateXMLNode(root.get(), CXT_Element, "lit");
  CPLCreateXMLNode(lit_elem, CXT_Literal, "<raw_literal/>");
  CPLXMLNode* nl_elem = CPLCreateXMLNode(root.get(), CXT_Element, "nl");
  CPLCreateXMLNode(nl_elem, CXT_Text, "line1\nline2");
  CPLXMLNode* empty_attr_elem =
      CPLCreateXMLNode(root.get(), CXT_Element, "empty_attr");
  CPLXMLNode* attr_node =
      CPLCreateXMLNode(empty_attr_elem, CXT_Attribute, "no_text_child");
  CPLCreateXMLNode(attr_node, CXT_Text, "");

  CPLMemoryCloser<char> xml(CPLSerializeXMLTree(root.get()));
  ASSERT_NE(nullptr, xml.get());
  EXPECT_NE(nullptr, strstr(xml.get(), "<!-- a comment -->"));
  EXPECT_NE(nullptr, strstr(xml.get(), "<raw_literal/>"));
  EXPECT_NE(nullptr, strstr(xml.get(), "no_text_child=\"\""));

  // File I/O.
  const char* kVsiFile = "/vsimem/minixml_test_file.xml";
  EXPECT_NE(0, CPLSerializeXMLTreeToFile(root.get(), kVsiFile));

  CPLXMLTreeCloser parsed_file(CPLParseXMLFile(kVsiFile));
  ASSERT_NE(nullptr, parsed_file.get());
  VSIUnlink(kVsiFile);

  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, CPLParseXMLFile("/vsimem/does_not_exist_minixml.xml"));
    // Writing into a non-existent directory must fail.
    const std::string missing_dir_file =
        ::testing::TempDir() + "/missing_dir_xyz/minixml.xml";
    EXPECT_EQ(0,
              CPLSerializeXMLTreeToFile(root.get(), missing_dir_file.c_str()));
  }

  // Test global ::CPLXMLTreeCloser::getDocumentElement().
  {
    ::CPLXMLTreeCloser empty_closer(nullptr);
    EXPECT_EQ(nullptr, empty_closer.getDocumentElement());

    ::CPLXMLTreeCloser doc_closer(CPLParseXMLString(
        "<?xml version=\"1.0\"?><!-- comment --><doc_elem><sub/></doc_elem>"));
    CPLXMLNode* doc_elem = doc_closer.getDocumentElement();
    ASSERT_NE(nullptr, doc_elem);
    EXPECT_STREQ("doc_elem", doc_elem->pszValue);

    ::CPLXMLTreeCloser comment_only(CPLParseXMLString("<!-- only comment -->"));
    EXPECT_EQ(nullptr, comment_only.getDocumentElement());
  }
}

}  // namespace
}  // namespace autotest2
