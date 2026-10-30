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

#include <map>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/netcdf/netcdfdataset.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(NetCDFWriterConfigTest, ParseInvalidXml) {
  netCDFWriterConfiguration config;
  // Malformed XML string starting with <Configuration
  EXPECT_FALSE(config.Parse("<Configuration><UnclosedTag>"));
  EXPECT_FALSE(config.m_bIsValid);

  // Non-existent file
  EXPECT_FALSE(config.Parse("/vsimem/non_existent_config.xml"));
  EXPECT_FALSE(config.m_bIsValid);
}

TEST(NetCDFWriterConfigTest, SetNameValue) {
  std::map<CPLString, CPLString> oMap;

  // Missing name and value
  CPLXMLNode* node_empty = CPLCreateXMLNode(nullptr, CXT_Element, "Option");
  CPLXMLTreeCloser closer_empty(node_empty);
  CPLErrorReset();
  EXPECT_FALSE(netCDFWriterConfiguration::SetNameValue(node_empty, oMap));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());

  // Valid name and value
  CPLXMLNode* node_valid = CPLCreateXMLNode(nullptr, CXT_Element, "Option");
  CPLXMLTreeCloser closer_valid(node_valid);
  CPLAddXMLAttributeAndValue(node_valid, "name", "FORMAT");
  CPLAddXMLAttributeAndValue(node_valid, "value", "NC4");
  EXPECT_TRUE(netCDFWriterConfiguration::SetNameValue(node_valid, oMap));
  EXPECT_EQ("NC4", oMap["FORMAT"]);
}

TEST(NetCDFWriterConfigTest, AttributeParse) {
  netCDFWriterConfigAttribute attr;

  // Unsupported type
  {
    CPLXMLNode* node = CPLCreateXMLNode(nullptr, CXT_Element, "Attribute");
    CPLXMLTreeCloser closer(node);
    CPLAddXMLAttributeAndValue(node, "name", "units");
    CPLAddXMLAttributeAndValue(node, "value", "m");
    CPLAddXMLAttributeAndValue(node, "type", "invalid_type");
    CPLErrorReset();
    EXPECT_FALSE(attr.Parse(node));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  }

  // Missing name
  {
    CPLXMLNode* node = CPLCreateXMLNode(nullptr, CXT_Element, "Attribute");
    CPLXMLTreeCloser closer(node);
    CPLAddXMLAttributeAndValue(node, "value", "10");
    CPLAddXMLAttributeAndValue(node, "type", "integer");
    CPLErrorReset();
    EXPECT_FALSE(attr.Parse(node));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  }

  // Valid string type (default type)
  {
    CPLXMLNode* node = CPLCreateXMLNode(nullptr, CXT_Element, "Attribute");
    CPLXMLTreeCloser closer(node);
    CPLAddXMLAttributeAndValue(node, "name", "units");
    CPLAddXMLAttributeAndValue(node, "value", "degrees");
    EXPECT_TRUE(attr.Parse(node));
    EXPECT_EQ("units", attr.m_osName);
    EXPECT_EQ("degrees", attr.m_osValue);
    EXPECT_EQ("string", attr.m_osType);
  }

  // Valid integer type
  {
    CPLXMLNode* node = CPLCreateXMLNode(nullptr, CXT_Element, "Attribute");
    CPLXMLTreeCloser closer(node);
    CPLAddXMLAttributeAndValue(node, "name", "scale");
    CPLAddXMLAttributeAndValue(node, "value", "100");
    CPLAddXMLAttributeAndValue(node, "type", "integer");
    EXPECT_TRUE(attr.Parse(node));
    EXPECT_EQ("scale", attr.m_osName);
    EXPECT_EQ("100", attr.m_osValue);
    EXPECT_EQ("integer", attr.m_osType);
  }

  // Valid double type
  {
    CPLXMLNode* node = CPLCreateXMLNode(nullptr, CXT_Element, "Attribute");
    CPLXMLTreeCloser closer(node);
    CPLAddXMLAttributeAndValue(node, "name", "offset");
    CPLAddXMLAttributeAndValue(node, "value", "1.5");
    CPLAddXMLAttributeAndValue(node, "type", "double");
    EXPECT_TRUE(attr.Parse(node));
    EXPECT_EQ("offset", attr.m_osName);
    EXPECT_EQ("1.5", attr.m_osValue);
    EXPECT_EQ("double", attr.m_osType);
  }
}

TEST(NetCDFWriterConfigTest, FieldParse) {
  netCDFWriterConfigField field;

  // Missing name and netcdf_name
  {
    CPLXMLNode* node = CPLCreateXMLNode(nullptr, CXT_Element, "Field");
    CPLXMLTreeCloser closer(node);
    CPLErrorReset();
    EXPECT_FALSE(field.Parse(node));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  }

  // With name, netcdf_name, main_dim, child elements and comments
  {
    CPLXMLNode* node = CPLCreateXMLNode(nullptr, CXT_Element, "Field");
    CPLXMLTreeCloser closer(node);
    CPLAddXMLAttributeAndValue(node, "name", "temp");
    CPLAddXMLAttributeAndValue(node, "netcdf_name", "temperature");
    CPLAddXMLAttributeAndValue(node, "main_dim", "time");

    // Comment node (non-element)
    CPLCreateXMLNode(node, CXT_Comment, "Some comment");

    // Child attribute
    CPLXMLNode* child_attr = CPLCreateXMLNode(node, CXT_Element, "Attribute");
    CPLAddXMLAttributeAndValue(child_attr, "name", "units");
    CPLAddXMLAttributeAndValue(child_attr, "value", "celsius");

    // Unknown child node (to trigger ignore debug branch)
    CPLCreateXMLNode(node, CXT_Element, "UnknownFieldChild");

    EXPECT_TRUE(field.Parse(node));
    EXPECT_EQ("temp", field.m_osName);
    EXPECT_EQ("temperature", field.m_osNetCDFName);
    EXPECT_EQ("time", field.m_osMainDim);
    ASSERT_EQ(1, field.m_aoAttributes.size());
    EXPECT_EQ("units", field.m_aoAttributes[0].m_osName);
  }
}

TEST(NetCDFWriterConfigTest, LayerParse) {
  netCDFWriterConfigLayer layer;

  // Missing name
  {
    CPLXMLNode* node = CPLCreateXMLNode(nullptr, CXT_Element, "Layer");
    CPLXMLTreeCloser closer(node);
    CPLErrorReset();
    EXPECT_FALSE(layer.Parse(node));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  }

  // Complete layer with children
  {
    CPLXMLNode* node = CPLCreateXMLNode(nullptr, CXT_Element, "Layer");
    CPLXMLTreeCloser closer(node);
    CPLAddXMLAttributeAndValue(node, "name", "stations");
    CPLAddXMLAttributeAndValue(node, "netcdf_name", "station_layer");

    // Comment node
    CPLCreateXMLNode(node, CXT_Comment, "Layer comment");

    // LayerCreationOption
    CPLXMLNode* lco =
        CPLCreateXMLNode(node, CXT_Element, "LayerCreationOption");
    CPLAddXMLAttributeAndValue(lco, "name", "RECORD_DIM_NAME");
    CPLAddXMLAttributeAndValue(lco, "value", "station_id");

    // Attribute
    CPLXMLNode* attr = CPLCreateXMLNode(node, CXT_Element, "Attribute");
    CPLAddXMLAttributeAndValue(attr, "name", "description");
    CPLAddXMLAttributeAndValue(attr, "value", "Weather stations");

    // Field with name
    CPLXMLNode* f1 = CPLCreateXMLNode(node, CXT_Element, "Field");
    CPLAddXMLAttributeAndValue(f1, "name", "elevation");

    // Field without name (netcdf_name only)
    CPLXMLNode* f2 = CPLCreateXMLNode(node, CXT_Element, "Field");
    CPLAddXMLAttributeAndValue(f2, "netcdf_name", "pressure");

    // Unknown child
    CPLCreateXMLNode(node, CXT_Element, "UnknownLayerChild");

    EXPECT_TRUE(layer.Parse(node));
    EXPECT_EQ("stations", layer.m_osName);
    EXPECT_EQ("station_layer", layer.m_osNetCDFName);
    EXPECT_EQ("station_id", layer.m_oLayerCreationOptions["RECORD_DIM_NAME"]);
    ASSERT_EQ(1, layer.m_aoAttributes.size());
    EXPECT_EQ("description", layer.m_aoAttributes[0].m_osName);
    EXPECT_EQ(2, layer.m_oFields.size());
    EXPECT_TRUE(layer.m_oFields.find("elevation") != layer.m_oFields.end());
    EXPECT_TRUE(layer.m_oFields.find("__pressure") != layer.m_oFields.end());
  }
}

TEST(NetCDFWriterConfigTest, ParseFullConfigurationStringAndFile) {
  const std::string xml_content =
      "<Configuration>\n"
      "    <!-- Top-level comment -->\n"
      "    <DatasetCreationOption name=\"COMPRESS\" value=\"DEFLATE\"/>\n"
      "    <LayerCreationOption name=\"FEATURE_TYPE\" value=\"POINT\"/>\n"
      "    <Attribute name=\"Conventions\" value=\"CF-1.6\" type=\"string\"/>\n"
      "    <Field name=\"temperature\" netcdf_name=\"temp\" "
      "main_dim=\"dim1\">\n"
      "        <Attribute name=\"units\" value=\"kelvin\"/>\n"
      "    </Field>\n"
      "    <Field netcdf_name=\"humidity\">\n"
      "        <Attribute name=\"units\" value=\"percentage\"/>\n"
      "    </Field>\n"
      "    <Layer name=\"my_layer\" netcdf_name=\"nc_layer\">\n"
      "        <LayerCreationOption name=\"STRING_AS_VARIABLE\" "
      "value=\"YES\"/>\n"
      "        <Attribute name=\"title\" value=\"My Layer\"/>\n"
      "        <Field name=\"f1\"/>\n"
      "    </Layer>\n"
      "    <UnknownGlobalChild name=\"ignored\"/>\n"
      "</Configuration>";

  // Test Parse from string starting with <Configuration
  {
    netCDFWriterConfiguration config;
    EXPECT_TRUE(config.Parse(xml_content.c_str()));
    EXPECT_TRUE(config.m_bIsValid);
    EXPECT_EQ("DEFLATE", config.m_oDatasetCreationOptions["COMPRESS"]);
    EXPECT_EQ("POINT", config.m_oLayerCreationOptions["FEATURE_TYPE"]);
    ASSERT_EQ(1, config.m_aoAttributes.size());
    EXPECT_EQ("Conventions", config.m_aoAttributes[0].m_osName);
    EXPECT_EQ("CF-1.6", config.m_aoAttributes[0].m_osValue);
    EXPECT_EQ("string", config.m_aoAttributes[0].m_osType);
    EXPECT_EQ(2, config.m_oFields.size());
    EXPECT_EQ("temp", config.m_oFields["temperature"].m_osNetCDFName);
    EXPECT_EQ("humidity", config.m_oFields["__humidity"].m_osNetCDFName);
    ASSERT_EQ(1, config.m_oLayers.size());
    EXPECT_EQ("nc_layer", config.m_oLayers["my_layer"].m_osNetCDFName);
  }

  // Test Parse from in-memory file (/vsimem/)
  {
    const char* vsimem_path = "/vsimem/test_netcdf_writer_config.xml";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path,
        reinterpret_cast<GByte*>(const_cast<char*>(xml_content.data())),
        xml_content.size(), FALSE));

    netCDFWriterConfiguration config;
    EXPECT_TRUE(config.Parse(vsimem_path));
    EXPECT_TRUE(config.m_bIsValid);
    EXPECT_EQ("DEFLATE", config.m_oDatasetCreationOptions["COMPRESS"]);

    VSIUnlink(vsimem_path);
  }
}

}  // namespace
}  // namespace autotest2
