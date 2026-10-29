// Copyright 2026 Google Inc. All Rights Reserved.
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
// Tests gcore/gdalmultidomainmetadata.cpp.

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"

namespace {

TEST(GdalmultidomainmetadataTest, GetSetClearAndXMLSerialize) {
  GDALMultiDomainMetadata mdmd;

  // Empty state.
  EXPECT_EQ(nullptr, mdmd.GetMetadata(nullptr));
  EXPECT_EQ(nullptr, mdmd.GetMetadataItem("K", nullptr));

  // SetMetadataItem on default and named domains.
  EXPECT_EQ(CE_None, mdmd.SetMetadataItem("K1", "V1", nullptr));
  EXPECT_EQ(CE_None, mdmd.SetMetadataItem("K2", "V2", ""));
  EXPECT_STREQ("V1", mdmd.GetMetadataItem("K1", nullptr));
  EXPECT_STREQ("V2", mdmd.GetMetadataItem("K2", ""));

  // SetMetadata on sorted domain vs unsorted domains (xml:, json:, SUBDATASETS,
  // IMD).
  CPLStringList list_default;
  list_default.SetNameValue("B_KEY", "2");
  list_default.SetNameValue("A_KEY", "1");
  EXPECT_EQ(CE_None, mdmd.SetMetadata(list_default.List(), "CUSTOM"));
  EXPECT_STREQ("1", mdmd.GetMetadataItem("A_KEY", "CUSTOM"));

  CPLStringList xml_list;
  xml_list.AddString("<root><child>hello</child></root>");
  EXPECT_EQ(CE_None, mdmd.SetMetadata(xml_list.List(), "xml:TREES"));

  CPLStringList json_list;
  json_list.AddString("{\"hello\": \"world\"}");
  EXPECT_EQ(CE_None, mdmd.SetMetadata(json_list.List(), "json:DATA"));

  CPLStringList imd_list;
  imd_list.SetNameValue("Z", "1");
  imd_list.SetNameValue("A", "2");
  EXPECT_EQ(CE_None, mdmd.SetMetadata(imd_list.List(), "IMD"));

  // Empty domain should be skipped by Serialize().
  EXPECT_EQ(CE_None, mdmd.SetMetadata(nullptr, "EMPTY_DOMAIN"));

  // Serialize to XML and re-import via XMLInit into a second instance.
  CPLXMLNode* serialized = mdmd.Serialize();
  ASSERT_NE(nullptr, serialized);

  CPLXMLNode* root = CPLCreateXMLNode(nullptr, CXT_Element, "PAMDataset");
  CPLAddXMLChild(root, serialized);

  GDALMultiDomainMetadata mdmd2;
  EXPECT_TRUE(mdmd2.XMLInit(root, FALSE));
  EXPECT_STREQ("V1", mdmd2.GetMetadataItem("K1", ""));
  EXPECT_STREQ("1", mdmd2.GetMetadataItem("A_KEY", "CUSTOM"));
  ASSERT_NE(nullptr, mdmd2.GetMetadata("xml:TREES"));
  ASSERT_NE(nullptr, mdmd2.GetMetadata("json:DATA"));
  EXPECT_STREQ("{\"hello\": \"world\"}", mdmd2.GetMetadata("json:DATA")[0]);

  CPLDestroyXMLNode(root);

  mdmd.Clear();
  EXPECT_EQ(nullptr, mdmd.GetMetadata(""));
}

}  // namespace
