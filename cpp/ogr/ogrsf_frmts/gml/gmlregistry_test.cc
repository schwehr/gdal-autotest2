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
// Tests for GMLRegistry, GMLRegistryNamespace, and GMLRegistryFeatureType.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/gmlregistry.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/gml/gmlregistry.h"

#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

constexpr std::string_view kValidRegistryXml = R"xml(
<gml_registry>
  <!-- Comment to test non-element skipping -->
  <other_element foo="bar"/>
  <namespace prefix="missing_uri_ns"/>
  <namespace prefix="myns" uri="http://example.com/ns" useGlobalSRSName="true">
    <!-- Comment inside namespace -->
    <other_child_elem/>
    <featureType schemaLocation="missing_element_name.xsd"/>
    <featureType elementName="MissingSchemas"/>
    <featureType elementName="RelXsd" schemaLocation="sub/schema.xsd"/>
    <featureType elementName="HttpXsd" elementValue="val1" schemaLocation="http://example.com/remote.xsd"/>
    <featureType elementName="HttpsXsd" schemaLocation="https://example.com/secure.xsd"/>
    <featureType elementName="AbsXsd" schemaLocation="/vsimem/gmlregistry_test/abs.xsd"/>
    <featureType elementName="RelGfs" gfsSchemaLocation="sub/schema.gfs"/>
    <featureType elementName="HttpGfs" gfsSchemaLocation="http://example.com/remote.gfs"/>
    <featureType elementName="HttpsGfs" gfsSchemaLocation="https://example.com/secure.gfs"/>
    <featureType elementName="AbsGfs" gfsSchemaLocation="/vsimem/gmlregistry_test/abs.gfs"/>
  </namespace>
  <namespace prefix="simple" uri="http://example.com/simple" useGlobalSRSName="false">
    <featureType elementName="Plain" schemaLocation="plain.xsd"/>
  </namespace>
</gml_registry>
)xml";

TEST(GMLRegistryTest, ParseNonExistentFile) {
  WithQuietHandler quiet_handler;
  GMLRegistry registry("/vsimem/gmlregistry_test/nonexistent.xml");
  EXPECT_FALSE(registry.Parse());
}

TEST(GMLRegistryTest, ParseInvalidXmlRoot) {
  constexpr std::string_view kInvalidRootXml =
      "<not_gml_registry><child/></not_gml_registry>";
  constexpr char kPath[] = "/vsimem/gmlregistry_test/invalid_root.xml";
  VsiMemTempWrapper file(kPath, std::string(kInvalidRootXml));

  WithQuietHandler quiet_handler;
  GMLRegistry registry(kPath);
  EXPECT_FALSE(registry.Parse());
}

TEST(GMLRegistryTest, ParseWithFinderLocation) {
  constexpr char kDir[] = "/vsimem/gmlregistry_test/finder_dir";
  constexpr char kPath[] =
      "/vsimem/gmlregistry_test/finder_dir/gml_registry.xml";
  constexpr std::string_view kMinimalXml =
      "<gml_registry><namespace prefix=\"f\" "
      "uri=\"http://find.me\"/></gml_registry>";
  VsiMemTempWrapper file(kPath, std::string(kMinimalXml));

  CPLPushFinderLocation(kDir);
  absl::Cleanup pop_finder = [] { CPLPopFinderLocation(); };

  GMLRegistry registry("");
  EXPECT_TRUE(registry.Parse());
  EXPECT_EQ(1, registry.aoNamespaces.size());
  EXPECT_EQ("f", registry.aoNamespaces[0].osPrefix);
  EXPECT_EQ("http://find.me", registry.aoNamespaces[0].osURI);
}

TEST(GMLRegistryTest, ParseFullRegistryXml) {
  constexpr char kPath[] = "/vsimem/gmlregistry_test/valid_registry.xml";
  VsiMemTempWrapper file(kPath, std::string(kValidRegistryXml));

  GMLRegistry registry(kPath);
  ASSERT_TRUE(registry.Parse());
  ASSERT_EQ(2, registry.aoNamespaces.size());

  // Namespace 1: myns
  const auto& ns1 = registry.aoNamespaces[0];
  EXPECT_EQ("myns", ns1.osPrefix);
  EXPECT_EQ("http://example.com/ns", ns1.osURI);
  EXPECT_TRUE(ns1.bUseGlobalSRSName);
  ASSERT_EQ(8, ns1.aoFeatureTypes.size());

  // FeatureType 0: RelXsd (relative schema location resolved against dir)
  EXPECT_EQ("RelXsd", ns1.aoFeatureTypes[0].osElementName);
  EXPECT_EQ("/vsimem/gmlregistry_test/sub/schema.xsd",
            ns1.aoFeatureTypes[0].osSchemaLocation);
  EXPECT_TRUE(ns1.aoFeatureTypes[0].osGFSSchemaLocation.empty());
  EXPECT_TRUE(ns1.aoFeatureTypes[0].osElementValue.empty());

  // FeatureType 1: HttpXsd
  EXPECT_EQ("HttpXsd", ns1.aoFeatureTypes[1].osElementName);
  EXPECT_EQ("http://example.com/remote.xsd",
            ns1.aoFeatureTypes[1].osSchemaLocation);
  EXPECT_EQ("val1", ns1.aoFeatureTypes[1].osElementValue);

  // FeatureType 2: HttpsXsd
  EXPECT_EQ("HttpsXsd", ns1.aoFeatureTypes[2].osElementName);
  EXPECT_EQ("https://example.com/secure.xsd",
            ns1.aoFeatureTypes[2].osSchemaLocation);

  // FeatureType 3: AbsXsd
  EXPECT_EQ("AbsXsd", ns1.aoFeatureTypes[3].osElementName);
  EXPECT_EQ("/vsimem/gmlregistry_test/abs.xsd",
            ns1.aoFeatureTypes[3].osSchemaLocation);

  // FeatureType 4: RelGfs
  EXPECT_EQ("RelGfs", ns1.aoFeatureTypes[4].osElementName);
  EXPECT_TRUE(ns1.aoFeatureTypes[4].osSchemaLocation.empty());
  EXPECT_EQ("/vsimem/gmlregistry_test/sub/schema.gfs",
            ns1.aoFeatureTypes[4].osGFSSchemaLocation);

  // FeatureType 5: HttpGfs
  EXPECT_EQ("HttpGfs", ns1.aoFeatureTypes[5].osElementName);
  EXPECT_EQ("http://example.com/remote.gfs",
            ns1.aoFeatureTypes[5].osGFSSchemaLocation);

  // FeatureType 6: HttpsGfs
  EXPECT_EQ("HttpsGfs", ns1.aoFeatureTypes[6].osElementName);
  EXPECT_EQ("https://example.com/secure.gfs",
            ns1.aoFeatureTypes[6].osGFSSchemaLocation);

  // FeatureType 7: AbsGfs
  EXPECT_EQ("AbsGfs", ns1.aoFeatureTypes[7].osElementName);
  EXPECT_EQ("/vsimem/gmlregistry_test/abs.gfs",
            ns1.aoFeatureTypes[7].osGFSSchemaLocation);

  // Namespace 2: simple
  const auto& ns2 = registry.aoNamespaces[1];
  EXPECT_EQ("simple", ns2.osPrefix);
  EXPECT_EQ("http://example.com/simple", ns2.osURI);
  EXPECT_FALSE(ns2.bUseGlobalSRSName);
  ASSERT_EQ(1, ns2.aoFeatureTypes.size());
  EXPECT_EQ("Plain", ns2.aoFeatureTypes[0].osElementName);
  EXPECT_EQ("/vsimem/gmlregistry_test/plain.xsd",
            ns2.aoFeatureTypes[0].osSchemaLocation);
}

}  // namespace
}  // namespace autotest2
