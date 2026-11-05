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

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_swq.h"

namespace autotest2 {
namespace {

TEST(SwqOpRegistrarTest, GetOperatorByName) {
  const swq_operation* op = swq_op_registrar::GetOperator("OR");
  ASSERT_NE(nullptr, op);
  EXPECT_EQ(SWQ_OR, op->eOperation);
  EXPECT_STREQ("OR", op->pszName);

  // Case insensitive
  op = swq_op_registrar::GetOperator("and");
  ASSERT_NE(nullptr, op);
  EXPECT_EQ(SWQ_AND, op->eOperation);

  // Unrecognized operator
  EXPECT_EQ(nullptr, swq_op_registrar::GetOperator("NON_EXISTENT_OP"));
}

TEST(SwqOpRegistrarTest, GetOperatorByEnum) {
  const swq_operation* op = swq_op_registrar::GetOperator(SWQ_CAST);
  ASSERT_NE(nullptr, op);
  EXPECT_EQ(SWQ_CAST, op->eOperation);
  EXPECT_STREQ("CAST", op->pszName);

  op = swq_op_registrar::GetOperator(SWQ_AVG);
  ASSERT_NE(nullptr, op);
  EXPECT_EQ(SWQ_AVG, op->eOperation);

  // Enum value not present in operator table (SWQ_CUSTOM_FUNC)
  EXPECT_EQ(nullptr, swq_op_registrar::GetOperator(SWQ_CUSTOM_FUNC));
}

TEST(SwqOpRegistrarTest, ColumnFuncChecker) {
  const swq_operation* op = swq_op_registrar::GetOperator("AVG");
  ASSERT_NE(nullptr, op);
  ASSERT_NE(nullptr, op->pfnChecker);

  WithQuietHandler quiet_handler;

  // Normal column summary function used in inappropriate context
  swq_expr_node node(SWQ_AVG);
  EXPECT_EQ(SWQ_ERROR, op->pfnChecker(&node, 0));

  // Node with an operation not recognized in table (poOp == nullptr branch)
  node.nOperation = SWQ_CUSTOM_FUNC;
  EXPECT_EQ(SWQ_ERROR, op->pfnChecker(&node, 0));
}

}  // namespace
}  // namespace autotest2
