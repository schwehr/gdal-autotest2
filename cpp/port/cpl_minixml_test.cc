// Tests for minixml's ability to work with XML files and data structures.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_minixml.h
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_minixml.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include "port/cpl_minixml.h"

#include <set>
#include <string>

#include "gunit.h"
#include "autotest2/cpp/util/cpl_memory_closer.h"
#include "port/cpl_conv.h"

namespace autotest2 {
namespace {

// Manage a tree of XML nodes so that all nodes are freed when the instance goes
// out of scope.  Only the top level node should be in a CPLXMLTreeCloser.
class CPLXMLTreeCloser {
 public:
  explicit CPLXMLTreeCloser(CPLXMLNode *tree) : tree_(tree) {}
  // CPLDestroyXMLNode handles nullptr.
  ~CPLXMLTreeCloser() { CPLDestroyXMLNode(tree_); }

  // Modifying the contents pointed to by the return is allowed.
  CPLXMLNode *get() const { return tree_; }
  CPLXMLNode *operator->() const { return get(); }

 private:
  CPLXMLNode *tree_;
};


// Tests sanitizing a string into a safe XML element name.
TEST(CplMiniXmlTest, CleanXMLElementName) {
  // Null pointer is a NOP.  Ensure that a nullptr does not cause a crash
  // or leak memory.
  CPLCleanXMLElementName(nullptr);

  char empty[]="";
  CPLCleanXMLElementName(empty);

  char single_char_str[] = "a";
  CPLCleanXMLElementName(single_char_str);
  ASSERT_STREQ("a", single_char_str);

  // These ASCII character numbers should be transformed into an underscore.
  std::set<int> invalid_set = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45,
    // Valid: 46 ('.')
    47,
    // Valid: 48..57 ('0'..'9')
    58, 59, 60,
    61, 62, 63, 64,
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

  string unchanged(".0123456789"
                   "abcdefghijklmnopqrstuvwxyz"
                   "_"
                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  CPLMemoryCloser<char> result(CPLStrdup(unchanged.c_str()));
  CPLCleanXMLElementName(result.get());
  ASSERT_STREQ(unchanged.c_str(), result.get());


  string changed("\a\b\f\r\n\t\v !\"#$%&'()*+,-/"
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
  CPLMemoryCloser<char>
      xml_element(CPLSerializeXMLTree(element.get()));
  ASSERT_STREQ("< />\n", xml_element.get());

  // With a tag name.
  CPLXMLTreeCloser element_named(CPLCreateXMLNode(nullptr, CXT_Element,
                                                  "a_name"));
  ASSERT_STREQ("a_name", element_named->pszValue);
  CPLMemoryCloser<char>
      xml_element_named(CPLSerializeXMLTree(element_named.get()));
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
  CPLMemoryCloser<char>
      xml_comment(CPLSerializeXMLTree(comment.get()));
  ASSERT_STREQ("<!---->\n", xml_comment.get());

  CPLXMLTreeCloser literal(CPLCreateXMLNode(nullptr, CXT_Literal, nullptr));
  ASSERT_EQ(CXT_Literal, literal->eType);
  CPLMemoryCloser<char>
      xml_literal(CPLSerializeXMLTree(literal.get()));
  ASSERT_STREQ("\n", xml_literal.get());
}

// Tests creating and destroying a small XML node tree.
TEST(CplMiniXmlTest, CPLCreateXMLNodeSmallTree) {
  CPLXMLTreeCloser root(CPLCreateXMLNode(nullptr, CXT_Element, "the root"));
  CPLCreateXMLNode(root.get(), CXT_Literal, "2nd level leaf node - literal");
  CPLCreateXMLNode(root.get(), CXT_Text, "text leaf node");
  CPLXMLNode *attrib = CPLCreateXMLNode(root.get(), CXT_Attribute, "attrib");
  CPLCreateXMLNode(attrib, CXT_Text, "text for attrib");

  ASSERT_EQ(CXT_Literal, root->psChild->eType);

  CPLMemoryCloser<char> xml(CPLSerializeXMLTree(root.get()));
  ASSERT_STREQ("<the root attrib=\"text for attrib\">\n"
               "  2nd level leaf node - literal\n"
               "text leaf node</the root>\n", xml.get());
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

  CPLXMLNode *attribute = root->psChild;
  EXPECT_EQ(CXT_Attribute, attribute->eType);
  EXPECT_STREQ("foo", attribute->pszValue);
  EXPECT_EQ(nullptr, attribute->psNext);
  ASSERT_NE(nullptr, attribute->psChild);

  CPLXMLNode *value = attribute->psChild;
  EXPECT_EQ(CXT_Text, value->eType);
  EXPECT_STREQ("2", value->pszValue);
  EXPECT_EQ(nullptr, value->psNext);
  EXPECT_EQ(nullptr, value->psChild);
}

void CheckXmlNode(CPLXMLNode *node, CPLXMLNodeType e_type, const char *value,
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
  CPLXMLTreeCloser root(CPLParseXMLString(
      "<a b='c'> <d e='f'> <g h='i'/> </d> <j/> </a>"));
  ASSERT_NE(nullptr, root.get());
  ASSERT_EQ(nullptr, CPLGetXMLNode(root.get(), "does_not_exist"));

  EXPECT_EQ(nullptr, CPLGetXMLNode(root.get(), "a"));

  CPLXMLNode *a = CPLGetXMLNode(root.get(), "=a");
  CheckXmlNode(a, CXT_Element, "a", false, true);

  CPLXMLNode *b = CPLGetXMLNode(root.get(), "b");
  CheckXmlNode(b, CXT_Attribute, "b", true, true);

  b = CPLGetXMLNode(root.get(), "=a.b");
  CheckXmlNode(b, CXT_Attribute, "b", true, true);

  CPLXMLNode *d = CPLGetXMLNode(root.get(), "d");
  CheckXmlNode(d, CXT_Element, "d", true, true);

  CPLXMLNode *e = CPLGetXMLNode(root.get(), "d.e");
  CheckXmlNode(e, CXT_Attribute, "e", true, true);

  CPLXMLNode *g = CPLGetXMLNode(root.get(), "d.g");
  CheckXmlNode(g, CXT_Element, "g", false, true);

  CPLXMLNode *h = CPLGetXMLNode(root.get(), "d.g.h");
  CheckXmlNode(h, CXT_Attribute, "h", false, true);

  CPLXMLNode *j = CPLGetXMLNode(root.get(), "j");
  CheckXmlNode(j, CXT_Element, "j", false, false);
}

TEST(CplMiniXmlTest, Search) {
  CPLXMLTreeCloser root(CPLParseXMLString(
      "<a b='c'> <d e='f'> <g h='i'/> </d> <j/> </a>"));

  EXPECT_EQ(nullptr, CPLSearchXMLNode(nullptr, "z"));
  EXPECT_EQ(nullptr, CPLSearchXMLNode(root.get(), "zz"));

  EXPECT_STREQ("a", CPLSearchXMLNode(root.get(), "a")->pszValue);
  EXPECT_STREQ("a", CPLSearchXMLNode(root.get(), "=a")->pszValue);
  CheckXmlNode(CPLSearchXMLNode(root.get(), "b"),
               CXT_Attribute, "b", true, true);

  EXPECT_STREQ("g", CPLSearchXMLNode(root.get(), "g")->pszValue);
  CheckXmlNode(CPLSearchXMLNode(root.get(), "h"),
               CXT_Attribute, "h", false, true);

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

}  // namespace
}  // namespace autotest2
