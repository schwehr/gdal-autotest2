// Tests public functions in cpl_list:
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_list.h
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_list.cpp
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

#include "port/cpl_list.h"

#include "gunit.h"
#include "gcore/gdal_priv.h"

namespace {

// Tests creating and destroying a list with a single nullptr element.
TEST(CplListTest, SingleNullptrElement) {
  // Create a list with one element and no data.
  CPLList* list = CPLListAppend(nullptr, nullptr);

  ASSERT_NE(nullptr, list);
  ASSERT_EQ(1, CPLListCount(list));

  // These checks return an element in the list, but that element is
  // the start element of the list.  Therefore, the pointer to the list
  // is the same as the pointer to the first element.
  ASSERT_EQ(list, CPLListGet(list, 0));
  ASSERT_EQ(list, CPLListGetLast(list));
  ASSERT_EQ(nullptr, CPLListGetData(list));

  CPLListDestroy(list);
}

// Tests append, insert, and remove with a short list.
TEST(CplListTest, TwoElement) {
  CPLList* list = nullptr;
  int first = 1;
  int second = 2;

  list = CPLListAppend(list, &first);
  list = CPLListAppend(list, &second);

  ASSERT_EQ(2, CPLListCount(list));
  ASSERT_EQ(&first, CPLListGet(list, 0)->pData);
  ASSERT_EQ(&second, CPLListGet(list, 1)->pData);
  ASSERT_EQ(&second, CPLListGetLast(list)->pData);
  ASSERT_EQ(&second, CPLListGetNext(list)->pData);

  CPLList *elem2 = CPLListGetNext(list);
  list = CPLListRemove(list, 0);
  // The second element becomes the first and is the pointer
  // representing the list.
  ASSERT_EQ(elem2, list);
  ASSERT_EQ(1, CPLListCount(list));
  ASSERT_EQ(&second, CPLListGet(list, 0)->pData);
  ASSERT_EQ(&second, CPLListGetLast(list)->pData);

  list = CPLListInsert(list, &first, 0);
  ASSERT_EQ(2, CPLListCount(list));
  ASSERT_EQ(&first, CPLListGet(list, 0)->pData);
  ASSERT_EQ(&second, CPLListGet(list, 1)->pData);

  int third = 3;
  list = CPLListInsert(list, &third, 1);
  ASSERT_EQ(3, CPLListCount(list));
  ASSERT_EQ(&first, CPLListGet(list, 0)->pData);
  ASSERT_EQ(&third, CPLListGet(list, 1)->pData);
  ASSERT_EQ(&second, CPLListGet(list, 2)->pData);

  CPLListDestroy(list);
}

// Tests insert cases.
TEST(CplListTest, CplListInsert) {
  CPLList* list = nullptr;
  int first = 1;
  int second = 2;

  list = CPLListInsert(list, &first, 0);
  ASSERT_EQ(1, CPLListCount(list));
  ASSERT_EQ(&first, list->pData);

  list = CPLListInsert(list, &second, 1);
  ASSERT_EQ(2, CPLListCount(list));
  ASSERT_EQ(&second, CPLListGet(list, 1)->pData);

  // Insert one past the end and create an empty node in the gap.
  int third = 3;
  list = CPLListInsert(list, &third, 3);
  ASSERT_EQ(4, CPLListCount(list));
  ASSERT_EQ(nullptr, CPLListGet(list, 2)->pData);
  ASSERT_EQ(&third, CPLListGet(list, 3)->pData);

  CPLListDestroy(list);
}

// Tests handling of nullptr.
TEST(CplListTest, CplListNullPtr) {
  ASSERT_EQ(nullptr, CPLListGetLast(nullptr));
  ASSERT_EQ(nullptr, CPLListGet(nullptr, 2));
  ASSERT_EQ(0, CPLListCount(nullptr));

  ASSERT_EQ(nullptr, CPLListGetNext(nullptr));
  ASSERT_EQ(nullptr, CPLListGetData(nullptr));

  // Try to access past the end of the list.
  CPLList* list = CPLListAppend(nullptr, nullptr);
  ASSERT_NE(nullptr, list);
  ASSERT_EQ(nullptr, CPLListGet(list, 4));
  CPLListDestroy(list);
}

// Tests remove cases.
TEST(CplListTest, CplListRemove) {
  ASSERT_EQ(nullptr, CPLListRemove(nullptr, 0));
  ASSERT_EQ(nullptr, CPLListRemove(nullptr, 10));

  CPLList* list = nullptr;
  int first = 1;
  int second = 2;
  int third = 3;

  list = CPLListAppend(list, &first);
  list = CPLListAppend(list, &second);
  list = CPLListAppend(list, &third);
  ASSERT_EQ(3, CPLListCount(list));

  CPLList *item2 = CPLListGet(list, 1);
  list = CPLListRemove(list, 0);
  ASSERT_EQ(item2, list);
  ASSERT_EQ(2, CPLListCount(list));

  list = CPLListRemove(list, 1);
  ASSERT_EQ(item2, list);
  ASSERT_EQ(1, CPLListCount(list));
  ASSERT_EQ(&second, list->pData);

  CPLListDestroy(list);
}

}  // namespace
