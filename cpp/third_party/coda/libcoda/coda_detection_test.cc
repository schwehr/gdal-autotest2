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
// Tests CODA detection rule entry construction, detection tree building, path
// prefix splitting, node ordering, backtracking evaluation, and .codadef
// product recognition (libcoda/coda-detection.c).
//
// google3 build caveats:
//   - coda_ascii_cursor_set_asciilines() (libcoda/coda-ascii-cursor.c) leaks a
//     coda_type_text on ASCII line access; the asciiline evaluation sub-case
//     uses absl::LeakCheckDisabler to stay ASan-clean.
//   - No libcoda/coda-detection.h header exists upstream; detection rule/node
//     structs and coda_evaluate_detection_node() live in coda-definition.h, and
//     coda_detection_tree_{add_rule,delete}() are declared via
//     coda_internal_prelude.h.
//   - Coverage ceiling on libcoda/coda-detection.c is 244/275 (88.7%) = 100%
//     of reachable non-OOM, non-dead lines: the 31 missed lines (67-68, 81, 83,
//     93, 95-96, 140, 142, 164-166, 201, 203, 225-227, 328, 333-335, 341-342,
//     378, 385, 387, 396-397, 431, 440, 493) are malloc/realloc/strdup OOM
//     paths (28 lines) or unreachable defensive checks (67-68, 493).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-detection.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-definition.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-definition.c

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/netcdf_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-definition.h"
#include "third_party/stcorp_coda/libcoda/coda-expr.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

struct DetectionEntryDeleter {
  void operator()(coda_detection_rule_entry* entry) const {
    if (entry != nullptr) {
      coda_detection_rule_entry_delete(entry);
    }
  }
};

using ScopedDetectionEntry =
    std::unique_ptr<coda_detection_rule_entry, DetectionEntryDeleter>;

struct DetectionRuleDeleter {
  void operator()(coda_detection_rule* rule) const {
    if (rule != nullptr) {
      coda_detection_rule_delete(rule);
    }
  }
};

using ScopedDetectionRule =
    std::unique_ptr<coda_detection_rule, DetectionRuleDeleter>;

struct ProductDefDeleter {
  void operator()(coda_product_definition* def) const {
    if (def != nullptr) {
      coda_product_definition_delete(def);
    }
  }
};

using ScopedProductDef =
    std::unique_ptr<coda_product_definition, ProductDefDeleter>;

// Owns a coda_detection_node* root. Must be declared after any rules and
// product definitions referenced by its nodes because tree nodes hold
// non-owning pointers to coda_detection_rule and coda_expression.
class ScopedDetectionTree {
 public:
  ScopedDetectionTree() = default;
  ~ScopedDetectionTree() { Reset(); }

  ScopedDetectionTree(const ScopedDetectionTree&) = delete;
  ScopedDetectionTree& operator=(const ScopedDetectionTree&) = delete;

  void Reset() {
    if (root_ != nullptr) {
      // coda_detection_tree_delete() unconditionally dereferences its argument.
      coda_detection_tree_delete(root_);
      root_ = nullptr;
    }
  }

  coda_detection_node** Addr() { return &root_; }
  coda_detection_node* get() const { return root_; }

 private:
  coda_detection_node* root_ = nullptr;
};

// Parses `expr_str` and returns an owning ScopedExpr (empty on failure).
ScopedExpr ParseExpr(const char* expr_str) {
  coda_expression* raw = nullptr;
  if (coda_expression_from_string(expr_str, &raw) != 0) {
    return ScopedExpr();
  }
  return ScopedExpr(raw);
}

// Builds a rule entry with optional `path` and optional boolean `expr_str`.
ScopedDetectionEntry MakeEntry(const char* path, const char* expr_str) {
  ScopedDetectionEntry entry(coda_detection_rule_entry_new(path));
  if (!entry) {
    return ScopedDetectionEntry();
  }
  if (expr_str != nullptr) {
    ScopedExpr expr = ParseExpr(expr_str);
    if (!expr) {
      return ScopedDetectionEntry();
    }
    if (coda_detection_rule_entry_set_expression(entry.get(), expr.get()) !=
        0) {
      return ScopedDetectionEntry();
    }
    expr.release();
  }
  return entry;
}

// Builds a detection rule bound to `def` from (path, expr_str) pairs.
ScopedDetectionRule MakeRule(
    coda_product_definition* def,
    std::initializer_list<std::pair<const char*, const char*>> specs) {
  ScopedDetectionRule rule(coda_detection_rule_new());
  if (!rule) {
    return ScopedDetectionRule();
  }
  rule->product_definition = def;
  for (const auto& [path, expr_str] : specs) {
    ScopedDetectionEntry entry = MakeEntry(path, expr_str);
    if (!entry) {
      return ScopedDetectionRule();
    }
    if (coda_detection_rule_add_entry(rule.get(), entry.get()) != 0) {
      return ScopedDetectionRule();
    }
    entry.release();
  }
  return rule;
}

std::vector<uint8_t> EncodeBeInt32(int32_t value) {
  std::vector<uint8_t> bytes;
  NetcdfBuilder::AppendBe32(bytes, static_cast<uint32_t>(value));
  return bytes;
}

std::vector<uint8_t> EncodeBeDouble(double value) {
  std::vector<uint8_t> bytes;
  NetcdfBuilder::AppendBe64(bytes, std::bit_cast<uint64_t>(value));
  return bytes;
}

std::vector<uint8_t> EncodeAsciiBytes(std::string_view text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void AppendText(std::vector<uint8_t>& out, std::string_view text) {
  out.insert(out.end(), text.begin(), text.end());
}

// Synthesizes a NetCDF classic file with global/variable attributes, scalars,
// and an int32 array so path/attribute/int/float/bytes rules can be evaluated.
std::vector<uint8_t> BuildSampleNetcdf(std::string_view global_tag,
                                       std::string_view magic,
                                       std::optional<std::string_view> kind,
                                       int32_t ver, double scale,
                                       std::array<int32_t, 2> arr_vals) {
  NetcdfBuilder nc(1);
  nc.AddDim({.name = "n", .size = 2});
  if (!global_tag.empty()) {
    nc.AddGlobalAttr({
        .name = "tag",
        .type = NetcdfType::kChar,
        .count = static_cast<int64_t>(global_tag.size()),
        .raw_values = EncodeAsciiBytes(global_tag),
    });
  }

  std::vector<NetcdfAttrSpec> magic_attrs;
  if (kind.has_value()) {
    magic_attrs.push_back({
        .name = "kind",
        .type = NetcdfType::kChar,
        .count = static_cast<int64_t>(kind->size()),
        .raw_values = EncodeAsciiBytes(*kind),
    });
  }
  nc.AddDim({.name = "mlen", .size = static_cast<int64_t>(magic.size())});
  nc.AddVar({
      .name = "magic",
      .dim_ids = {1},
      .attrs = std::move(magic_attrs),
      .type = NetcdfType::kChar,
      .vsize = static_cast<int64_t>(((magic.size() + 3) / 4) * 4),
      .data = EncodeAsciiBytes(magic),
  });
  nc.AddVar({
      .name = "ver",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 4,
      .data = EncodeBeInt32(ver),
  });
  nc.AddVar({
      .name = "scale",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kDouble,
      .vsize = 8,
      .data = EncodeBeDouble(scale),
  });

  std::vector<uint8_t> arr_bytes;
  NetcdfBuilder::AppendBe32(arr_bytes, static_cast<uint32_t>(arr_vals[0]));
  NetcdfBuilder::AppendBe32(arr_bytes, static_cast<uint32_t>(arr_vals[1]));
  nc.AddVar({
      .name = "arr",
      .dim_ids = {0},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 8,
      .data = std::move(arr_bytes),
  });
  return nc.Build();
}

constexpr std::string_view kCodadefIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="DetectClass">
  <cd:ProductType name="BaseBin">
    <cd:ProductDefinition id="base_bin" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">BINHDR</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="ChildBin">
    <cd:ProductDefinition id="child_bin" format="binary" version="2">
      <cd:DetectionRule>
        <cd:MatchData offset="0">BINHDR</cd:MatchData>
        <cd:MatchFilename offset="0">child_</cd:MatchFilename>
        <cd:MatchSize size="16"/>
        <cd:MatchExpression>bytes(., 8, 4) == "SPEC"</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="NestedBin">
    <cd:ProductDefinition id="nested_bin" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">NESTED01</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="AscDoc">
    <cd:ProductDefinition id="asc_doc" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASCHDR</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)xml";

constexpr std::string_view kNestedBinXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="nested_bin" format="binary" version="1">
  <cd:Record format="binary">
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="hdr">
      <cd:Record>
        <cd:Field name="a">
          <cd:Integer>
            <cd:NativeType>int32</cd:NativeType>
            <cd:BitSize>32</cd:BitSize>
            <cd:LittleEndian/>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="b">
          <cd:Integer>
            <cd:NativeType>int32</cd:NativeType>
            <cd:BitSize>32</cd:BitSize>
            <cd:LittleEndian/>
          </cd:Integer>
        </cd:Field>
      </cd:Record>
    </cd:Field>
    <cd:Field name="arr">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int32</cd:NativeType>
          <cd:BitSize>32</cd:BitSize>
          <cd:LittleEndian/>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kAscDocXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="asc_doc" format="ascii" version="1">
  <cd:Record format="ascii">
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>6</cd:ByteSize>
      </cd:Text>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

std::vector<uint8_t> BuildDetectionCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 2>
      kExtraEntries = {{
          {"products/nested_bin.xml", kNestedBinXml},
          {"products/asc_doc.xml", kAscDocXml},
      }};
  return BuildCodadefBytes(kCodadefIndexXml, kExtraEntries);
}

std::vector<uint8_t> BuildNestedBinProductBytes(int32_t a, int32_t b,
                                                int32_t arr0, int32_t arr1) {
  std::vector<uint8_t> bytes;
  AppendText(bytes, "NESTED01");
  AppendLe32(bytes, static_cast<uint32_t>(a));
  AppendLe32(bytes, static_cast<uint32_t>(b));
  AppendLe32(bytes, static_cast<uint32_t>(arr0));
  AppendLe32(bytes, static_cast<uint32_t>(arr1));
  return bytes;
}

TEST(CodaDetectionTest, RuleEntryLifecycleAndErrorPaths) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. NULL path creates an empty entry cleanly deleted with no path/expr.
  {
    ScopedDetectionEntry empty(coda_detection_rule_entry_new(nullptr));
    ASSERT_NE(nullptr, empty.get());
    EXPECT_EQ(nullptr, empty->path);
    EXPECT_EQ(nullptr, empty->expression);
  }

  // 2. Invalid path syntax fails in coda_expression_from_string().
  for (const char* bad_syntax : {"", "invalid[", "/a/"}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(nullptr, coda_detection_rule_entry_new(bad_syntax))
        << "bad_syntax=" << bad_syntax;
    EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno) << "bad_syntax=" << bad_syntax;
  }

  // 3. Valid non-node expressions fail with CODA_ERROR_DATA_DEFINITION.
  constexpr const char* kNonNodeExprs[] = {
      "1 + 2",
      "true",
      "\"literal\"",
      "filesize() == 10",
  };
  for (const char* non_node : kNonNodeExprs) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(nullptr, coda_detection_rule_entry_new(non_node))
        << "non_node=" << non_node;
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno)
        << "non_node=" << non_node;
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("not a valid path"));
  }

  // 4. Valid node paths populate entry->path; setting expression twice fails.
  for (const char* valid_path : {"/header/magic", "@tag", "/arr[0]"}) {
    ScopedDetectionEntry entry(coda_detection_rule_entry_new(valid_path));
    ASSERT_NE(nullptr, entry.get()) << "valid_path=" << valid_path;
    EXPECT_STREQ(valid_path, entry->path);
    EXPECT_EQ(nullptr, entry->expression);

    ScopedExpr expr1 = ParseExpr("filesize() > 0");
    ASSERT_NE(nullptr, expr1.get());
    ASSERT_EQ(CODA_SUCCESS, coda_detection_rule_entry_set_expression(
                                entry.get(), expr1.get()));
    expr1.release();
    EXPECT_NE(nullptr, entry->expression);

    ScopedExpr expr2 = ParseExpr("filesize() == 16");
    ASSERT_NE(nullptr, expr2.get());
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(
        -1, coda_detection_rule_entry_set_expression(entry.get(), expr2.get()));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("already has an expression"));
  }
}

TEST(CodaDetectionTest, RuleLifecycleAndAddEntryValidation) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Delete an empty rule (entry == nullptr branch in
  // coda_detection_rule_delete).
  {
    ScopedDetectionRule empty_rule(coda_detection_rule_new());
    ASSERT_NE(nullptr, empty_rule.get());
    EXPECT_EQ(0, empty_rule->num_entries);
    EXPECT_EQ(nullptr, empty_rule->entry);
    EXPECT_EQ(nullptr, empty_rule->product_definition);
  }

  // 2. Adding an entry with both path == NULL and expression == NULL fails.
  ScopedDetectionRule rule(coda_detection_rule_new());
  ASSERT_NE(nullptr, rule.get());
  ScopedDetectionEntry blank_entry(coda_detection_rule_entry_new(nullptr));
  ASSERT_NE(nullptr, blank_entry.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_detection_rule_add_entry(rule.get(), blank_entry.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("path and/or an expression"));

  // 3. Add path-only, expression-only, and combined entries.
  ScopedDetectionEntry path_only = MakeEntry("/hdr", nullptr);
  ASSERT_NE(nullptr, path_only.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_rule_add_entry(rule.get(), path_only.get()));
  path_only.release();

  ScopedDetectionEntry expr_only = MakeEntry(nullptr, "filesize() > 4");
  ASSERT_NE(nullptr, expr_only.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_rule_add_entry(rule.get(), expr_only.get()));
  expr_only.release();

  ScopedDetectionEntry both = MakeEntry("/hdr/ver", "int(.) == 1");
  ASSERT_NE(nullptr, both.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_rule_add_entry(rule.get(), both.get()));
  both.release();

  EXPECT_EQ(3, rule->num_entries);
}

TEST(CodaDetectionTest, TreeAddRuleErrorsAndShadowing) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedProductDef def_a(
      coda_product_definition_new("DefA", coda_format_binary, 1));
  ScopedProductDef def_b(
      coda_product_definition_new("DefB", coda_format_binary, 1));
  ScopedProductDef def_c(
      coda_product_definition_new("DefC", coda_format_binary, 1));
  ScopedProductDef def_d(
      coda_product_definition_new("DefD", coda_format_binary, 1));
  ASSERT_NE(nullptr, def_a.get());
  ASSERT_NE(nullptr, def_b.get());
  ASSERT_NE(nullptr, def_c.get());
  ASSERT_NE(nullptr, def_d.get());

  ScopedDetectionRule empty_rule = MakeRule(def_a.get(), {});
  ScopedDetectionRule rule_a = MakeRule(def_a.get(), {{"/a/b", nullptr}});
  ScopedDetectionRule rule_b = MakeRule(def_b.get(), {{"/a/b", nullptr}});
  ScopedDetectionRule rule_c =
      MakeRule(def_c.get(), {{"/x", "filesize() > 0"}});
  ScopedDetectionRule rule_d =
      MakeRule(def_d.get(), {{"/x", "filesize() > 0"}});
  ASSERT_NE(nullptr, empty_rule.get());
  ASSERT_NE(nullptr, rule_a.get());
  ASSERT_NE(nullptr, rule_b.get());
  ASSERT_NE(nullptr, rule_c.get());
  ASSERT_NE(nullptr, rule_d.get());

  ScopedDetectionTree tree;

  // 1. Adding a rule with 0 entries fails without allocating a root node.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_detection_tree_add_rule(tree.Addr(), empty_rule.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("should have at least one entry"));
  EXPECT_EQ(nullptr, tree.get());

  // 2. Identical path-only rule is rejected as shadowed.
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), rule_a.get()));
  ASSERT_NE(nullptr, tree.get());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_detection_tree_add_rule(tree.Addr(), rule_b.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("shadowed"));
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("DefB"));
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("DefA"));

  // 3. Identical path + equal expression rule is also rejected as shadowed.
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), rule_c.get()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_detection_tree_add_rule(tree.Addr(), rule_d.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("DefD"));
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("DefC"));
}

TEST(CodaDetectionTest, TreeConstructionSplittingOrderingAndBlockGrowth) {
  std::vector<ScopedProductDef> defs;
  std::vector<ScopedDetectionRule> rules;
  ScopedDetectionTree tree;

  auto add_rule =
      [&](std::string_view name,
          std::initializer_list<std::pair<const char*, const char*>> specs)
      -> void {
    std::string id(name);
    defs.emplace_back(
        coda_product_definition_new(id.c_str(), coda_format_binary, 1));
    ASSERT_NE(nullptr, defs.back().get());
    rules.emplace_back(MakeRule(defs.back().get(), specs));
    ASSERT_NE(nullptr, rules.back().get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_detection_tree_add_rule(tree.Addr(), rules.back().get()));
  };

  // 1. Expression-first insertion followed by field path then @attribute path
  // exercises path-before-expression and @attribute-before-field swapping.
  ASSERT_NO_FATAL_FAILURE(add_rule("R_Expr", {{nullptr, "filesize() > 0"}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_Field", {{"/field", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_Attr", {{"@attr", nullptr}}));

  ASSERT_NE(nullptr, tree.get());
  ASSERT_EQ(3, tree.get()->num_subnodes);
  EXPECT_STREQ("@attr", tree.get()->subnode[0]->path);
  EXPECT_STREQ("/field", tree.get()->subnode[1]->path);
  EXPECT_EQ(nullptr, tree.get()->subnode[2]->path);
  EXPECT_NE(nullptr, tree.get()->subnode[2]->expression);

  // 2. Strict prefix extensions via '/', '@', '[', plus non-separator sibling.
  ASSERT_NO_FATAL_FAILURE(add_rule("R_Prefix", {{"/p", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_PrefixSlash", {{"/p/child", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_PrefixAttr", {{"/p@tag", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_PrefixIdx", {{"/p[0]", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_NonSep", {{"/p2", nullptr}}));

  // Locate "/p" and verify its 3 children ('@tag' swapped before 'child').
  coda_detection_node* p_node = nullptr;
  coda_detection_node* p2_node = nullptr;
  for (int i = 0; i < tree.get()->num_subnodes; ++i) {
    const char* path = tree.get()->subnode[i]->path;
    if (path != nullptr && std::string_view(path) == "/p") {
      p_node = tree.get()->subnode[i];
    } else if (path != nullptr && std::string_view(path) == "/p2") {
      p2_node = tree.get()->subnode[i];
    }
  }
  ASSERT_NE(nullptr, p_node);
  ASSERT_NE(nullptr, p2_node);
  ASSERT_EQ(3, p_node->num_subnodes);
  EXPECT_STREQ("@tag", p_node->subnode[0]->path);
  EXPECT_STREQ("child", p_node->subnode[1]->path);
  EXPECT_STREQ("[0]", p_node->subnode[2]->path);

  // 3. Node splitting on common prefix ending with '/', '[', and '/' vs '@'.
  ASSERT_NO_FATAL_FAILURE(add_rule("R_SplitA", {{"/split/alpha", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_SplitB", {{"/split/beta", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_Arr0", {{"/arr[0]", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_Arr1", {{"/arr[1]", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_MixField", {{"/mix/x", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_MixAttr", {{"/mix@y", nullptr}}));

  // 4. Shared path + expression combinations (path before expr, expr before
  // path, equal expr reuse with continuation, unequal expr sibling).
  ASSERT_NO_FATAL_FAILURE(add_rule("R_SharedPath", {{"/shared", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(
      add_rule("R_SharedChild", {{"/shared/child", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(
      add_rule("R_SharedExpr1", {{"/shared", "int(/shared) == 1"}}));
  ASSERT_NO_FATAL_FAILURE(
      add_rule("R_SharedExpr2", {{"/shared", "int(/shared) == 2"}}));
  ASSERT_NO_FATAL_FAILURE(
      add_rule("R_SharedExpr1Ext",
               {{"/shared", "int(/shared) == 1"}, {"/extra", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_GrpSub", {{"/grp/sub", nullptr}}));
  ASSERT_NO_FATAL_FAILURE(add_rule("R_GrpExpr", {{"/grp", "filesize() > 10"}}));

  // 5. Cross BLOCK_SIZE (16) subnode realloc boundary at root.
  for (int i = 0; i < 18; ++i) {
    const std::string def_name = absl::StrCat("R_Bulk_", i);
    const std::string path = absl::StrCat("/bulk_", i);
    ASSERT_NO_FATAL_FAILURE(add_rule(def_name, {{path.c_str(), nullptr}}));
  }
  EXPECT_GE(tree.get()->num_subnodes, 24);
}

TEST(CodaDetectionTest, TreeAddRuleNormalizesEmptySubpathToNull) {
  ScopedProductDef def(
      coda_product_definition_new("EmptySubpathDef", coda_format_binary, 1));
  ASSERT_NE(nullptr, def.get());

  ScopedDetectionRule rule(coda_detection_rule_new());
  ASSERT_NE(nullptr, rule.get());
  rule->product_definition = def.get();

  ScopedDetectionEntry entry = MakeEntry(nullptr, "filesize() > 0");
  ASSERT_NE(nullptr, entry.get());
  // coda_detection_rule_entry_new("") rejects an empty path via
  // coda_expression_from_string(), so populate entry->path with an allocated
  // empty C-string to cover get_node_for_entry()'s empty-subpath
  // normalization (coda-detection.c:277-280).
  char* empty_path = static_cast<char*>(std::malloc(1));
  ASSERT_NE(nullptr, empty_path);
  empty_path[0] = '\0';
  entry->path = empty_path;
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_rule_add_entry(rule.get(), entry.get()));
  entry.release();

  ScopedDetectionTree tree;
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), rule.get()));
  ASSERT_NE(nullptr, tree.get());
  ASSERT_EQ(1, tree.get()->num_subnodes);
  EXPECT_EQ(nullptr, tree.get()->subnode[0]->path);
  EXPECT_NE(nullptr, tree.get()->subnode[0]->expression);
  EXPECT_EQ(rule.get(), tree.get()->subnode[0]->rule);
}

TEST(CodaDetectionTest, EvaluateNullMismatchAndErrorPathsClearErrno) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  const absl::Cleanup clear_def = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile nc_file("eval_mismatch.nc");
  ASSERT_TRUE(nc_file.WriteBytes(
      BuildSampleNetcdf("OBS", "MAG1", "RAW", 2, 3.5, {10, 20})));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(nc_file.c_str(), product.OutParam()));
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  // 1. NULL detection tree clears *definition even if pre-populated.
  ScopedProductDef def_sentinel(
      coda_product_definition_new("DefSentinel", coda_format_netcdf, 1));
  ASSERT_NE(nullptr, def_sentinel.get());
  coda_product_definition* matched = def_sentinel.get();
  ASSERT_EQ(CODA_SUCCESS,
            coda_evaluate_detection_node(nullptr, &cursor, &matched));
  EXPECT_EQ(nullptr, matched);

  // 2. Non-existent path, failing expression evaluation (bad int parse,
  // missing node in bytes(), asciiline on NetCDF), and false boolean result all
  // fail to match while clearing coda_errno back to 0.
  ScopedProductDef def_missing(
      coda_product_definition_new("DefMissing", coda_format_netcdf, 1));
  ScopedProductDef def_bad_int(
      coda_product_definition_new("DefBadInt", coda_format_netcdf, 1));
  ScopedProductDef def_bad_bytes(
      coda_product_definition_new("DefBadBytes", coda_format_netcdf, 1));
  ScopedProductDef def_bad_asc(
      coda_product_definition_new("DefBadAsc", coda_format_netcdf, 1));
  ScopedProductDef def_false(
      coda_product_definition_new("DefFalse", coda_format_netcdf, 1));
  ASSERT_NE(nullptr, def_missing.get());
  ASSERT_NE(nullptr, def_bad_int.get());
  ASSERT_NE(nullptr, def_bad_bytes.get());
  ASSERT_NE(nullptr, def_bad_asc.get());
  ASSERT_NE(nullptr, def_false.get());

  ScopedDetectionRule r_missing =
      MakeRule(def_missing.get(), {{"/no_such_field", nullptr}});
  ScopedDetectionRule r_bad_int =
      MakeRule(def_bad_int.get(), {{nullptr, "int(/magic) == 1"}});
  ScopedDetectionRule r_bad_bytes =
      MakeRule(def_bad_bytes.get(), {{nullptr, "bytes(/missing) == \"X\""}});
  ScopedDetectionRule r_bad_asc =
      MakeRule(def_bad_asc.get(), {{nullptr, "str(asciiline[0]) == \"X\""}});
  ScopedDetectionRule r_false =
      MakeRule(def_false.get(), {{nullptr, "filesize() == 999999"}});
  ASSERT_NE(nullptr, r_missing.get());
  ASSERT_NE(nullptr, r_bad_int.get());
  ASSERT_NE(nullptr, r_bad_bytes.get());
  ASSERT_NE(nullptr, r_bad_asc.get());
  ASSERT_NE(nullptr, r_false.get());

  ScopedDetectionTree tree;
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), r_missing.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), r_bad_int.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), r_bad_bytes.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), r_bad_asc.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), r_false.get()));

  coda_set_error(CODA_ERROR_INVALID_ARGUMENT, "seed error");
  ASSERT_EQ(CODA_SUCCESS,
            coda_evaluate_detection_node(tree.get(), &cursor, &matched));
  EXPECT_EQ(nullptr, matched);
  EXPECT_EQ(CODA_SUCCESS, coda_errno);
}

TEST(CodaDetectionTest, EvaluateNetcdfEntriesPriorityAndBacktracking) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  const absl::Cleanup clear_def = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempDir dir("nc_detect");
  const std::filesystem::path p_deep = dir.child("obs_001.nc");
  const std::filesystem::path p_child = dir.child("obs_003.nc");
  const std::filesystem::path p_fallback = dir.child("obs_002.nc");
  const std::filesystem::path p_no_attr = dir.child("other.nc");

  ASSERT_TRUE(WriteFileBytes(
      p_deep, BuildSampleNetcdf("OBS", "MAG1", "RAW", 2, 3.5, {10, 20})));
  ASSERT_TRUE(WriteFileBytes(
      p_child,
      BuildSampleNetcdf("OBS", "MAG2", std::nullopt, 2, 3.5, {10, 20})));
  ASSERT_TRUE(WriteFileBytes(
      p_fallback,
      BuildSampleNetcdf("OBS", "MAG2", std::nullopt, 1, 0.5, {10, 99})));
  ASSERT_TRUE(WriteFileBytes(
      p_no_attr, BuildSampleNetcdf("", "MAG2", std::nullopt, 9, 0.25, {1, 2})));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Build detection tree exercising filename, filesize, @attr, /path@attr,
  // array element [1], int(), float(), str(), and parent backtracking.
  ScopedProductDef def_parent(
      coda_product_definition_new("NcParent", coda_format_netcdf, 1));
  ScopedProductDef def_child(
      coda_product_definition_new("NcChild", coda_format_netcdf, 2));
  ScopedProductDef def_deep(
      coda_product_definition_new("NcDeep", coda_format_netcdf, 3));
  ASSERT_NE(nullptr, def_parent.get());
  ASSERT_NE(nullptr, def_child.get());
  ASSERT_NE(nullptr, def_deep.get());

  ScopedDetectionRule r_parent =
      MakeRule(def_parent.get(),
               {{nullptr, "regex(\"^obs_[0-9]+\\\\.nc$\", filename())"},
                {nullptr, "filesize() > 100"}});
  ScopedDetectionRule r_deep = MakeRule(
      def_deep.get(), {{nullptr, "regex(\"^obs_[0-9]+\\\\.nc$\", filename())"},
                       {nullptr, "filesize() > 100"},
                       {"@tag", "str(.) == \"OBS\""},
                       {"/magic@kind", "str(.) == \"RAW\""},
                       {"/magic", "str(.) == \"MAG1\""}});
  ScopedDetectionRule r_child = MakeRule(
      def_child.get(), {{nullptr, "regex(\"^obs_[0-9]+\\\\.nc$\", filename())"},
                        {nullptr, "filesize() > 100"},
                        {"@tag", "str(.) == \"OBS\""},
                        {"/ver", "int(.) == 2"},
                        {"/scale", "float(.) > 1.5"},
                        {"/arr[1]", "int(.) == 20"}});
  ASSERT_NE(nullptr, r_parent.get());
  ASSERT_NE(nullptr, r_deep.get());
  ASSERT_NE(nullptr, r_child.get());

  // Add r_deep before r_child so /magic@kind is evaluated before /ver.
  ScopedDetectionTree tree;
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), r_parent.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), r_deep.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(tree.Addr(), r_child.get()));

  // obs_001.nc matches the deepest branch (NcDeep).
  {
    ScopedProduct prod;
    ASSERT_EQ(CODA_SUCCESS, coda_open(p_deep.c_str(), prod.OutParam()));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, prod.get()));
    coda_product_definition* matched = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_evaluate_detection_node(tree.get(), &cursor, &matched));
    EXPECT_EQ(def_deep.get(), matched);
  }

  // obs_003.nc lacks /magic@kind, falls back to sibling /ver, and matches
  // NcChild.
  {
    ScopedProduct prod;
    ASSERT_EQ(CODA_SUCCESS, coda_open(p_child.c_str(), prod.OutParam()));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, prod.get()));
    coda_product_definition* matched = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_evaluate_detection_node(tree.get(), &cursor, &matched));
    EXPECT_EQ(def_child.get(), matched);
  }

  // obs_002.nc enters @tag == "OBS", fails /magic@kind and /ver == 2, and
  // backtracks to NcParent.
  {
    ScopedProduct prod;
    ASSERT_EQ(CODA_SUCCESS, coda_open(p_fallback.c_str(), prod.OutParam()));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, prod.get()));
    coda_product_definition* matched = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_evaluate_detection_node(tree.get(), &cursor, &matched));
    EXPECT_EQ(def_parent.get(), matched);
  }

  // 2. Verify evaluation priority: @attr beats /field which beats expression,
  // regardless of rule insertion order.
  ScopedProductDef def_expr_first(
      coda_product_definition_new("PrioExpr", coda_format_netcdf, 1));
  ScopedProductDef def_field_second(
      coda_product_definition_new("PrioField", coda_format_netcdf, 1));
  ScopedProductDef def_attr_third(
      coda_product_definition_new("PrioAttr", coda_format_netcdf, 1));
  ASSERT_NE(nullptr, def_expr_first.get());
  ASSERT_NE(nullptr, def_field_second.get());
  ASSERT_NE(nullptr, def_attr_third.get());

  ScopedDetectionRule r_prio_expr =
      MakeRule(def_expr_first.get(), {{nullptr, "filesize() > 0"}});
  ScopedDetectionRule r_prio_field =
      MakeRule(def_field_second.get(), {{"/ver", nullptr}});
  ScopedDetectionRule r_prio_attr =
      MakeRule(def_attr_third.get(), {{"@tag", nullptr}});
  ASSERT_NE(nullptr, r_prio_expr.get());
  ASSERT_NE(nullptr, r_prio_field.get());
  ASSERT_NE(nullptr, r_prio_attr.get());

  ScopedDetectionTree prio_tree;
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(prio_tree.Addr(), r_prio_expr.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(prio_tree.Addr(), r_prio_field.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(prio_tree.Addr(), r_prio_attr.get()));

  {
    ScopedProduct prod;
    ASSERT_EQ(CODA_SUCCESS, coda_open(p_deep.c_str(), prod.OutParam()));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, prod.get()));
    coda_product_definition* matched = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_evaluate_detection_node(prio_tree.get(), &cursor, &matched));
    EXPECT_EQ(def_attr_third.get(), matched);
  }
  {
    ScopedProduct prod;
    ASSERT_EQ(CODA_SUCCESS, coda_open(p_no_attr.c_str(), prod.OutParam()));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, prod.get()));
    coda_product_definition* matched = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_evaluate_detection_node(prio_tree.get(), &cursor, &matched));
    EXPECT_EQ(def_field_second.get(), matched);
  }
}

TEST(CodaDetectionTest, CodadefRecognitionBacktrackingNestedAndAsciiLine) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  const absl::Cleanup clear_def = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempDir dir("codadef_detect");
  const std::filesystem::path codadef_path = dir.child("detect.codadef");
  ASSERT_TRUE(WriteFileBytes(codadef_path, BuildDetectionCodadefBytes()));

  const std::filesystem::path child_match = dir.child("child_01.bin");
  const std::filesystem::path child_wrong_data = dir.child("child_02.bin");
  const std::filesystem::path child_wrong_size = dir.child("child_03.bin");
  const std::filesystem::path other_name = dir.child("other_01.bin");
  const std::filesystem::path unknown_file = dir.child("unknown.bin");
  const std::filesystem::path nested_file_a = dir.child("nested_a.bin");
  const std::filesystem::path nested_file_b = dir.child("nested_b.bin");
  const std::filesystem::path nested_file_c = dir.child("nested_c.bin");
  const std::filesystem::path asc_file = dir.child("sample.asc");

  std::vector<uint8_t> spec_16 = EncodeAsciiBytes("BINHDR00SPEC0000");
  std::vector<uint8_t> othr_16 = EncodeAsciiBytes("BINHDR00OTHR0000");
  std::vector<uint8_t> spec_12 = EncodeAsciiBytes("BINHDR00SPEC");
  std::vector<uint8_t> unk_16 = EncodeAsciiBytes("UNKNOWN_MAGIC_00");
  ASSERT_TRUE(WriteFileBytes(child_match, spec_16));
  ASSERT_TRUE(WriteFileBytes(child_wrong_data, othr_16));
  ASSERT_TRUE(WriteFileBytes(child_wrong_size, spec_12));
  ASSERT_TRUE(WriteFileBytes(other_name, spec_16));
  ASSERT_TRUE(WriteFileBytes(unknown_file, unk_16));
  ASSERT_TRUE(
      WriteFileBytes(nested_file_a, BuildNestedBinProductBytes(10, 20, 1, 2)));
  ASSERT_TRUE(
      WriteFileBytes(nested_file_b, BuildNestedBinProductBytes(99, 20, 1, 2)));
  ASSERT_TRUE(
      WriteFileBytes(nested_file_c, BuildNestedBinProductBytes(99, 99, 1, 2)));
  ASSERT_TRUE(WriteFileText(asc_file, "ASCHDR line1\nsecond line\n"));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_path.c_str()));
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Exercise .codadef recognition across MatchData, MatchFilename,
  // MatchSize, MatchExpression, and backtracking to BaseBin.
  struct RecognizeCase {
    const std::filesystem::path* path;
    const char* expected_class;
    const char* expected_type;
    int expected_version;
  };
  const RecognizeCase kRecognizeCases[] = {
      {.path = &child_match,
       .expected_class = "DetectClass",
       .expected_type = "ChildBin",
       .expected_version = 2},
      {.path = &child_wrong_data,
       .expected_class = "DetectClass",
       .expected_type = "BaseBin",
       .expected_version = 1},
      {.path = &child_wrong_size,
       .expected_class = "DetectClass",
       .expected_type = "BaseBin",
       .expected_version = 1},
      {.path = &other_name,
       .expected_class = "DetectClass",
       .expected_type = "BaseBin",
       .expected_version = 1},
      {.path = &unknown_file,
       .expected_class = nullptr,
       .expected_type = nullptr,
       .expected_version = -1},
  };
  for (const RecognizeCase& tc : kRecognizeCases) {
    coda_format fmt = coda_format_ascii;
    const char* pclass = nullptr;
    const char* ptype = nullptr;
    int version = -999;
    ASSERT_EQ(CODA_SUCCESS, coda_recognize_file(tc.path->c_str(), nullptr, &fmt,
                                                &pclass, &ptype, &version))
        << tc.path->string();
    EXPECT_EQ(coda_format_binary, fmt);
    if (tc.expected_class == nullptr) {
      EXPECT_EQ(nullptr, pclass);
      EXPECT_EQ(nullptr, ptype);
    } else {
      EXPECT_STREQ(tc.expected_class, pclass);
      EXPECT_STREQ(tc.expected_type, ptype);
      EXPECT_EQ(tc.expected_version, version);
    }
  }

  // 2. Evaluate nested /hdr/a, /hdr/b, /arr[1] rules with sibling fallback and
  // parent backtracking on opened NestedBin products.
  ScopedProductDef def_hdr(
      coda_product_definition_new("HdrBase", coda_format_binary, 1));
  ScopedProductDef def_hdr_a(
      coda_product_definition_new("HdrA", coda_format_binary, 1));
  ScopedProductDef def_hdr_b(
      coda_product_definition_new("HdrB", coda_format_binary, 1));
  ScopedProductDef def_asc(
      coda_product_definition_new("AscRule", coda_format_ascii, 1));
  ASSERT_NE(nullptr, def_hdr.get());
  ASSERT_NE(nullptr, def_hdr_a.get());
  ASSERT_NE(nullptr, def_hdr_b.get());
  ASSERT_NE(nullptr, def_asc.get());

  ScopedDetectionRule r_hdr = MakeRule(def_hdr.get(), {{"/hdr", nullptr}});
  ScopedDetectionRule r_hdr_a = MakeRule(
      def_hdr_a.get(),
      {{"/hdr/a", "int(.) == 10"}, {"/magic", "bytes(.) == \"NESTED01\""}});
  ScopedDetectionRule r_hdr_b =
      MakeRule(def_hdr_b.get(),
               {{"/hdr/b", "int(.) == 20"}, {"/arr[1]", "int(.) == 2"}});
  // coda_cursor_goto() does not navigate virtual asciiline nodes, so
  // asciiline[i] must appear inside an expression-only entry (path == nullptr);
  // str(asciiline[i]) also retains the trailing '\n'
  // (ascii_text_line_with_eol).
  ScopedDetectionRule r_asc = MakeRule(
      def_asc.get(), {{nullptr, "bytes(asciiline[0], 0, 6) == \"ASCHDR\""},
                      {nullptr, "str(asciiline[1]) == \"second line\\n\""}});
  ASSERT_NE(nullptr, r_hdr.get());
  ASSERT_NE(nullptr, r_hdr_a.get());
  ASSERT_NE(nullptr, r_hdr_b.get());
  ASSERT_NE(nullptr, r_asc.get());

  ScopedDetectionTree bin_tree;
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(bin_tree.Addr(), r_hdr.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(bin_tree.Addr(), r_hdr_a.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(bin_tree.Addr(), r_hdr_b.get()));

  struct NestedEvalCase {
    const std::filesystem::path* path;
    coda_product_definition* expected;
  };
  const NestedEvalCase kNestedCases[] = {
      {.path = &nested_file_a, .expected = def_hdr_a.get()},
      {.path = &nested_file_b, .expected = def_hdr_b.get()},
      {.path = &nested_file_c, .expected = def_hdr.get()},
  };
  for (const NestedEvalCase& tc : kNestedCases) {
    ScopedProduct prod;
    ASSERT_EQ(CODA_SUCCESS, coda_open(tc.path->c_str(), prod.OutParam()));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, prod.get()));
    coda_product_definition* matched = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_evaluate_detection_node(bin_tree.get(), &cursor, &matched));
    EXPECT_EQ(tc.expected, matched);
  }

  // 3. Evaluate asciiline rule on an opened ASCII product.
  ScopedDetectionTree asc_tree;
  ASSERT_EQ(CODA_SUCCESS,
            coda_detection_tree_add_rule(asc_tree.Addr(), r_asc.get()));
  {
    ScopedProduct asc_prod;
    ASSERT_EQ(CODA_SUCCESS, coda_open(asc_file.c_str(), asc_prod.OutParam()));
    coda_cursor asc_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_set_product(&asc_cursor, asc_prod.get()));
    coda_product_definition* matched = nullptr;
    {
      // Upstream bug: coda_ascii_cursor_set_asciilines() (coda-ascii-cursor.c:
      // 611-625) omits coda_type_release(asciiline) after
      // coda_type_array_set_base_type(), leaving retain_count == 2 so
      // coda_ascii_close() leaks a 72-byte coda_type_text.
      absl::LeakCheckDisabler leak_disabler;
      ASSERT_EQ(CODA_SUCCESS, coda_evaluate_detection_node(
                                  asc_tree.get(), &asc_cursor, &matched));
    }
    EXPECT_EQ(def_asc.get(), matched);
  }
}

// TODO(schwehr): 88.7% reachable ceiling in libcoda/coda-detection.c (244/275);
// remaining 31 missed lines are 28 malloc/strdup OOM branches and 3 dead lines
// (328, 378, 493).

}  // namespace
}  // namespace stcorp_coda
