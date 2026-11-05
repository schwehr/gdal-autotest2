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

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::IsNull;
using ::testing::NotNull;
using ::testing::StrEq;

class TestListener : public OGR_SRSNode::Listener {
 public:
  void notifyChange(OGR_SRSNode* node) override {
    change_count_++;
    last_node_ = node;
  }

  int change_count_ = 0;
  OGR_SRSNode* last_node_ = nullptr;
};

TEST(OgrSrsNodeTest, ConstructorAndDestructorAndListener) {
  auto root = std::make_unique<OGR_SRSNode>("ROOT");
  EXPECT_STREQ("ROOT", root->GetValue());
  EXPECT_EQ(0, root->GetChildCount());
  EXPECT_TRUE(root->IsLeafNode());

  auto listener = std::make_shared<TestListener>();
  root->RegisterListener(listener);

  root->SetValue("NEW_ROOT");
  EXPECT_STREQ("NEW_ROOT", root->GetValue());
  EXPECT_EQ(1, listener->change_count_);
  EXPECT_EQ(root.get(), listener->last_node_);

  auto child = new OGR_SRSNode("CHILD");
  root->AddChild(child);
  EXPECT_EQ(1, root->GetChildCount());
  EXPECT_FALSE(root->IsLeafNode());
  EXPECT_EQ(2, listener->change_count_);

  auto child2 = new OGR_SRSNode("CHILD2");
  root->InsertChild(child2, 0);
  EXPECT_EQ(2, root->GetChildCount());
  EXPECT_EQ(3, listener->change_count_);

  root->DestroyChild(0);
  EXPECT_EQ(1, root->GetChildCount());
  EXPECT_EQ(4, listener->change_count_);

  // When listener is expired (shared_ptr reset), notifyChange should not crash.
  listener.reset();
  root->SetValue("ANOTHER_ROOT");
}

TEST(OgrSrsNodeTest, ChildrenAndTreeNavigation) {
  OGR_SRSNode root("GEOGCS");
  auto* child0 = new OGR_SRSNode("WGS 84");
  auto* child1 = new OGR_SRSNode("DATUM");
  child1->AddChild(new OGR_SRSNode("WGS_1984"));
  auto* spheroid = new OGR_SRSNode("SPHEROID");
  spheroid->AddChild(new OGR_SRSNode("WGS 84"));
  spheroid->AddChild(new OGR_SRSNode("6378137"));
  child1->AddChild(spheroid);

  root.AddChild(child0);
  root.AddChild(child1);

  // Check child count and access
  EXPECT_EQ(2, root.GetChildCount());
  EXPECT_EQ(child0, root.GetChild(0));
  EXPECT_EQ(child1, root.GetChild(1));
  EXPECT_THAT(root.GetChild(-1), IsNull());
  EXPECT_THAT(root.GetChild(2), IsNull());

  // Const GetChild
  const auto& const_root = root;
  EXPECT_EQ(child0, const_root.GetChild(0));
  EXPECT_THAT(const_root.GetChild(-1), IsNull());
  EXPECT_THAT(const_root.GetChild(2), IsNull());

  // FindChild
  EXPECT_EQ(0, root.FindChild("wgs 84"));
  EXPECT_EQ(1, root.FindChild("datum"));
  EXPECT_EQ(-1, root.FindChild("nonexistent"));

  // GetNode (pre-order traversal)
  // Leaf nodes are not considered by GetNode
  EXPECT_THAT(root.GetNode("WGS 84"), IsNull());
  // Non-leaf root
  EXPECT_EQ(&root, root.GetNode("GEOGCS"));
  // Immediate non-leaf child
  EXPECT_EQ(child1, root.GetNode("DATUM"));
  // Grandchild non-leaf
  EXPECT_EQ(spheroid, root.GetNode("SPHEROID"));
  // Non-existent non-leaf
  EXPECT_THAT(root.GetNode("UNKNOWN"), IsNull());

  // Const GetNode
  EXPECT_EQ(spheroid, const_root.GetNode("SPHEROID"));
  EXPECT_THAT(const_root.GetNode("UNKNOWN"), IsNull());

  // InsertChild beyond current count should clamp
  auto* child_extra = new OGR_SRSNode("EXTRA");
  root.InsertChild(child_extra, 100);
  EXPECT_EQ(3, root.GetChildCount());
  EXPECT_EQ(child_extra, root.GetChild(2));

  // DestroyChild out of range does nothing
  root.DestroyChild(-1);
  root.DestroyChild(10);
  EXPECT_EQ(3, root.GetChildCount());

  // DestroyChild at middle
  root.DestroyChild(1);
  EXPECT_EQ(2, root.GetChildCount());
  EXPECT_EQ(child_extra, root.GetChild(1));

  // Clone
  std::unique_ptr<OGR_SRSNode> cloned(root.Clone());
  ASSERT_THAT(cloned, NotNull());
  EXPECT_STREQ("GEOGCS", cloned->GetValue());
  EXPECT_EQ(2, cloned->GetChildCount());
  EXPECT_STREQ("WGS 84", cloned->GetChild(0)->GetValue());

  // ClearChildren
  root.ClearChildren();
  EXPECT_EQ(0, root.GetChildCount());
}

TEST(OgrSrsNodeTest, NeedsQuotingAndExportWkt) {
  // Test quoting rules:
  // AUTHORITY children must be quoted even if numeric
  auto auth = std::make_unique<OGR_SRSNode>("AUTHORITY");
  auth->AddChild(new OGR_SRSNode("EPSG"));
  auth->AddChild(new OGR_SRSNode("4326"));

  // AXIS: 1st child quoted, 2nd child not quoted
  auto axis1 = std::make_unique<OGR_SRSNode>("AXIS");
  axis1->AddChild(new OGR_SRSNode("E"));
  axis1->AddChild(new OGR_SRSNode("EAST"));

  auto axis2 = std::make_unique<OGR_SRSNode>("AXIS");
  axis2->AddChild(new OGR_SRSNode("easting"));
  axis2->AddChild(new OGR_SRSNode("NORTH"));

  // CS: 1st child not quoted
  auto cs = std::make_unique<OGR_SRSNode>("CS");
  cs->AddChild(new OGR_SRSNode("ellipsoidal"));
  cs->AddChild(new OGR_SRSNode("2"));

  // Clean numerics not quoted
  auto num1 = std::make_unique<OGR_SRSNode>("123.45");
  auto num2 = std::make_unique<OGR_SRSNode>("-1e5");
  auto num3 = std::make_unique<OGR_SRSNode>("+3.14E-2");

  // Non-numeric string quoted
  auto str1 = std::make_unique<OGR_SRSNode>("WGS 84");

  auto root = std::make_unique<OGR_SRSNode>("ROOT");
  root->AddChild(auth.release());
  root->AddChild(axis1.release());
  root->AddChild(axis2.release());
  root->AddChild(cs.release());
  root->AddChild(num1.release());
  root->AddChild(num2.release());
  root->AddChild(num3.release());
  root->AddChild(str1.release());

  char* wkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, root->exportToWkt(&wkt));
  ASSERT_THAT(wkt, NotNull());
  std::string s_wkt(wkt);
  CPLFree(wkt);

  EXPECT_THAT(s_wkt, HasSubstr("AUTHORITY[\"EPSG\",\"4326\"]"));
  EXPECT_THAT(s_wkt, HasSubstr("AXIS[\"E\",EAST]"));
  EXPECT_THAT(s_wkt, HasSubstr("AXIS[\"easting\",NORTH]"));
  EXPECT_THAT(s_wkt, HasSubstr("CS[ellipsoidal,2]"));
  EXPECT_THAT(s_wkt, HasSubstr("123.45"));
  EXPECT_THAT(s_wkt, HasSubstr("-1e5"));
  EXPECT_THAT(s_wkt, HasSubstr("+3.14E-2"));
  EXPECT_THAT(s_wkt, HasSubstr("\"WGS 84\""));

  // Pretty WKT
  char* pretty_wkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, root->exportToPrettyWkt(&pretty_wkt, 1));
  ASSERT_THAT(pretty_wkt, NotNull());
  std::string s_pretty(pretty_wkt);
  CPLFree(pretty_wkt);
  EXPECT_THAT(s_pretty, HasSubstr("\n"));

  // Export leaf node directly
  OGR_SRSNode leaf("single_token");
  char* leaf_wkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, leaf.exportToWkt(&leaf_wkt));
  EXPECT_STREQ("\"single_token\"", leaf_wkt);
  CPLFree(leaf_wkt);

  char* leaf_pretty = nullptr;
  EXPECT_EQ(OGRERR_NONE, leaf.exportToPrettyWkt(&leaf_pretty, 0));
  EXPECT_STREQ("\"single_token\"", leaf_pretty);
  CPLFree(leaf_pretty);

  OGR_SRSNode num_leaf("42");
  char* num_wkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, num_leaf.exportToWkt(&num_wkt));
  EXPECT_STREQ("42", num_wkt);
  CPLFree(num_wkt);
}

TEST(OgrSrsNodeTest, ImportFromWktSuccessAndFailures) {
  // Deprecated importFromWkt(char **)
  {
    std::string text = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\"]]";
    std::vector<char> buf(text.begin(), text.end());
    buf.push_back('\0');
    char* ptr = buf.data();
    OGR_SRSNode node;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    EXPECT_EQ(OGRERR_NONE, node.importFromWkt(&ptr));
#pragma clang diagnostic pop
    EXPECT_STREQ("GEOGCS", node.GetValue());
    EXPECT_EQ(2, node.GetChildCount());
  }

  // Valid import with parentheses and whitespace
  {
    const char* text =
        "  PROJCS ( \"UTM 10\" ,\n\tDATUM ( \"NAD83\" ) , UNIT ( \"metre\" , "
        "1.0 ) )  ";
    const char* ptr = text;
    OGR_SRSNode node;
    EXPECT_EQ(OGRERR_NONE, node.importFromWkt(&ptr));
    EXPECT_STREQ("PROJCS", node.GetValue());
    EXPECT_EQ(3, node.GetChildCount());
  }

  // Quoted string containing special characters like [, ], ,
  {
    const char* text = "NODE[\"Val [1, 2] with brackets\"]";
    const char* ptr = text;
    OGR_SRSNode node;
    EXPECT_EQ(OGRERR_NONE, node.importFromWkt(&ptr));
    EXPECT_STREQ("NODE", node.GetValue());
    EXPECT_EQ(1, node.GetChildCount());
    EXPECT_STREQ("Val [1, 2] with brackets", node.GetChild(0)->GetValue());
  }

  // Error case: Empty string
  {
    const char* text = "";
    const char* ptr = text;
    OGR_SRSNode node;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, node.importFromWkt(&ptr));
  }

  // Error case: Unclosed bracket
  {
    const char* text = "PROJCS[\"abc\",DATUM[\"d\"]";
    const char* ptr = text;
    OGR_SRSNode node;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, node.importFromWkt(&ptr));
  }

  // Error case: Missing closing bracket after child
  {
    const char* text = "PROJCS[\"a\"!";
    const char* ptr = text;
    OGR_SRSNode node;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, node.importFromWkt(&ptr));
  }

  // Error case: Recursion depth == 10
  {
    const char* text = "A[B[C[D[E[F[G[H[I[J[K[\"x\"]]]]]]]]]]]";
    const char* ptr = text;
    OGR_SRSNode node;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, node.importFromWkt(&ptr));
  }

  // Error case: 1000 nodes limit
  {
    std::string deep = "ROOT[";
    for (int i = 0; i < 1005; ++i) {
      if (i > 0) deep += ",";
      deep += "\"a\"";
    }
    deep += "]";
    const char* ptr = deep.c_str();
    OGR_SRSNode node;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, node.importFromWkt(&ptr));
  }

  // Error case: Token length >= 512
  {
    std::string long_token = "\"" + std::string(520, 'x') + "\"";
    const char* ptr = long_token.c_str();
    OGR_SRSNode node;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, node.importFromWkt(&ptr));
  }
}

TEST(OgrSrsNodeTest, MakeValueSafe) {
  OGR_SRSNode root("ROOT");
  auto* num_node = new OGR_SRSNode("12345");
  auto* normal_node = new OGR_SRSNode("NormalName");
  auto* safe_test1 = new OGR_SRSNode(".abc!@#def_123!!");
  auto* safe_test2 = new OGR_SRSNode(".abc!@#def9");

  root.AddChild(num_node);
  root.AddChild(normal_node);
  root.AddChild(safe_test1);
  root.AddChild(safe_test2);

  root.MakeValueSafe();

  // Numeric node unchanged
  EXPECT_STREQ("12345", num_node->GetValue());
  // Node not starting with '.' unchanged
  EXPECT_STREQ("NormalName", normal_node->GetValue());
  // Node ending with non-alphanumeric strips trailing underscore
  EXPECT_STREQ("_abc_def_123", safe_test1->GetValue());
  // Node ending with alphanumeric keeps final character
  EXPECT_STREQ("_abc_def9", safe_test2->GetValue());
}

TEST(OgrSrsNodeTest, StripNodes) {
  OGR_SRSNode root("ROOT");
  auto* child0 = new OGR_SRSNode("STRIP_ME");
  auto* child1 = new OGR_SRSNode("KEEP");
  auto* subchild = new OGR_SRSNode("STRIP_ME");
  child1->AddChild(subchild);
  child1->AddChild(new OGR_SRSNode("KEEP_SUB"));

  root.AddChild(child0);
  root.AddChild(child1);

  root.StripNodes("STRIP_ME");

  EXPECT_EQ(1, root.GetChildCount());
  EXPECT_STREQ("KEEP", root.GetChild(0)->GetValue());
  EXPECT_EQ(1, root.GetChild(0)->GetChildCount());
  EXPECT_STREQ("KEEP_SUB", root.GetChild(0)->GetChild(0)->GetValue());
}

}  // namespace
}  // namespace autotest2
