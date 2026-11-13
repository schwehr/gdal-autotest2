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
// Tests CODA node tree creation, path insertion, cursor lookup, and deletion
// (libcoda/coda-tree.c).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-tree.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-tree.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-type.h

#include <array>
#include <memory>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-tree.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

struct TypeReleaser {
  void operator()(coda_type* type) const { coda_type_release(type); }
};

using ScopedType = std::unique_ptr<coda_type, TypeReleaser>;

struct TreeNodeDeleter {
  void (*free_item)(void*) = nullptr;

  void operator()(coda_tree_node* node) const {
    if (node != nullptr) {
      coda_tree_node_delete(node, free_item);
    }
  }
};

using ScopedTreeNode = std::unique_ptr<coda_tree_node, TreeNodeDeleter>;

struct TrackedItem {
  int id;
  int* freed_count;
};

void FreeTrackedItem(void* ptr) {
  auto* item = static_cast<TrackedItem*>(ptr);
  if (item != nullptr) {
    ++(*item->freed_count);
    delete item;
  }
}

ScopedType MakeIntType(coda_format format = coda_format_binary) {
  coda_type_number* num = coda_type_number_new(format, coda_integer_class);
  if (num == nullptr) {
    return nullptr;
  }
  auto* base = reinterpret_cast<coda_type*>(num);
  if (coda_type_set_read_type(base, coda_native_type_int32) != 0 ||
      coda_type_set_bit_size(base, 32) != 0) {
    coda_type_release(base);
    return nullptr;
  }
  return ScopedType(base);
}

ScopedType MakeDoubleType(coda_format format = coda_format_binary) {
  coda_type_number* num = coda_type_number_new(format, coda_real_class);
  if (num == nullptr) {
    return nullptr;
  }
  auto* base = reinterpret_cast<coda_type*>(num);
  if (coda_type_set_read_type(base, coda_native_type_double) != 0 ||
      coda_type_set_bit_size(base, 64) != 0) {
    coda_type_release(base);
    return nullptr;
  }
  return ScopedType(base);
}

bool AddRecordField(coda_type_record* record, const char* name,
                    coda_type* field_type) {
  coda_type_record_field* field = coda_type_record_field_new(name);
  if (field == nullptr) {
    return false;
  }
  if (coda_type_record_field_set_type(field, field_type) != 0 ||
      coda_type_record_add_field(record, field) != 0) {
    coda_type_record_field_delete(field);
    return false;
  }
  return true;
}

bool AddAttribute(coda_type* target, const char* name, coda_type* attr_type) {
  coda_type_record_field* attr = coda_type_record_field_new(name);
  if (attr == nullptr) {
    return false;
  }
  if (coda_type_record_field_set_type(attr, attr_type) != 0 ||
      coda_type_add_attribute(target, attr) != 0) {
    coda_type_record_field_delete(attr);
    return false;
  }
  return true;
}

// Builds a fixed-size 1D array of int32 of length `length`.
ScopedType MakeIntArrayType(long length,
                            coda_format format = coda_format_binary) {
  ScopedType elem = MakeIntType(format);
  if (!elem) {
    return nullptr;
  }
  coda_type_array* arr = coda_type_array_new(format);
  if (arr == nullptr) {
    return nullptr;
  }
  if (coda_type_array_add_fixed_dimension(arr, length) != 0 ||
      coda_type_array_set_base_type(arr, elem.get()) != 0) {
    coda_type_release(reinterpret_cast<coda_type*>(arr));
    return nullptr;
  }
  return ScopedType(reinterpret_cast<coda_type*>(arr));
}

TEST(CodaTreeTest, NewAndDeleteEmptyAndPopulatedTrees) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedType int_type = MakeIntType();
  ASSERT_NE(nullptr, int_type);
  ScopedType arr_type = MakeIntArrayType(3);
  ASSERT_NE(nullptr, arr_type);
  ScopedType root_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, root_type);
  auto* root_rec = reinterpret_cast<coda_type_record*>(root_type.get());
  ASSERT_TRUE(AddRecordField(root_rec, "val", int_type.get()));
  ASSERT_TRUE(AddRecordField(root_rec, "arr", arr_type.get()));

  // 1. Verify initial fields of a newly allocated node and delete empty node.
  coda_tree_node* empty = coda_tree_node_new(root_type.get());
  ASSERT_NE(nullptr, empty);
  EXPECT_EQ(root_type.get(), empty->type);
  EXPECT_EQ(0, empty->num_items);
  EXPECT_EQ(nullptr, empty->item);
  EXPECT_EQ(nullptr, empty->all_children);
  EXPECT_EQ(0, empty->num_indexed_children);
  EXPECT_EQ(nullptr, empty->index);
  EXPECT_EQ(nullptr, empty->indexed_child);
  coda_tree_node_delete(empty, nullptr);

  // 2. Delete a populated tree with free_item == nullptr (leaves items intact).
  int stack_payload = 42;
  {
    ScopedTreeNode non_owning(coda_tree_node_new(root_type.get()));
    ASSERT_NE(nullptr, non_owning);
    ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                                non_owning.get(), "/val", &stack_payload, 1));
    ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                                non_owning.get(), "/arr[]", &stack_payload, 1));
    ASSERT_EQ(CODA_SUCCESS,
              coda_tree_node_add_item_for_path(non_owning.get(), "/arr[1]",
                                               &stack_payload, 1));
  }
  EXPECT_EQ(42, stack_payload);

  // 3. Delete a populated tree with a free_item callback, including a NULL
  // item slot to exercise the non-null guard in coda_tree_node_delete().
  int freed_count = 0;
  {
    ScopedTreeNode owning(coda_tree_node_new(root_type.get()),
                          TreeNodeDeleter{.free_item = FreeTrackedItem});
    ASSERT_NE(nullptr, owning);
    ASSERT_EQ(CODA_SUCCESS,
              coda_tree_node_add_item_for_path(
                  owning.get(), "/val", new TrackedItem{1, &freed_count}, 1));
    ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                                owning.get(), "/val", nullptr, 1));
    ASSERT_EQ(CODA_SUCCESS,
              coda_tree_node_add_item_for_path(
                  owning.get(), "/arr[]", new TrackedItem{2, &freed_count}, 1));
    ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                                owning.get(), "/arr[0]",
                                new TrackedItem{3, &freed_count}, 1));
  }
  EXPECT_EQ(3, freed_count);
}

TEST(CodaTreeTest, AddItemAtRootPathsAndLeafOnlyEnforcement) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Scalar root accepts empty and "/" paths even with leaf_only == 1, and
  // grows node->item on repeated insertions.
  ScopedType int_type = MakeIntType();
  ASSERT_NE(nullptr, int_type);
  ScopedTreeNode scalar_tree(coda_tree_node_new(int_type.get()));
  ASSERT_NE(nullptr, scalar_tree);

  int item1 = 10;
  int item2 = 20;
  int item3 = 30;
  ASSERT_EQ(CODA_SUCCESS,
            coda_tree_node_add_item_for_path(scalar_tree.get(), "", &item1, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(scalar_tree.get(),
                                                           "/", &item2, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(scalar_tree.get(),
                                                           "/", &item3, 1));
  ASSERT_EQ(3, scalar_tree->num_items);
  EXPECT_EQ(&item1, scalar_tree->item[0]);
  EXPECT_EQ(&item2, scalar_tree->item[1]);
  EXPECT_EQ(&item3, scalar_tree->item[2]);

  // 2. Record and array nodes reject leaf_only == 1, but succeed when
  // leaf_only == 0.
  ScopedType arr_type = MakeIntArrayType(2);
  ASSERT_NE(nullptr, arr_type);
  ScopedType sub_rec(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, sub_rec);
  ASSERT_TRUE(AddRecordField(reinterpret_cast<coda_type_record*>(sub_rec.get()),
                             "leaf", int_type.get()));

  ScopedType root_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, root_type);
  auto* root_rec = reinterpret_cast<coda_type_record*>(root_type.get());
  ASSERT_TRUE(AddRecordField(root_rec, "sub", sub_rec.get()));
  ASSERT_TRUE(AddRecordField(root_rec, "arr", arr_type.get()));

  ScopedTreeNode rec_tree(coda_tree_node_new(root_type.get()));
  ASSERT_NE(nullptr, rec_tree);

  const char* const kCompoundPaths[] = {"", "/", "/sub", "/arr"};
  for (const char* path : kCompoundPaths) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_tree_node_add_item_for_path(rec_tree.get(), path, &item1, 1))
        << "path=" << path;
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno) << "path=" << path;
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("not a leaf item"));

    EXPECT_EQ(CODA_SUCCESS,
              coda_tree_node_add_item_for_path(rec_tree.get(), path, &item2, 0))
        << "path=" << path;
  }
}

TEST(CodaTreeTest, LeadingSlashVariantsAttributesAndRepeatedLookups) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedType int_type = MakeIntType();
  ASSERT_NE(nullptr, int_type);
  ScopedType arr_type = MakeIntArrayType(4);
  ASSERT_NE(nullptr, arr_type);

  ScopedType sub_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, sub_type);
  ASSERT_TRUE(
      AddRecordField(reinterpret_cast<coda_type_record*>(sub_type.get()), "x",
                     int_type.get()));
  ASSERT_TRUE(AddAttribute(sub_type.get(), "tag", int_type.get()));

  ScopedType root_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, root_type);
  auto* root_rec = reinterpret_cast<coda_type_record*>(root_type.get());
  ASSERT_TRUE(AddRecordField(root_rec, "a", int_type.get()));
  ASSERT_TRUE(AddRecordField(root_rec, "sub", sub_type.get()));
  ASSERT_TRUE(AddRecordField(root_rec, "arr", arr_type.get()));
  ASSERT_TRUE(AddAttribute(root_type.get(), "meta", int_type.get()));

  // Also test a type with no attributes (attributes == NULL) so
  // coda_type_get_attributes() exercises the empty-record singleton path.
  ScopedType no_attr_rec(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, no_attr_rec);
  ScopedTreeNode no_attr_tree(coda_tree_node_new(no_attr_rec.get()));
  ASSERT_NE(nullptr, no_attr_tree);

  int item_meta = 1;
  int item_a = 2;
  int item_tag = 3;
  int item_x = 4;
  int item_all = 5;
  int item_idx = 6;

  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              no_attr_tree.get(), "/@", &item_meta, 0));

  ScopedTreeNode rec_tree(coda_tree_node_new(root_type.get()));
  ASSERT_NE(nullptr, rec_tree);

  // Leading "//" skips the first '/' and looks up field "a".
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(rec_tree.get(),
                                                           "//a", &item_a, 1));
  // Reusing an existing field child exercises the cache hit branch.
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_add_item_for_path(rec_tree.get(), "/a", &item_a, 1));

  // Leading "/@" skips '/' and enters the root attributes record.
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(rec_tree.get(), "/@",
                                                           &item_meta, 0));
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              rec_tree.get(), "/@/meta", &item_meta, 1));

  // Field followed by '@' and attribute field name (note coda-tree.c requires
  // '/' after '@').
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              rec_tree.get(), "/sub@/tag", &item_tag, 1));
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              rec_tree.get(), "/sub@/tag", &item_tag, 1));
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              rec_tree.get(), "/sub/x", &item_x, 1));

  // Repeating "/arr[]" hits the existing all_children path without
  // re-allocating.
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              rec_tree.get(), "/arr[]", &item_all, 1));
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              rec_tree.get(), "/arr[]", &item_all, 1));
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              rec_tree.get(), "/arr[2]", &item_idx, 1));

  // Array root exercises leading "/[" stripping.
  ScopedTreeNode arr_tree(coda_tree_node_new(arr_type.get()));
  ASSERT_NE(nullptr, arr_tree);
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              arr_tree.get(), "/[0]", &item_idx, 1));
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              arr_tree.get(), "/[]", &item_all, 1));
}

TEST(CodaTreeTest, IndexedChildrenBubbleSortAndBinarySearch) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedType int_type = MakeIntType();
  ASSERT_NE(nullptr, int_type);
  ScopedType root_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, root_type);
  auto* root_rec = reinterpret_cast<coda_type_record*>(root_type.get());

  constexpr std::array<const char*, 6> kFieldNames = {"f0", "f1", "f2",
                                                      "f3", "f4", "f5"};
  for (const char* name : kFieldNames) {
    ASSERT_TRUE(AddRecordField(root_rec, name, int_type.get()));
  }
  ASSERT_TRUE(AddAttribute(root_type.get(), "attr", int_type.get()));

  ScopedTreeNode tree(coda_tree_node_new(root_type.get()));
  ASSERT_NE(nullptr, tree);

  std::array<int, 7> payloads = {0, 1, 2, 3, 4, 5, 6};
  // Insert out of index order so bubble sort performs both swaps and
  // non-swaps, and binary search visits both <= and > branches.
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(tree.get(), "/f4",
                                                           &payloads[4], 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(tree.get(), "/f1",
                                                           &payloads[1], 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(tree.get(), "/f5",
                                                           &payloads[5], 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(tree.get(), "/f0",
                                                           &payloads[0], 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(tree.get(), "/f3",
                                                           &payloads[3], 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(tree.get(), "/f2",
                                                           &payloads[2], 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              tree.get(), "/@/attr", &payloads[6], 1));

  // Re-access lower, middle, and upper existing indices via binary search.
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(tree.get(), "/f0",
                                                           &payloads[0], 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(tree.get(), "/f2",
                                                           &payloads[2], 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(tree.get(), "/f5",
                                                           &payloads[5], 1));

  ASSERT_EQ(7, tree->num_indexed_children);
  for (int i = 0; i < 7; ++i) {
    EXPECT_EQ(static_cast<long>(i - 1), tree->index[i]);
  }
}

TEST(CodaTreeTest, SpecialTypesWrappingArrayAndRecord) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Special type (time) whose base_type is an array: both "/[0]" and "/[]"
  // unwrap coda_special_class to coda_array_class.
  ScopedType arr_type = MakeIntArrayType(3);
  ASSERT_NE(nullptr, arr_type);
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("1.0", expr.OutParam()));
  coda_type_special* raw_special =
      coda_type_time_new(coda_format_binary, expr.get());
  ASSERT_NE(nullptr, raw_special);
  expr.release();
  ScopedType special_arr(reinterpret_cast<coda_type*>(raw_special));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_time_set_base_type(
                reinterpret_cast<coda_type_special*>(special_arr.get()),
                arr_type.get()));

  ScopedTreeNode arr_tree(coda_tree_node_new(special_arr.get()));
  ASSERT_NE(nullptr, arr_tree);
  int idx_item = 11;
  int all_item = 22;
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              arr_tree.get(), "/[0]", &idx_item, 1));
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              arr_tree.get(), "/[]", &all_item, 1));
  ASSERT_EQ(1, arr_tree->num_indexed_children);
  ASSERT_NE(nullptr, arr_tree->all_children);

  // 2. Special type (complex) whose base_type is a record:
  // coda_tree_node_add_item_for_path() resolves type_class to record, but
  // passes node->type (still special) to
  // coda_type_get_record_field_index_from_name_n(), failing with
  // CODA_ERROR_INVALID_TYPE. Pin this upstream behavior.
  ScopedType dbl_type = MakeDoubleType();
  ASSERT_NE(nullptr, dbl_type);
  ScopedType complex_type(
      reinterpret_cast<coda_type*>(coda_type_complex_new(coda_format_binary)));
  ASSERT_NE(nullptr, complex_type);
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_complex_set_type(
                reinterpret_cast<coda_type_special*>(complex_type.get()),
                dbl_type.get()));

  ScopedTreeNode complex_tree(coda_tree_node_new(complex_type.get()));
  ASSERT_NE(nullptr, complex_tree);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_tree_node_add_item_for_path(complex_tree.get(), "/real",
                                                 &idx_item, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
}

TEST(CodaTreeTest, AddItemRejectsMalformedPathsAndTypeMismatches) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedType int_type = MakeIntType();
  ASSERT_NE(nullptr, int_type);
  ScopedType arr_type = MakeIntArrayType(2);
  ASSERT_NE(nullptr, arr_type);
  ScopedType root_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, root_type);
  auto* root_rec = reinterpret_cast<coda_type_record*>(root_type.get());
  ASSERT_TRUE(AddRecordField(root_rec, "scalar", int_type.get()));
  ASSERT_TRUE(AddRecordField(root_rec, "arr", arr_type.get()));
  ASSERT_TRUE(AddAttribute(root_type.get(), "meta", int_type.get()));

  ScopedTreeNode tree(coda_tree_node_new(root_type.get()));
  ASSERT_NE(nullptr, tree);
  int dummy = 7;

  struct InvalidPathCase {
    const char* path;
    int expected_errno;
    std::string_view expected_substr;
  };
  constexpr InvalidPathCase kCases[] = {
      {.path = "scalar",
       .expected_errno = CODA_ERROR_INVALID_ARGUMENT,
       .expected_substr = "missing '/'"},
      {.path = "/@meta",
       .expected_errno = CODA_ERROR_INVALID_ARGUMENT,
       .expected_substr = "missing '/'"},
      {.path = "/arr[0]extra",
       .expected_errno = CODA_ERROR_INVALID_ARGUMENT,
       .expected_substr = "missing '/'"},
      {.path = "/arr[0",
       .expected_errno = CODA_ERROR_INVALID_ARGUMENT,
       .expected_substr = "missing ']'"},
      {.path = "/arr[abc]",
       .expected_errno = CODA_ERROR_INVALID_ARGUMENT,
       .expected_substr = "invalid array index 'abc'"},
      {.path = "/arr[1x]",
       .expected_errno = CODA_ERROR_INVALID_ARGUMENT,
       .expected_substr = "invalid array index '1x'"},
      // Indexing a non-array scalar sets "not an array", falls through to
      // tree_node_get_node_for_index() -> coda_type_get_record_field_type(),
      // and fails with CODA_ERROR_INVALID_TYPE.
      {.path = "/scalar[0]",
       .expected_errno = CODA_ERROR_INVALID_TYPE,
       .expected_substr = "does not refer to a record"},
      // Field traversal into non-records sets "not a record", falls through
      // to coda_type_get_record_field_index_from_name_n(), and fails with
      // CODA_ERROR_INVALID_TYPE.
      {.path = "/scalar/foo",
       .expected_errno = CODA_ERROR_INVALID_TYPE,
       .expected_substr = "does not refer to a record"},
      {.path = "/arr/foo",
       .expected_errno = CODA_ERROR_INVALID_TYPE,
       .expected_substr = "does not refer to a record"},
      {.path = "/nonexistent",
       .expected_errno = CODA_ERROR_INVALID_NAME,
       .expected_substr = "nonexistent"},
  };

  for (const InvalidPathCase& tc : kCases) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_tree_node_add_item_for_path(tree.get(), tc.path, &dummy, 1))
        << "path=" << tc.path;
    EXPECT_EQ(tc.expected_errno, coda_errno) << "path=" << tc.path;
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(tc.expected_substr))
        << "path=" << tc.path;
  }

  // Attributes lookup on a tree node with a NULL type triggers
  // coda_type_get_attributes() failure in tree_node_get_node_for_index().
  ScopedTreeNode null_type_tree(coda_tree_node_new(nullptr));
  ASSERT_NE(nullptr, null_type_tree);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_tree_node_add_item_for_path(null_type_tree.get(), "/@",
                                                 &dummy, 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

TEST(CodaTreeTest, GetItemForCursorWithXmlProductAndPrecedence) {
  TempFile xml_file("coda_tree_product.xml");
  ASSERT_TRUE(
      xml_file.WriteText("<?xml version=\"1.0\"?>\n"
                         "<root attr=\"root_v\">\n"
                         "  <scalar>42</scalar>\n"
                         "  <items><x>1</x><y>2</y></items>\n"
                         "  <items><x>10</x><y>20</y></items>\n"
                         "  <items><x>100</x><y>200</y></items>\n"
                         "  <items><x>1000</x><y>2000</y></items>\n"
                         "  <empty_branch><z>0</z></empty_branch>\n"
                         "</root>\n"));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_type* root_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_root_type(product.get(), &root_type));
  ASSERT_NE(nullptr, root_type);

  ScopedTreeNode tree(coda_tree_node_new(root_type));
  ASSERT_NE(nullptr, tree);

  int root_item = 100;
  int attr_item = 101;
  int scalar_item1 = 102;
  int scalar_item2 = 103;
  int all_x = 104;
  int all_y = 105;
  int idx0_x = 106;
  int idx2_x = 107;
  int idx3_x = 108;

  ASSERT_EQ(CODA_SUCCESS,
            coda_tree_node_add_item_for_path(tree.get(), "/", &root_item, 0));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              tree.get(), "/root@/attr", &attr_item, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              tree.get(), "/root/scalar", &scalar_item1, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              tree.get(), "/root/scalar", &scalar_item2, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              tree.get(), "/root/items[]/x", &all_x, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              tree.get(), "/root/items[]/y", &all_y, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              tree.get(), "/root/items[0]/x", &idx0_x, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              tree.get(), "/root/items[2]/x", &idx2_x, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_tree_node_add_item_for_path(
                              tree.get(), "/root/items[3]/x", &idx3_x, 1));

  coda_cursor cursor;
  void* found = &root_item;

  // 1. NULL tree node sets *item to NULL and returns 0.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(nullptr, &cursor, &found));
  EXPECT_EQ(nullptr, found);

  // 2. Root cursor matches the item attached to "/".
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(&root_item, found);

  // 3. Attribute cursor matches "/root@/attr".
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root@attr"));
  found = nullptr;
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(&attr_item, found);

  // 4. Multiple items on the same node return the last added item.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/scalar"));
  found = nullptr;
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(&scalar_item2, found);

  // 5. Explicit indexed array paths take precedence over "[]" across lower,
  // middle, and upper indices, while unindexed element [1] falls back to "[]".
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/items[0]/x"));
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(&idx0_x, found);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/items[2]/x"));
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(&idx2_x, found);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/items[3]/x"));
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(&idx3_x, found);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/items[1]/x"));
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(&all_x, found);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/items[1]/y"));
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(&all_y, found);

  // 6. Upstream quirk in get_item_for_cursor(): when an indexed child node
  // exists (items[2]), it checks `item != NULL` instead of `*item != NULL`,
  // returning early with *item == NULL instead of falling back to all_children
  // for sibling fields like /y.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/items[2]/y"));
  found = &all_y;
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(nullptr, found);

  // 7. Intermediate node without items and unpopulated subtrees return NULL.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/items"));
  found = &root_item;
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(nullptr, found);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/empty_branch/z"));
  found = &root_item;
  EXPECT_EQ(CODA_SUCCESS,
            coda_tree_node_get_item_for_cursor(tree.get(), &cursor, &found));
  EXPECT_EQ(nullptr, found);

  // 8. Empty tree node with no items or children returns NULL for root cursor.
  ScopedTreeNode empty_tree(coda_tree_node_new(root_type));
  ASSERT_NE(nullptr, empty_tree);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  found = &root_item;
  EXPECT_EQ(CODA_SUCCESS, coda_tree_node_get_item_for_cursor(empty_tree.get(),
                                                             &cursor, &found));
  EXPECT_EQ(nullptr, found);
}

// TODO(schwehr): 90.7% line coverage in libcoda/coda-tree.c (244/269);
// remaining uncovered lines are malloc/realloc OOM
// branches, create == 0 dead code in tree_node_get_node_for_index(), and
// defensive error-propagation checks after validated type-class guards.

}  // namespace
}  // namespace stcorp_coda
