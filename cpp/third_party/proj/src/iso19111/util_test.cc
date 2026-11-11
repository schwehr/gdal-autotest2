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
// Unit tests for ISO-19111/19103 utility classes (BaseObject, BoxedValue,
// ArrayOfBaseObject, PropertyMap, GenericName, NameSpace, LocalName,
// NameFactory, CodeList, Exception hierarchy, and IComparable) in
// proj/src/iso19111/util.cpp. Note: in google3 default non-GCE builds, proj.db
// is embedded in memory and networking is disabled. Tests calling PROJ_INTERNAL
// C++ methods require -fexceptions and linkstatic = True.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/util.cpp

#include "third_party/proj4/proj/include/proj/util.hpp"

#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

using ::osgeo::proj::io::DatabaseContext;
using ::osgeo::proj::io::DatabaseContextPtr;
using ::osgeo::proj::util::ArrayOfBaseObject;
using ::osgeo::proj::util::BaseObject;
using ::osgeo::proj::util::BaseObjectNNPtr;
using ::osgeo::proj::util::BoxedValue;
using ::osgeo::proj::util::CodeList;
using ::osgeo::proj::util::Exception;
using ::osgeo::proj::util::GenericName;
using ::osgeo::proj::util::GenericNameNNPtr;
using ::osgeo::proj::util::IComparable;
using ::osgeo::proj::util::InvalidValueTypeException;
using ::osgeo::proj::util::LocalName;
using ::osgeo::proj::util::NameFactory;
using ::osgeo::proj::util::NameSpace;
using ::osgeo::proj::util::NameSpacePtr;
using ::osgeo::proj::util::nn;
using ::osgeo::proj::util::nn_make_shared;
using ::osgeo::proj::util::optional;
using ::osgeo::proj::util::PropertyMap;
using ::osgeo::proj::util::UnsupportedOperationException;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::NotNull;

class TestBaseObject final : public BaseObject {
 public:
  TestBaseObject() = default;
  TestBaseObject(TestBaseObject&&) = delete;

  TestBaseObject& operator=(TestBaseObject&& other) {
    BaseObject::operator=(std::move(other));
    return *this;
  }

  static nn<std::shared_ptr<TestBaseObject>> CreateWithSelf() {
    auto obj = nn_make_shared<TestBaseObject>();
    obj->assignSelf(obj);
    return obj;
  }

  BaseObjectNNPtr SharedFromThis() const { return shared_from_this(); }

  void ResetSelfViaMoveAssign() {
    TestBaseObject source;
    EXPECT_EQ(this, &(*this = std::move(source)));
  }
};

class TestLocalName final : public LocalName {
 public:
  explicit TestLocalName(const std::string& name) : LocalName(name) {}
  TestLocalName(const NameSpacePtr& ns, const std::string& name)
      : LocalName(ns, name) {}
  TestLocalName(const TestLocalName& other) = default;
};

class TestCodeList final : public CodeList {
 public:
  explicit TestCodeList(const std::string& name) : CodeList(name) {}
  TestCodeList(const TestCodeList&) = default;
  TestCodeList& operator=(const TestCodeList& other) {
    CodeList::operator=(other);
    return *this;
  }
};

class TestComparable final : public IComparable {
 public:
  explicit TestComparable(int value) : value_(value) {}

  int call_count() const { return call_count_; }
  Criterion last_criterion() const { return last_criterion_; }
  bool last_had_db_context() const { return last_had_db_context_; }

  bool _isEquivalentTo(const IComparable* other, Criterion criterion,
                       const DatabaseContextPtr& db_context) const override {
    ++call_count_;
    last_criterion_ = criterion;
    last_had_db_context_ = (db_context != nullptr);
    const auto* other_tc = dynamic_cast<const TestComparable*>(other);
    if (other_tc == nullptr) {
      return false;
    }
    if (criterion == Criterion::STRICT) {
      return value_ == other_tc->value_;
    }
    return (value_ % 10) == (other_tc->value_ % 10);
  }

 private:
  int value_;
  mutable int call_count_ = 0;
  mutable Criterion last_criterion_ = Criterion::STRICT;
  mutable bool last_had_db_context_ = false;
};

TEST(UtilTest, BaseObjectAssignSelfSharedFromThisAndMoveAssign) {
  // 1. Construct via nn_make_shared, bind weak self_, and retrieve via
  //    shared_from_this() + BaseObjectNNPtr destructor.
  auto obj = TestBaseObject::CreateWithSelf();
  {
    const BaseObjectNNPtr self = obj->SharedFromThis();
    EXPECT_EQ(obj.get(), self.get());
  }

  // 2. Move assignment clears internal self_ weak_ptr cleanly.
  obj->ResetSelfViaMoveAssign();

  // 3. Polymorphic deletion via BaseObject* exercises deleting destructor.
  std::unique_ptr<BaseObject> heap_base = std::make_unique<TestBaseObject>();
  EXPECT_THAT(heap_base.get(), NotNull());
  heap_base.reset();
}

TEST(UtilTest, BoxedValueConstructorsCopyAndAccessors) {
  // Null const char* ternary branch yields an empty STRING BoxedValue.
  const BoxedValue from_null(static_cast<const char*>(nullptr));
  EXPECT_EQ(BoxedValue::Type::STRING, from_null.type());
  EXPECT_EQ("", from_null.stringValue());

  const BoxedValue from_cstr("EPSG:4326");
  EXPECT_EQ(BoxedValue::Type::STRING, from_cstr.type());
  EXPECT_EQ("EPSG:4326", from_cstr.stringValue());

  const BoxedValue from_str(std::string("WGS 84"));
  EXPECT_EQ(BoxedValue::Type::STRING, from_str.type());
  EXPECT_EQ("WGS 84", from_str.stringValue());

  const BoxedValue from_int(-42);
  EXPECT_EQ(BoxedValue::Type::INTEGER, from_int.type());
  EXPECT_EQ(-42, from_int.integerValue());

  const BoxedValue from_true(true);
  EXPECT_EQ(BoxedValue::Type::BOOLEAN, from_true.type());
  EXPECT_TRUE(from_true.booleanValue());

  const BoxedValue from_false(false);
  EXPECT_EQ(BoxedValue::Type::BOOLEAN, from_false.type());
  EXPECT_FALSE(from_false.booleanValue());

  // Copy constructor preserves type and payload across all variants.
  const BoxedValue copy_str(from_str);
  EXPECT_EQ(BoxedValue::Type::STRING, copy_str.type());
  EXPECT_EQ("WGS 84", copy_str.stringValue());

  const BoxedValue copy_int(from_int);
  EXPECT_EQ(BoxedValue::Type::INTEGER, copy_int.type());
  EXPECT_EQ(-42, copy_int.integerValue());

  const BoxedValue copy_bool(from_true);
  EXPECT_EQ(BoxedValue::Type::BOOLEAN, copy_bool.type());
  EXPECT_TRUE(copy_bool.booleanValue());

  std::unique_ptr<BaseObject> heap_boxed = std::make_unique<BoxedValue>("heap");
  EXPECT_THAT(heap_boxed.get(), NotNull());
  heap_boxed.reset();
}

TEST(UtilTest, ArrayOfBaseObjectCreateAddAndIterate) {
  auto arr = ArrayOfBaseObject::create();
  EXPECT_TRUE(arr->empty());
  EXPECT_EQ(arr->begin(), arr->end());

  arr->add(nn_make_shared<BoxedValue>("first"));
  arr->add(nn_make_shared<BoxedValue>(99));
  arr->add(NameFactory::createLocalName(nullptr, "third"));

  EXPECT_FALSE(arr->empty());
  std::vector<const BaseObject*> seen;
  for (const auto& item : *arr) {
    seen.push_back(item.get());
  }
  ASSERT_EQ(3u, seen.size());

  const auto* first = dynamic_cast<const BoxedValue*>(seen[0]);
  ASSERT_THAT(first, NotNull());
  EXPECT_EQ("first", first->stringValue());

  const auto* second = dynamic_cast<const BoxedValue*>(seen[1]);
  ASSERT_THAT(second, NotNull());
  EXPECT_EQ(99, second->integerValue());

  const auto* third = dynamic_cast<const GenericName*>(seen[2]);
  ASSERT_THAT(third, NotNull());
  EXPECT_EQ("third", third->toString());
}

TEST(UtilTest, PropertyMapSetOverwriteCopyUnsetAndGetStringValue) {
  PropertyMap map;
  EXPECT_EQ(nullptr, map.get("missing"));
  map.unset("missing");

  std::string out_str = "preserved";
  optional<std::string> out_opt("preserved_opt");
  EXPECT_FALSE(map.getStringValue("missing", out_str));
  EXPECT_EQ("preserved", out_str);
  EXPECT_FALSE(map.getStringValue("missing", out_opt));
  ASSERT_TRUE(out_opt.has_value());
  EXPECT_EQ("preserved_opt", *out_opt);

  // 1. Insert and overwrite via every PropertyMap::set overload.
  map.set("cstr", "initial");
  map.set("cstr", "updated");
  map.set("null_cstr", static_cast<const char*>(nullptr));
  map.set("str", std::string("alpha"));
  map.set("str", std::string("beta"));
  map.set("int_val", 123);
  map.set("bool_val", true);
  map.set("empty_vec", std::vector<std::string>{});
  map.set("str_vec", std::vector<std::string>{"one", "two"});

  const BaseObjectNNPtr obj1 = NameFactory::createLocalName(nullptr, "obj1");
  const BaseObjectNNPtr obj2 = NameFactory::createLocalName(nullptr, "obj2");
  map.set("obj", obj1);
  map.set("obj", obj2);
  map.set("typed", NameFactory::createLocalName(nullptr, "typed_name"));

  // 2. Verify get() hits and misses across a populated list.
  ASSERT_THAT(map.get("obj"), NotNull());
  EXPECT_EQ(obj2.get(), map.get("obj")->get());
  EXPECT_EQ(nullptr, map.get("not_in_map"));

  const auto* empty_vec_entry = map.get("empty_vec");
  ASSERT_THAT(empty_vec_entry, NotNull());
  const auto* empty_arr =
      dynamic_cast<const ArrayOfBaseObject*>(empty_vec_entry->get());
  ASSERT_THAT(empty_arr, NotNull());
  EXPECT_TRUE(empty_arr->empty());
  EXPECT_EQ(empty_arr->begin(), empty_arr->end());

  const auto* str_vec_entry = map.get("str_vec");
  ASSERT_THAT(str_vec_entry, NotNull());
  const auto* str_arr =
      dynamic_cast<const ArrayOfBaseObject*>(str_vec_entry->get());
  ASSERT_THAT(str_arr, NotNull());
  std::vector<std::string> str_arr_vals;
  for (const auto& item : *str_arr) {
    const auto* boxed = dynamic_cast<const BoxedValue*>(item.get());
    ASSERT_THAT(boxed, NotNull());
    str_arr_vals.push_back(boxed->stringValue());
  }
  EXPECT_THAT(str_arr_vals, ElementsAre("one", "two"));

  const auto* typed_entry = map.get("typed");
  ASSERT_THAT(typed_entry, NotNull());
  const auto* typed_name = dynamic_cast<const GenericName*>(typed_entry->get());
  ASSERT_THAT(typed_name, NotNull());
  EXPECT_EQ("typed_name", typed_name->toString());
  EXPECT_TRUE(typed_name->scope()->isGlobal());

  // 3. Verify valid getStringValue() overloads.
  EXPECT_TRUE(map.getStringValue("cstr", out_str));
  EXPECT_EQ("updated", out_str);
  EXPECT_TRUE(map.getStringValue("null_cstr", out_str));
  EXPECT_EQ("", out_str);
  EXPECT_TRUE(map.getStringValue("str", out_opt));
  ASSERT_TRUE(out_opt.has_value());
  EXPECT_EQ("beta", *out_opt);

  EXPECT_FALSE(map.getStringValue("absent", out_str));
  EXPECT_FALSE(map.getStringValue("absent", out_opt));

  // 4. Verify InvalidValueTypeException for wrong BoxedValue and non-BoxedValue
  //    entries across both getStringValue overloads.
  constexpr std::string_view kNonStringKeys[] = {
      "int_val",
      "bool_val",
      "str_vec",
      "obj",
  };
  for (const std::string_view key_sv : kNonStringKeys) {
    const std::string key(key_sv);
    EXPECT_THROW(map.getStringValue(key, out_str), InvalidValueTypeException)
        << key;
    EXPECT_THROW(map.getStringValue(key, out_opt), InvalidValueTypeException)
        << key;
  }

  // 5. Verify copy constructor deep-copies entries independently of unset().
  const PropertyMap copied(map);
  map.unset("cstr");
  map.unset("obj");
  map.unset("still_missing");
  EXPECT_EQ(nullptr, map.get("cstr"));
  EXPECT_EQ(nullptr, map.get("obj"));
  EXPECT_FALSE(map.getStringValue("cstr", out_str));

  EXPECT_TRUE(copied.getStringValue("cstr", out_str));
  EXPECT_EQ("updated", out_str);
  ASSERT_THAT(copied.get("obj"), NotNull());
  EXPECT_EQ(obj2.get(), copied.get("obj")->get());
}

TEST(UtilTest, NameSpaceLocalNameGenericNameAndNameFactory) {
  // 1. Direct LocalName(name) has null internal scope and falls back to GLOBAL.
  const TestLocalName bare_local("bare");
  EXPECT_EQ("bare", bare_local.toString());
  ASSERT_THAT(bare_local.scope().get(), NotNull());
  EXPECT_TRUE(bare_local.scope()->isGlobal());

  // 2. LocalName(nullptr, name) and copy constructor + polymorphic delete.
  const TestLocalName null_ns_local(nullptr, "from_null_ns");
  EXPECT_TRUE(null_ns_local.scope()->isGlobal());
  const TestLocalName copied_local(null_ns_local);
  EXPECT_EQ("from_null_ns", copied_local.toString());

  auto heap_name =
      std::make_unique<TestLocalName>(nullptr, "heap_generic_name");
  std::unique_ptr<GenericName> base_name = std::move(heap_name);
  EXPECT_EQ("heap_generic_name", base_name->toString());
  base_name.reset();

  // 3. NameFactory::createLocalName with global scope short-circuits FQN.
  const auto global_name = NameFactory::createLocalName(nullptr, "WGS 84");
  EXPECT_TRUE(global_name->scope()->isGlobal());
  EXPECT_EQ("WGS 84", global_name->toString());
  const GenericNameNNPtr global_fqn = global_name->toFullyQualifiedName();
  EXPECT_TRUE(global_fqn->scope()->isGlobal());
  EXPECT_EQ("WGS 84", global_fqn->toString());

  // 4. NameFactory::createNameSpace with default and custom separators, plus
  //    invalid property types.
  const auto epsg_label = NameFactory::createLocalName(nullptr, "EPSG");
  const auto epsg_ns = NameFactory::createNameSpace(epsg_label, PropertyMap());
  EXPECT_FALSE(epsg_ns->isGlobal());
  ASSERT_THAT(epsg_ns->name().get(), NotNull());
  EXPECT_EQ("EPSG", epsg_ns->name()->toString());

  const auto custom_ns = NameFactory::createNameSpace(
      NameFactory::createLocalName(nullptr, "URN"),
      PropertyMap().set("separator", ".").set("separator.head", "/"));
  EXPECT_FALSE(custom_ns->isGlobal());

  EXPECT_THROW(NameFactory::createNameSpace(epsg_label,
                                            PropertyMap().set("separator", 42)),
               InvalidValueTypeException);
  EXPECT_THROW(NameFactory::createNameSpace(
                   epsg_label, PropertyMap().set("separator.head", true)),
               InvalidValueTypeException);

  // 5. LocalName in non-global NameSpace expands recursively and attaches a
  //    global wrapper namespace whose own name("global") has null internal
  //    scope.
  const auto scoped_code = NameFactory::createLocalName(epsg_ns, "4326");
  EXPECT_EQ(epsg_ns.get(), scoped_code->scope().get());
  EXPECT_EQ("4326", scoped_code->toString());

  const GenericNameNNPtr fqn = scoped_code->toFullyQualifiedName();
  EXPECT_EQ("EPSG:4326", fqn->toString());
  EXPECT_TRUE(fqn->scope()->isGlobal());
  ASSERT_THAT(fqn->scope()->name().get(), NotNull());
  EXPECT_EQ("global", fqn->scope()->name()->toString());
  EXPECT_TRUE(fqn->scope()->name()->scope()->isGlobal());

  // Re-qualifying an already-global FQN takes the isGlobal() fast path.
  EXPECT_EQ("EPSG:4326", fqn->toFullyQualifiedName()->toString());

  // Multi-level nested namespaces combine each scope's separator.
  const auto inner_ns =
      NameFactory::createNameSpace(NameFactory::createLocalName(epsg_ns, "CRS"),
                                   PropertyMap().set("separator", "."));
  const auto nested_name = NameFactory::createLocalName(inner_ns, "84");
  EXPECT_EQ("EPSG:CRS.84", nested_name->toFullyQualifiedName()->toString());

  // 6. NameFactory::createGenericName with global/custom scope and 0/1/N parts.
  EXPECT_EQ("", NameFactory::createGenericName(nullptr, {})->toString());
  EXPECT_EQ("solo",
            NameFactory::createGenericName(nullptr, {"solo"})->toString());
  EXPECT_EQ(
      "EPSG:4326",
      NameFactory::createGenericName(nullptr, {"EPSG", "4326"})->toString());
  const auto dot_gen =
      NameFactory::createGenericName(custom_ns, {"ogc", "def", "crs"});
  EXPECT_EQ(custom_ns.get(), dot_gen->scope().get());
  EXPECT_EQ("ogc.def.crs", dot_gen->toString());
  EXPECT_EQ("URN.ogc.def.crs", dot_gen->toFullyQualifiedName()->toString());
}

TEST(UtilTest, CodeListCopyAssignComparisonAndStringConversion) {
  const TestCodeList alpha("alpha");
  TestCodeList beta("beta");
  const TestCodeList alpha_copy(alpha);

  EXPECT_EQ("alpha", alpha.toString());
  EXPECT_EQ("alpha", static_cast<std::string>(alpha));
  EXPECT_TRUE(alpha == alpha_copy);
  EXPECT_FALSE(alpha != alpha_copy);
  EXPECT_TRUE(alpha != beta);
  EXPECT_FALSE(alpha == beta);

  beta = alpha;
  EXPECT_TRUE(alpha == beta);
  EXPECT_EQ("alpha", beta.toString());
}

TEST(UtilTest, ExceptionsConstructorsCopyWhatAndHierarchy) {
  // 1. Base Exception: const char*, std::string, copy ctor, heap delete, throw.
  const Exception exc_cstr("base cstr error");
  const Exception exc_str(std::string("base string error"));
  const Exception exc_copy(exc_str);
  EXPECT_STREQ("base cstr error", exc_cstr.what());
  EXPECT_STREQ("base string error", exc_str.what());
  EXPECT_STREQ("base string error", exc_copy.what());

  std::unique_ptr<std::exception> heap_exc =
      std::make_unique<Exception>("heap base error");
  EXPECT_STREQ("heap base error", heap_exc->what());
  heap_exc.reset();

  try {
    throw Exception("thrown base");
  } catch (const std::exception& caught) {
    EXPECT_STREQ("thrown base", caught.what());
  }

  // 2. InvalidValueTypeException: both ctors, copy ctor, heap delete, throw.
  const InvalidValueTypeException inv_cstr("invalid cstr");
  const InvalidValueTypeException inv_str(std::string("invalid string"));
  const InvalidValueTypeException inv_copy(inv_str);
  EXPECT_STREQ("invalid cstr", inv_cstr.what());
  EXPECT_STREQ("invalid string", inv_copy.what());

  std::unique_ptr<Exception> heap_inv =
      std::make_unique<InvalidValueTypeException>("heap invalid");
  EXPECT_STREQ("heap invalid", heap_inv->what());
  heap_inv.reset();

  try {
    throw InvalidValueTypeException(std::string("thrown invalid"));
  } catch (const Exception& caught) {
    EXPECT_THAT(caught.what(), HasSubstr("thrown invalid"));
  }

  // 3. UnsupportedOperationException: both ctors, copy ctor, heap delete,
  //    throw.
  const UnsupportedOperationException unsup_cstr("unsupported cstr");
  const UnsupportedOperationException unsup_str(
      std::string("unsupported string"));
  const UnsupportedOperationException unsup_copy(unsup_str);
  EXPECT_STREQ("unsupported cstr", unsup_cstr.what());
  EXPECT_STREQ("unsupported string", unsup_copy.what());

  std::unique_ptr<Exception> heap_unsup =
      std::make_unique<UnsupportedOperationException>("heap unsupported");
  EXPECT_STREQ("heap unsupported", heap_unsup->what());
  heap_unsup.reset();

  try {
    throw UnsupportedOperationException("thrown unsupported");
  } catch (const Exception& caught) {
    EXPECT_STREQ("thrown unsupported", caught.what());
  }
}

TEST(UtilTest, IComparableIsEquivalentToIdentityAndDelegation) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  const DatabaseContextPtr db_ctx =
      DatabaseContext::create(std::string(), {}, ctx.get()).as_nullable();
  ASSERT_THAT(db_ctx.get(), NotNull());

  const TestComparable a(10);
  const TestComparable b(20);
  const TestComparable c(21);

  // 1. Self-comparison short-circuits before calling _isEquivalentTo.
  EXPECT_TRUE(a.isEquivalentTo(&a));
  EXPECT_EQ(0, a.call_count());

  // 2. Nullptr and distinct objects delegate to _isEquivalentTo.
  EXPECT_FALSE(a.isEquivalentTo(nullptr));
  EXPECT_EQ(1, a.call_count());
  EXPECT_EQ(IComparable::Criterion::STRICT, a.last_criterion());
  EXPECT_FALSE(a.last_had_db_context());

  EXPECT_FALSE(a.isEquivalentTo(&b, IComparable::Criterion::STRICT));
  EXPECT_TRUE(a.isEquivalentTo(&b, IComparable::Criterion::EQUIVALENT, db_ctx));
  EXPECT_EQ(IComparable::Criterion::EQUIVALENT, a.last_criterion());
  EXPECT_TRUE(a.last_had_db_context());

  EXPECT_FALSE(a.isEquivalentTo(
      &c, IComparable::Criterion::EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS));
  EXPECT_EQ(IComparable::Criterion::EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS,
            a.last_criterion());
  EXPECT_FALSE(a.last_had_db_context());

  // 3. Polymorphic deletion via IComparable*.
  std::unique_ptr<IComparable> heap_comp = std::make_unique<TestComparable>(5);
  EXPECT_TRUE(heap_comp->isEquivalentTo(heap_comp.get()));
  heap_comp.reset();
}

// Note: BoxedValue::BoxedValue() (util.cpp:136) is declared private in util.hpp
// without friend declarations or internal callers, so it cannot be covered
// outside BoxedValue itself.

}  // namespace
}  // namespace proj
