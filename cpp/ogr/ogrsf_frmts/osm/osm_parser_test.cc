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
// Tests for the OSM XML and PBF parser (osm_parser.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/osm/osm_parser.cpp
//   https://gdal.org/drivers/vector/osm.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_osm.py
//
// Google3 build caveats:
//   - Compiled with HAVE_EXPAT enabled so XML parsing via Expat is active,
//     and WITHOUT_CPLDEBUG defined so CPLDebug calls are compiled out.
//   - Tests that check exact OSM_ProcessBlock step counts pin
//     GDAL_NUM_THREADS explicitly via CPLConfigOptionSetter so PBF block
//     emission does not vary with host CPU count.

#include "third_party/gdal/ogr/ogrsf_frmts/osm/osm_parser.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/osm/gpb.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

using OsmContextUniquePtr =
    std::unique_ptr<OSMContext, decltype([](OSMContext* c) { OSM_Close(c); })>;

OSMRetCode DrainBlocks(OSMContext* ctxt) {
  OSMRetCode ret = OSM_OK;
  while ((ret = OSM_ProcessBlock(ctxt)) == OSM_OK) {
  }
  return ret;
}

void AppendBigEndian32(std::string& out, uint32_t val) {
  out.push_back(static_cast<char>((val >> 24) & 0xFF));
  out.push_back(static_cast<char>((val >> 16) & 0xFF));
  out.push_back(static_cast<char>((val >> 8) & 0xFF));
  out.push_back(static_cast<char>(val & 0xFF));
}

void AppendVarUInt(std::string& out, uint64_t val) {
  while (val > 127) {
    out.push_back(static_cast<char>((val & 0x7f) | 0x80));
    val >>= 7;
  }
  out.push_back(static_cast<char>(val));
}

void AppendVarSInt(std::string& out, int64_t val) {
  AppendVarUInt(out, EncodeSInt(static_cast<GIntBig>(val)));
}

void AppendKey(std::string& out, int field_num, int wire_type) {
  AppendVarUInt(out, MAKE_KEY(field_num, wire_type));
}

void AppendBytesField(std::string& out, int field_num,
                      const std::string& data) {
  AppendKey(out, field_num, WT_DATA);
  AppendVarUInt(out, data.size());
  out.append(data);
}

void AppendVarUIntField(std::string& out, int field_num, uint64_t val) {
  AppendKey(out, field_num, WT_VARINT);
  AppendVarUInt(out, val);
}

void AppendVarSIntField(std::string& out, int field_num, int64_t val) {
  AppendKey(out, field_num, WT_VARINT);
  AppendVarSInt(out, val);
}

std::string MakeBlob(const std::string& raw_payload, bool compress = true) {
  std::string blob;
  if (compress) {
    size_t comp_size = 0;
    void* compressed = CPLZLibDeflate(raw_payload.data(), raw_payload.size(),
                                      -1, nullptr, 0, &comp_size);
    if (compressed != nullptr) {
      std::unique_ptr<void, decltype(&VSIFree)> compressed_guard(compressed,
                                                                 VSIFree);
      AppendVarUIntField(blob, 2 /* BLOB_IDX_RAW_SIZE */, raw_payload.size());
      AppendBytesField(
          blob, 3 /* BLOB_IDX_ZLIB_DATA */,
          std::string(static_cast<const char*>(compressed), comp_size));
      return blob;
    }
  }
  AppendBytesField(blob, 1 /* BLOB_IDX_RAW */, raw_payload);
  return blob;
}

std::string MakeFileBlockFromBlob(const std::string& type,
                                  const std::string& blob,
                                  const std::string& indexdata = "") {
  std::string header;
  AppendBytesField(header, 1 /* BLOBHEADER_IDX_TYPE */, type);
  if (!indexdata.empty()) {
    AppendBytesField(header, 2 /* BLOBHEADER_IDX_INDEXDATA */, indexdata);
  }
  AppendVarUIntField(header, 3 /* BLOBHEADER_IDX_DATASIZE */, blob.size());

  std::string block;
  AppendBigEndian32(block, static_cast<uint32_t>(header.size()));
  block.append(header);
  block.append(blob);
  return block;
}

std::string MakeFileBlock(const std::string& type,
                          const std::string& raw_payload, bool compress = true,
                          const std::string& indexdata = "") {
  return MakeFileBlockFromBlob(type, MakeBlob(raw_payload, compress),
                               indexdata);
}

std::string MakeHeaderBlock(int64_t left, int64_t bottom, int64_t right,
                            int64_t top, bool include_bbox = true,
                            bool include_required = true,
                            bool include_optional = true,
                            bool include_extra_metadata = true) {
  std::string header;
  if (include_bbox) {
    std::string bbox;
    AppendVarSIntField(bbox, 1 /* HEADERBBOX_IDX_LEFT */, left);
    AppendVarSIntField(bbox, 2 /* HEADERBBOX_IDX_RIGHT */, right);
    AppendVarSIntField(bbox, 3 /* HEADERBBOX_IDX_TOP */, top);
    AppendVarSIntField(bbox, 4 /* HEADERBBOX_IDX_BOTTOM */, bottom);
    AppendBytesField(header, 1 /* HEADERBLOCK_IDX_BBOX */, bbox);
  }
  if (include_required) {
    AppendBytesField(header, 4 /* HEADERBLOCK_IDX_REQUIRED_FEATURES */,
                     "OsmSchema-V0.6");
    AppendBytesField(header, 4 /* HEADERBLOCK_IDX_REQUIRED_FEATURES */,
                     "DenseNodes");
  }
  if (include_optional) {
    AppendBytesField(header, 5 /* HEADERBLOCK_IDX_OPTIONAL_FEATURES */,
                     "Has_Historic_Info");
    AppendBytesField(header, 5 /* HEADERBLOCK_IDX_OPTIONAL_FEATURES */,
                     "Sort.Type_then_ID");
  }
  if (include_extra_metadata) {
    AppendBytesField(header, 16 /* HEADERBLOCK_IDX_WRITINGPROGRAM */,
                     "GDAL autotest2 osm_parser_test");
    AppendBytesField(header, 17 /* HEADERBLOCK_IDX_SOURCE */, "Synthetic PBF");
    AppendVarSIntField(header,
                       32 /* HEADERBLOCK_IDX_OSMOSIS_REPLICATION_TIMESTAMP */,
                       1600000000);
    AppendVarUIntField(
        header, 33 /* HEADERBLOCK_IDX_OSMOSIS_REPLICATION_SEQUENCENUMBER */,
        12345);
    AppendBytesField(header,
                     34 /* HEADERBLOCK_IDX_OSMOSIS_REPLICATION_BASE_URL */,
                     "http://example.com/replication");
  }
  return header;
}

std::string MakeStringTable(const std::vector<std::string>& strings,
                            bool add_unknown = false) {
  std::string table;
  for (const auto& s : strings) {
    AppendBytesField(table, 1 /* STRINGTABLE_IDX_S */, s);
  }
  if (add_unknown) {
    AppendVarUIntField(table, 999 /* UNKNOWN_FIELD */, 42);
  }
  return table;
}

struct DenseNodeData {
  int64_t id = 0;
  double lat = 0.0;
  double lon = 0.0;
  int32_t version = 1;
  int64_t timestamp = 0;
  int64_t changeset = 0;
  int32_t uid = 0;
  int32_t user_sid = 0;
  std::vector<std::pair<uint32_t, uint32_t>> tags;
};

std::string MakeDenseNodes(const std::vector<DenseNodeData>& nodes,
                           bool add_dense_info = true,
                           bool add_unknown = false) {
  std::string out;
  std::string id_buf;
  std::string lat_buf;
  std::string lon_buf;
  std::string keys_vals_buf;

  std::string ver_buf;
  std::string ts_buf;
  std::string cs_buf;
  std::string uid_buf;
  std::string user_sid_buf;

  int64_t last_id = 0;
  int64_t last_lat = 0;
  int64_t last_lon = 0;
  int64_t last_ts = 0;
  int64_t last_cs = 0;
  int32_t last_uid = 0;
  int32_t last_user_sid = 0;

  for (const auto& n : nodes) {
    AppendVarSInt(id_buf, n.id - last_id);
    last_id = n.id;

    int64_t nlat = static_cast<int64_t>(std::round(n.lat * 1e7));
    AppendVarSInt(lat_buf, nlat - last_lat);
    last_lat = nlat;

    int64_t nlon = static_cast<int64_t>(std::round(n.lon * 1e7));
    AppendVarSInt(lon_buf, nlon - last_lon);
    last_lon = nlon;

    if (add_dense_info) {
      AppendVarUInt(ver_buf, n.version);
      AppendVarSInt(ts_buf, n.timestamp - last_ts);
      last_ts = n.timestamp;
      AppendVarSInt(cs_buf, n.changeset - last_cs);
      last_cs = n.changeset;
      AppendVarSInt(uid_buf, n.uid - last_uid);
      last_uid = n.uid;
      AppendVarSInt(user_sid_buf, n.user_sid - last_user_sid);
      last_user_sid = n.user_sid;
    }

    for (const auto& kv : n.tags) {
      AppendVarUInt(keys_vals_buf, kv.first);
      AppendVarUInt(keys_vals_buf, kv.second);
    }
    keys_vals_buf.push_back('\0');  // 0 delimiter
  }

  AppendBytesField(out, 1 /* DENSENODES_IDX_ID */, id_buf);
  AppendBytesField(out, 8 /* DENSENODES_IDX_LAT */, lat_buf);
  AppendBytesField(out, 9 /* DENSENODES_IDX_LON */, lon_buf);
  if (!keys_vals_buf.empty()) {
    AppendBytesField(out, 10 /* DENSENODES_IDX_KEYS_VALS */, keys_vals_buf);
  }

  if (add_dense_info) {
    std::string dense_info;
    AppendBytesField(dense_info, 1 /* DENSEINFO_IDX_VERSION */, ver_buf);
    AppendBytesField(dense_info, 2 /* DENSEINFO_IDX_TIMESTAMP */, ts_buf);
    AppendBytesField(dense_info, 3 /* DENSEINFO_IDX_CHANGESET */, cs_buf);
    AppendBytesField(dense_info, 4 /* DENSEINFO_IDX_UID */, uid_buf);
    AppendBytesField(dense_info, 5 /* DENSEINFO_IDX_USER_SID */, user_sid_buf);
    if (add_unknown) {
      AppendVarUIntField(dense_info, 999 /* UNKNOWN_FIELD */, 123);
    }
    AppendBytesField(out, 5 /* DENSENODES_IDX_DENSEINFO */, dense_info);
  }

  if (add_unknown) {
    AppendVarUIntField(out, 999 /* UNKNOWN_FIELD */, 456);
  }

  return out;
}

std::string MakeInfo(int32_t version = 1, int64_t timestamp = 0,
                     int64_t changeset = 0, int32_t uid = 0,
                     int32_t user_sid = 0, bool visible = true,
                     bool add_unknown = false) {
  std::string info;
  AppendVarUIntField(info, 1 /* INFO_IDX_VERSION */, version);
  AppendVarUIntField(info, 2 /* INFO_IDX_TIMESTAMP */, timestamp);
  AppendVarUIntField(info, 3 /* INFO_IDX_CHANGESET */, changeset);
  AppendVarUIntField(info, 4 /* INFO_IDX_UID */, uid);
  AppendVarUIntField(info, 5 /* INFO_IDX_USER_SID */, user_sid);
  AppendVarUIntField(info, 6 /* INFO_IDX_VISIBLE */, visible ? 1 : 0);
  if (add_unknown) {
    AppendVarUIntField(info, 999 /* UNKNOWN_FIELD */, 789);
  }
  return info;
}

std::string MakeNode(
    int64_t id, double lat, double lon,
    const std::vector<std::pair<uint32_t, uint32_t>>& tags = {},
    bool include_empty_info = false, bool add_unknown = false) {
  std::string node;
  AppendVarSIntField(node, 1 /* NODE_IDX_ID */, id);
  if (!tags.empty()) {
    std::string keys;
    std::string vals;
    for (const auto& kv : tags) {
      AppendVarUInt(keys, kv.first);
      AppendVarUInt(vals, kv.second);
    }
    AppendBytesField(node, 2 /* NODE_IDX_KEYS */, keys);
    AppendBytesField(node, 3 /* NODE_IDX_VALS */, vals);
  }
  if (include_empty_info) {
    AppendBytesField(node, 4 /* NODE_IDX_INFO */, "");
  }
  int64_t nlat = static_cast<int64_t>(std::round(lat * 1e7));
  int64_t nlon = static_cast<int64_t>(std::round(lon * 1e7));
  AppendVarSIntField(node, 8 /* NODE_IDX_LAT */, nlat);
  AppendVarSIntField(node, 9 /* NODE_IDX_LON */, nlon);
  if (add_unknown) {
    AppendVarUIntField(node, 999 /* UNKNOWN_FIELD */, 111);
  }
  return node;
}

std::string MakeWay(int64_t id, const std::vector<int64_t>& node_refs,
                    const std::vector<std::pair<uint32_t, uint32_t>>& tags = {},
                    const std::string& info_payload = "",
                    bool add_unknown = false) {
  std::string way;
  AppendVarUIntField(way, 1 /* WAY_IDX_ID */, id);
  if (!tags.empty()) {
    std::string keys;
    std::string vals;
    for (const auto& kv : tags) {
      AppendVarUInt(keys, kv.first);
      AppendVarUInt(vals, kv.second);
    }
    AppendBytesField(way, 2 /* WAY_IDX_KEYS */, keys);
    AppendBytesField(way, 3 /* WAY_IDX_VALS */, vals);
  }
  if (!info_payload.empty()) {
    AppendBytesField(way, 4 /* WAY_IDX_INFO */, info_payload);
  }
  if (!node_refs.empty()) {
    std::string refs;
    int64_t last_ref = 0;
    for (int64_t ref : node_refs) {
      AppendVarSInt(refs, ref - last_ref);
      last_ref = ref;
    }
    AppendBytesField(way, 8 /* WAY_IDX_REFS */, refs);
  }
  if (add_unknown) {
    AppendVarUIntField(way, 999 /* UNKNOWN_FIELD */, 222);
  }
  return way;
}

struct RelationMemberData {
  int64_t memid = 0;
  uint32_t role_sid = 0;
  OSMMemberType type = MEMBER_NODE;
};

std::string MakeRelation(
    int64_t id, const std::vector<RelationMemberData>& members,
    const std::vector<std::pair<uint32_t, uint32_t>>& tags = {},
    const std::string& info_payload = "", bool add_unknown = false) {
  std::string rel;
  AppendVarUIntField(rel, 1 /* RELATION_IDX_ID */, id);
  if (!tags.empty()) {
    std::string keys;
    std::string vals;
    for (const auto& kv : tags) {
      AppendVarUInt(keys, kv.first);
      AppendVarUInt(vals, kv.second);
    }
    AppendBytesField(rel, 2 /* RELATION_IDX_KEYS */, keys);
    AppendBytesField(rel, 3 /* RELATION_IDX_VALS */, vals);
  }
  if (!info_payload.empty()) {
    AppendBytesField(rel, 4 /* RELATION_IDX_INFO */, info_payload);
  }
  if (!members.empty()) {
    std::string roles_sid;
    std::string memids;
    std::string types;
    int64_t last_memid = 0;
    for (const auto& m : members) {
      AppendVarUInt(roles_sid, m.role_sid);
      AppendVarSInt(memids, m.memid - last_memid);
      last_memid = m.memid;
      types.push_back(static_cast<char>(m.type));
    }
    AppendBytesField(rel, 8 /* RELATION_IDX_ROLES_SID */, roles_sid);
    AppendBytesField(rel, 9 /* RELATION_IDX_MEMIDS */, memids);
    AppendBytesField(rel, 10 /* RELATION_IDX_TYPES */, types);
  }
  if (add_unknown) {
    AppendVarUIntField(rel, 999 /* UNKNOWN_FIELD */, 333);
  }
  return rel;
}

std::string MakePrimitiveBlock(const std::string& string_table,
                               const std::vector<std::string>& primitive_groups,
                               int32_t granularity = 100,
                               int32_t date_granularity = 1000,
                               int64_t lat_offset = 0, int64_t lon_offset = 0,
                               bool add_unknown_fields = false) {
  std::string block;
  AppendBytesField(block, 1 /* PRIMITIVEBLOCK_IDX_STRINGTABLE */, string_table);
  for (const auto& pg : primitive_groups) {
    AppendBytesField(block, 2 /* PRIMITIVEBLOCK_IDX_PRIMITIVEGROUP */, pg);
  }
  AppendVarUIntField(block, 17 /* PRIMITIVEBLOCK_IDX_GRANULARITY */,
                     granularity);
  AppendVarUIntField(block, 18 /* PRIMITIVEBLOCK_IDX_DATE_GRANULARITY */,
                     date_granularity);
  AppendVarSIntField(block, 19 /* PRIMITIVEBLOCK_IDX_LAT_OFFSET */, lat_offset);
  AppendVarSIntField(block, 20 /* PRIMITIVEBLOCK_IDX_LON_OFFSET */, lon_offset);
  if (add_unknown_fields) {
    AppendVarUIntField(block, 999 /* UNKNOWN_FIELD */, 999);
  }
  return block;
}

struct ParserCapture {
  int node_count = 0;
  int way_count = 0;
  int relation_count = 0;
  std::vector<int64_t> node_ids;
  std::vector<int64_t> way_ids;
  std::vector<int64_t> relation_ids;
  std::vector<double> bounds;
};

void NotifyNodesCbk(unsigned int nNodes, OSMNode* pasNodes,
                    OSMContext* /* psOSMContext */, void* user_data) {
  auto* cap = static_cast<ParserCapture*>(user_data);
  cap->node_count += nNodes;
  for (unsigned int i = 0; i < nNodes; ++i) {
    cap->node_ids.push_back(pasNodes[i].nID);
    for (unsigned int j = 0; j < pasNodes[i].nTags; ++j) {
      CPL_IGNORE_RET_VAL(pasNodes[i].pasTags[j].pszK);
      CPL_IGNORE_RET_VAL(pasNodes[i].pasTags[j].pszV);
    }
  }
}

void NotifyWayCbk(OSMWay* psWay, OSMContext* /* psOSMContext */,
                  void* user_data) {
  auto* cap = static_cast<ParserCapture*>(user_data);
  cap->way_count++;
  cap->way_ids.push_back(psWay->nID);
  for (unsigned int j = 0; j < psWay->nTags; ++j) {
    CPL_IGNORE_RET_VAL(psWay->pasTags[j].pszK);
    CPL_IGNORE_RET_VAL(psWay->pasTags[j].pszV);
  }
}

void NotifyRelationCbk(OSMRelation* psRelation, OSMContext* /* psOSMContext */,
                       void* user_data) {
  auto* cap = static_cast<ParserCapture*>(user_data);
  cap->relation_count++;
  cap->relation_ids.push_back(psRelation->nID);
  for (unsigned int j = 0; j < psRelation->nTags; ++j) {
    CPL_IGNORE_RET_VAL(psRelation->pasTags[j].pszK);
    CPL_IGNORE_RET_VAL(psRelation->pasTags[j].pszV);
  }
}

void NotifyBoundsCbk(double dfXMin, double dfYMin, double dfXMax, double dfYMax,
                     OSMContext* /* psOSMContext */, void* user_data) {
  auto* cap = static_cast<ParserCapture*>(user_data);
  cap->bounds = {dfXMin, dfYMin, dfXMax, dfYMax};
}

TEST(OsmParserTest, OpenNonExistentFileFails) {
  WithQuietHandler quiet;
  EXPECT_EQ(nullptr, OSM_Open("/vsimem/does_not_exist_xyz.pbf", nullptr,
                              nullptr, nullptr, nullptr, nullptr));
}

TEST(OsmParserTest, OpenUnrecognizedFormatFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/unrecognized.bin";
  VsiMemTempWrapper wrapper(path, "NOT_AN_OSM_FILE_AT_ALL_JUST_RANDOM_DATA");
  EXPECT_EQ(nullptr, OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr,
                              nullptr));
}

TEST(OsmParserTest, CloseNullContextIsSafe) { OSM_Close(nullptr); }

TEST(OsmParserTest, ValidPbfHappyPathAllElements) {
  CPLConfigOptionSetter threads_setter("GDAL_NUM_THREADS", "1",
                                       /*bSetOnlyIfUndefined=*/false);
  const std::string path = "/vsimem/valid_all_elements.pbf";
  std::string pbf_data;

  // 1. OSMHeader block
  std::string header_raw = MakeHeaderBlock(
      /*left=*/2000000000LL /* 2.0 deg */,
      /*bottom=*/48000000000LL /* 48.0 deg */,
      /*right=*/3000000000LL /* 3.0 deg */,
      /*top=*/49000000000LL /* 49.0 deg */);
  pbf_data.append(MakeFileBlock("OSMHeader", header_raw, /*compress=*/true,
                                /*indexdata=*/"ignored_index_bytes"));

  // 2. OSMData block with StringTable and PrimitiveGroups
  std::vector<std::string> strings = {
      "", "highway", "primary", "name", "Main Street", "outer", "alice"};
  std::string string_table = MakeStringTable(strings, /*add_unknown=*/true);

  // Group 1: DenseNodes
  std::vector<DenseNodeData> dense_nodes = {{.id = 101,
                                             .lat = 48.1,
                                             .lon = 2.1,
                                             .version = 1,
                                             .timestamp = 1000,
                                             .changeset = 10,
                                             .uid = 1,
                                             .user_sid = 6,
                                             .tags = {{1, 2}}},
                                            {.id = 102,
                                             .lat = 48.2,
                                             .lon = 2.2,
                                             .version = 2,
                                             .timestamp = 2000,
                                             .changeset = 20,
                                             .uid = 2,
                                             .user_sid = 6,
                                             .tags = {}}};
  std::string dense_payload =
      MakeDenseNodes(dense_nodes, /*add_dense_info=*/true,
                     /*add_unknown=*/true);
  std::string pg1;
  AppendBytesField(pg1, 2 /* PRIMITIVEGROUP_IDX_DENSE */, dense_payload);

  // Group 2: Standard Node (with empty info at end)
  std::string node_payload =
      MakeNode(/*id=*/201, /*lat=*/48.5, /*lon=*/2.5,
               /*tags=*/{{3, 4}}, /*include_empty_info=*/true,
               /*add_unknown=*/true);
  std::string pg2;
  AppendBytesField(pg2, 1 /* PRIMITIVEGROUP_IDX_NODES */, node_payload);

  // Group 3: Way
  std::string way_info =
      MakeInfo(/*version=*/3, /*timestamp=*/3000, /*changeset=*/30,
               /*uid=*/3, /*user_sid=*/6, /*visible=*/true,
               /*add_unknown=*/true);
  std::string way_payload =
      MakeWay(/*id=*/301, /*node_refs=*/{101, 102, 201},
              /*tags=*/{{1, 2}, {3, 4}}, way_info, /*add_unknown=*/true);
  std::string pg3;
  AppendBytesField(pg3, 3 /* PRIMITIVEGROUP_IDX_WAYS */, way_payload);

  // Group 4: Relation
  std::string rel_info =
      MakeInfo(/*version=*/4, /*timestamp=*/4000, /*changeset=*/40,
               /*uid=*/4, /*user_sid=*/6, /*visible=*/true,
               /*add_unknown=*/true);
  std::vector<RelationMemberData> members = {
      {/*memid=*/101, /*role_sid=*/5, MEMBER_NODE},
      {/*memid=*/301, /*role_sid=*/5, MEMBER_WAY}};
  std::string rel_payload =
      MakeRelation(/*id=*/401, members, /*tags=*/{{3, 4}}, rel_info,
                   /*add_unknown=*/true);
  std::string pg4;
  AppendBytesField(pg4, 4 /* PRIMITIVEGROUP_IDX_RELATIONS */, rel_payload);

  std::string primitive_block = MakePrimitiveBlock(
      string_table, {pg1, pg2, pg3, pg4}, /*granularity=*/100,
      /*date_granularity=*/1000, /*lat_offset=*/0, /*lon_offset=*/0,
      /*add_unknown_fields=*/true);
  pbf_data.append(MakeFileBlock("OSMData", primitive_block, /*compress=*/true));

  VsiMemTempWrapper wrapper(path, pbf_data);
  ParserCapture capture;
  OsmContextUniquePtr ctxt(OSM_Open(path.c_str(), NotifyNodesCbk, NotifyWayCbk,
                                    NotifyRelationCbk, NotifyBoundsCbk,
                                    &capture));
  ASSERT_NE(nullptr, ctxt);

  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_EOF, OSM_ProcessBlock(ctxt.get()));

  EXPECT_EQ(4, capture.bounds.size());
  EXPECT_DOUBLE_EQ(2.0, capture.bounds[0]);
  EXPECT_DOUBLE_EQ(48.0, capture.bounds[1]);
  EXPECT_DOUBLE_EQ(3.0, capture.bounds[2]);
  EXPECT_DOUBLE_EQ(49.0, capture.bounds[3]);

  EXPECT_EQ(3, capture.node_count);
  EXPECT_THAT(capture.node_ids, ElementsAre(101, 102, 201));
  EXPECT_EQ(1, capture.way_count);
  EXPECT_THAT(capture.way_ids, ElementsAre(301));
  EXPECT_EQ(1, capture.relation_count);
  EXPECT_THAT(capture.relation_ids, ElementsAre(401));

  EXPECT_GT(OSM_GetBytesRead(ctxt.get()), 0);

  // Test OSM_ResetReading
  OSM_ResetReading(ctxt.get());
  capture = ParserCapture();
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_EOF, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(3, capture.node_count);
  EXPECT_EQ(1, capture.way_count);
  EXPECT_EQ(1, capture.relation_count);
}

TEST(OsmParserTest, PbfDefaultNullCallbacks) {
  CPLConfigOptionSetter threads_setter("GDAL_NUM_THREADS", "1",
                                       /*bSetOnlyIfUndefined=*/false);
  const std::string path = "/vsimem/null_cbks.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0),
                                /*compress=*/false));
  std::vector<std::string> strings = {"", "k", "v"};
  std::string string_table = MakeStringTable(strings);
  std::vector<DenseNodeData> dense_nodes = {{.id = 1,
                                             .lat = 10.0,
                                             .lon = 20.0,
                                             .version = 1,
                                             .timestamp = 100,
                                             .changeset = 1,
                                             .uid = 1,
                                             .user_sid = 1,
                                             .tags = {{1, 2}}}};
  std::string pg1;
  AppendBytesField(pg1, 2, MakeDenseNodes(dense_nodes));
  std::string pg2;
  AppendBytesField(pg2, 3, MakeWay(2, {1}, {{1, 2}}));
  std::string pg3;
  std::vector<RelationMemberData> members = {{1, 1, MEMBER_NODE}};
  AppendBytesField(pg3, 4, MakeRelation(3, members, {{1, 2}}));
  pbf_data.append(MakeFileBlock(
      "OSMData", MakePrimitiveBlock(string_table, {pg1, pg2, pg3}),
      /*compress=*/false));

  VsiMemTempWrapper wrapper(path, pbf_data);
  // Pass nullptr for all callbacks
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_EOF, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, PbfWorkerThreadPoolDecompression) {
  // Test with GDAL_NUM_THREADS = 2 to exercise CPLWorkerThreadPool branch
  CPLConfigOptionSetter threads_setter("GDAL_NUM_THREADS", "2",
                                       /*bSetOnlyIfUndefined=*/false);

  const std::string path = "/vsimem/worker_threads.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0),
                                /*compress=*/true));

  std::vector<std::string> strings = {"", "name", "node"};
  std::string string_table = MakeStringTable(strings);

  // Create two compressed OSMData blocks followed by an uncompressed one
  std::string pg1;
  AppendBytesField(pg1, 2,
                   MakeDenseNodes({{.id = 1,
                                    .lat = 10.0,
                                    .lon = 10.0,
                                    .timestamp = 1,
                                    .changeset = 1,
                                    .uid = 1,
                                    .user_sid = 1,
                                    .tags = {{1, 2}}}}));
  pbf_data.append(MakeFileBlock("OSMData",
                                MakePrimitiveBlock(string_table, {pg1}),
                                /*compress=*/true));

  std::string pg2;
  AppendBytesField(pg2, 2,
                   MakeDenseNodes({{.id = 2,
                                    .lat = 20.0,
                                    .lon = 20.0,
                                    .timestamp = 1,
                                    .changeset = 1,
                                    .uid = 1,
                                    .user_sid = 1,
                                    .tags = {{1, 2}}}}));
  pbf_data.append(MakeFileBlock("OSMData",
                                MakePrimitiveBlock(string_table, {pg2}),
                                /*compress=*/true));

  // Also include a raw (uncompressed) OSMData block to exercise
  // line 1749 in ReadBlob (flush queued jobs before processing raw data)
  std::string pg3;
  AppendBytesField(pg3, 2,
                   MakeDenseNodes({{.id = 3,
                                    .lat = 30.0,
                                    .lon = 30.0,
                                    .timestamp = 1,
                                    .changeset = 1,
                                    .uid = 1,
                                    .user_sid = 1,
                                    .tags = {{1, 2}}}}));
  pbf_data.append(MakeFileBlock("OSMData",
                                MakePrimitiveBlock(string_table, {pg3}),
                                /*compress=*/false));

  VsiMemTempWrapper wrapper(path, pbf_data);
  ParserCapture capture;
  OsmContextUniquePtr ctxt(OSM_Open(path.c_str(), NotifyNodesCbk, NotifyWayCbk,
                                    NotifyRelationCbk, NotifyBoundsCbk,
                                    &capture));
  ASSERT_NE(nullptr, ctxt);

  EXPECT_EQ(OSM_EOF, DrainBlocks(ctxt.get()));
  EXPECT_EQ(3, capture.node_count);
  EXPECT_THAT(capture.node_ids, ElementsAre(1, 2, 3));
}

TEST(OsmParserTest, PbfLargeStringTableAndReallocations) {
  const std::string path = "/vsimem/realloc.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0),
                                /*compress=*/false));

  // > 1100 strings to trigger panStrOff reallocation
  std::vector<std::string> strings;
  strings.reserve(2100);
  strings.push_back("");
  for (int i = 1; i <= 2050; ++i) {
    strings.push_back("tag_str_" + std::to_string(i));
  }
  std::string string_table = MakeStringTable(strings);

  // Dense nodes with 300 tags
  std::vector<std::pair<uint32_t, uint32_t>> dense_tags;
  dense_tags.reserve(300);
  for (uint32_t i = 1; i <= 300; ++i) {
    dense_tags.push_back({i, i});
  }
  std::vector<DenseNodeData> dense_nodes = {
      {/*id=*/10, /*lat=*/10.0, /*lon=*/10.0, /*version=*/1, /*timestamp=*/10,
       /*changeset=*/1, /*uid=*/1, /*user_sid=*/1, dense_tags}};
  std::string pg1;
  AppendBytesField(pg1, 2, MakeDenseNodes(dense_nodes));

  // Standard Node with 0 tags
  std::string node_zero_tags =
      MakeNode(/*id=*/20, /*lat=*/20.0, /*lon=*/20.0, /*tags=*/{});
  std::string pg2;
  AppendBytesField(pg2, 1, node_zero_tags);

  // Way with 700 tags and 20,000 refs to trigger pasTags and panNodeRefs
  // realloc
  std::vector<std::pair<uint32_t, uint32_t>> way_tags;
  way_tags.reserve(700);
  for (uint32_t i = 1; i <= 700; ++i) {
    way_tags.push_back({i, i});
  }
  std::vector<int64_t> way_refs(20000, 10);
  std::string way_payload = MakeWay(/*id=*/30, way_refs, way_tags);
  std::string pg3;
  AppendBytesField(pg3, 3, way_payload);

  // Way with 0 tags and refs
  std::string way_zero_tags =
      MakeWay(/*id=*/31, /*node_refs=*/{10, 20}, /*tags=*/{});
  std::string pg4;
  AppendBytesField(pg4, 3, way_zero_tags);

  // Relation with 1500 tags and 2500 members to trigger pasTags and pasMembers
  // realloc
  std::vector<std::pair<uint32_t, uint32_t>> rel_tags;
  rel_tags.reserve(1500);
  for (uint32_t i = 1; i <= 1500; ++i) {
    rel_tags.push_back({i, i});
  }
  std::vector<RelationMemberData> rel_members;
  rel_members.reserve(2500);
  for (int i = 0; i < 2500; ++i) {
    rel_members.push_back({10, 1, MEMBER_NODE});
  }
  std::string rel_payload = MakeRelation(/*id=*/40, rel_members, rel_tags);
  std::string pg5;
  AppendBytesField(pg5, 4, rel_payload);

  // Relation with 0 tags and MEMBER_RELATION
  std::vector<RelationMemberData> rel_members2 = {
      {/*memid=*/40, /*role_sid=*/1, MEMBER_RELATION}};
  std::string rel_zero_tags =
      MakeRelation(/*id=*/41, rel_members2, /*tags=*/{});
  std::string pg6;
  AppendBytesField(pg6, 4, rel_zero_tags);

  // Custom granularity, date_granularity, lat_offset, lon_offset
  std::string pblock = MakePrimitiveBlock(
      string_table, {pg1, pg2, pg3, pg4, pg5, pg6}, /*granularity=*/10,
      /*date_granularity=*/500, /*lat_offset=*/1000, /*lon_offset=*/2000);
  pbf_data.append(MakeFileBlock("OSMData", pblock, /*compress=*/false));

  VsiMemTempWrapper wrapper(path, pbf_data);
  ParserCapture capture;
  OsmContextUniquePtr ctxt(OSM_Open(path.c_str(), NotifyNodesCbk, NotifyWayCbk,
                                    NotifyRelationCbk, NotifyBoundsCbk,
                                    &capture));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_EOF, DrainBlocks(ctxt.get()));
  EXPECT_EQ(2, capture.node_count);
  EXPECT_EQ(2, capture.way_count);
  EXPECT_EQ(2, capture.relation_count);
}

TEST(OsmParserTest, PbfUnsupportedRequiredFeatureFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/unsupported_feature.pbf";
  std::string header;
  AppendBytesField(header, 4 /* REQUIRED_FEATURES */,
                   "UnsupportedFutureFeatureXYZ");
  std::string pbf_data = MakeFileBlock("OSMHeader", header);

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, PbfInvalidCoordinatesFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/invalid_coords.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0)));

  // Node with lat = 95 degrees
  std::vector<std::string> strings = {""};
  std::string string_table = MakeStringTable(strings);
  std::string pg;
  AppendBytesField(pg, 1, MakeNode(1, 95.0, 0.0, {}));
  pbf_data.append(
      MakeFileBlock("OSMData", MakePrimitiveBlock(string_table, {pg})));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));     // OSMHeader
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));  // OSMData
}

TEST(OsmParserTest, PbfDenseNodesInvalidCoordinatesFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/dense_invalid_coords.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0)));

  std::vector<std::string> strings = {""};
  std::string string_table = MakeStringTable(strings);
  std::string pg;
  AppendBytesField(pg, 2,
                   MakeDenseNodes({{.id = 1, .lat = 0.0, .lon = 200.0}}));
  pbf_data.append(
      MakeFileBlock("OSMData", MakePrimitiveBlock(string_table, {pg})));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));     // OSMHeader
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));  // OSMData
}

TEST(OsmParserTest, PbfInvalidGranularityFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/invalid_granularity.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0)));

  std::string string_table = MakeStringTable({""});
  std::string pblock = MakePrimitiveBlock(string_table, {}, /*granularity=*/0);
  pbf_data.append(MakeFileBlock("OSMData", pblock));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));     // OSMHeader
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));  // OSMData
}

TEST(OsmParserTest, PbfDuplicateStringTableFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/dup_string_table.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0)));

  std::string string_table = MakeStringTable({""});
  std::string block;
  AppendBytesField(block, 1 /* STRINGTABLE */, string_table);
  AppendBytesField(block, 1 /* DUPLICATE STRINGTABLE */, string_table);
  pbf_data.append(MakeFileBlock("OSMData", block));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));     // OSMHeader
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));  // OSMData
}

TEST(OsmParserTest, PbfStringIndexOutOfBoundsFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/oob_string.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0)));

  std::string string_table = MakeStringTable({""});  // size 1
  std::string pg;
  // Node references string index 99 which does not exist
  AppendBytesField(pg, 1, MakeNode(1, 10.0, 10.0, {{99, 99}}));
  pbf_data.append(
      MakeFileBlock("OSMData", MakePrimitiveBlock(string_table, {pg})));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, PbfCorruptZlibBlobFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/corrupt_zlib.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0)));

  std::string blob;
  AppendVarUIntField(blob, 2 /* RAW_SIZE */, 100);
  AppendBytesField(blob, 3 /* ZLIB_DATA */, "NOT_ZLIB_DATA_CORRUPT");
  pbf_data.append(MakeFileBlockFromBlob("OSMData", blob));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));     // Header
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));  // Corrupt zlib
}

TEST(OsmParserTest, PbfExcessiveCompressionRatioFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/ratio_bomb.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0)));

  std::string blob;
  AppendVarUIntField(blob, 2 /* RAW_SIZE */, 10000000);  // 10 MB raw
  AppendBytesField(blob, 3 /* ZLIB_DATA */, "tiny");     // 4 bytes compressed
  pbf_data.append(MakeFileBlockFromBlob("OSMData", blob));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, PbfHeaderExceedsMaxBlobHeaderSizeFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/huge_header.pbf";
  // 65KB header size (> 64KB MAX_BLOB_HEADER_SIZE)
  char len_buf[4] = {0, 1, 0, 1};
  std::string pbf_data =
      MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0));
  pbf_data.append(len_buf, 4);

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, PbfTruncatedHeaderFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/truncated_header.pbf";
  std::string pbf_data =
      MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0));
  // 100-byte header length, but only 10 bytes written
  char len_buf[4] = {0, 0, 0, 100};
  pbf_data.append(len_buf, 4);
  pbf_data.append("0123456789");

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, PbfUnknownBlobTypeFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/unknown_blob_type.pbf";
  std::string pbf_data =
      MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0));
  pbf_data.append(MakeFileBlock("UnknownBlobType", "data"));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, PbfNodeParseErrorBranch) {
  // Test failure branch in ReadNode when VALS appears without KEYS (sNode.nTags
  // == 0)
  WithQuietHandler quiet;
  const std::string path = "/vsimem/node_parse_err.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0)));

  std::string string_table = MakeStringTable({""});
  std::string node;
  AppendVarSIntField(node, 1, 1);
  AppendVarSIntField(node, 8, 100);
  AppendVarSIntField(node, 9, 200);
  // VALS without KEYS triggers line 1022 THROW_OSM_PARSING_EXCEPTION
  AppendBytesField(node, 3 /* NODE_IDX_VALS */, "\x01\x01");

  std::string pg;
  AppendBytesField(pg, 1, node);
  pbf_data.append(
      MakeFileBlock("OSMData", MakePrimitiveBlock(string_table, {pg})));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, PbfRelationMemberTypeInvalidFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/rel_bad_type.pbf";
  std::string pbf_data;
  pbf_data.append(MakeFileBlock("OSMHeader", MakeHeaderBlock(0, 0, 0, 0)));

  std::string string_table = MakeStringTable({"", "role"});
  std::string rel;
  AppendVarUIntField(rel, 1, 10);
  // Member with invalid type 5 (> MEMBER_RELATION)
  std::string roles_sid;
  AppendVarUInt(roles_sid, 1);
  std::string memids;
  AppendVarSInt(memids, 100);
  std::string types;
  types.push_back(5);  // Invalid enum value
  AppendBytesField(rel, 8, roles_sid);
  AppendBytesField(rel, 9, memids);
  AppendBytesField(rel, 10, types);

  std::string pg;
  AppendBytesField(pg, 4, rel);
  pbf_data.append(
      MakeFileBlock("OSMData", MakePrimitiveBlock(string_table, {pg})));

  VsiMemTempWrapper wrapper(path, pbf_data);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_OK, OSM_ProcessBlock(ctxt.get()));
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

// -----------------------------------------------------------------------------
// OSM XML Parser Tests
// -----------------------------------------------------------------------------

TEST(OsmParserTest, XmlHappyPathStandardAndOsmosisBounds) {
  const std::string path = "/vsimem/xml_happy.osm";
  const std::string xml =
      "<?xml version='1.0' encoding='UTF-8'?>\n"
      "<osm version='0.6' generator='test'>\n"
      "  <bounds minlat='10.0' minlon='20.0' maxlat='11.0' maxlon='21.0'/>\n"
      "  <node id='1' lat='10.1' lon='20.1' version='1' changeset='100'\n"
      "        user='alice' uid='10' timestamp='2020-01-01T00:00:00Z'>\n"
      "    <tag k='amenity' v='cafe'/>\n"
      "  </node>\n"
      "  <way id='2' version='2' changeset='101' user='bob' uid='11'\n"
      "       timestamp='2020-01-02T00:00:00Z'>\n"
      "    <nd ref='1'/>\n"
      "    <tag k='highway' v='residential'/>\n"
      "  </way>\n"
      "  <relation id='3' version='3' changeset='102' user='charlie' uid='12'\n"
      "            timestamp='2020-01-03T00:00:00Z'>\n"
      "    <member type='node' ref='1' role='stop'/>\n"
      "    <member type='way' ref='2' role='street'/>\n"
      "    <member type='relation' ref='3' role='sub'/>\n"
      "    <tag k='type' v='route'/>\n"
      "  </relation>\n"
      "</osm>\n";

  VsiMemTempWrapper wrapper(path, xml);
  ParserCapture capture;
  OsmContextUniquePtr ctxt(OSM_Open(path.c_str(), NotifyNodesCbk, NotifyWayCbk,
                                    NotifyRelationCbk, NotifyBoundsCbk,
                                    &capture));
  ASSERT_NE(nullptr, ctxt);

  EXPECT_EQ(OSM_EOF, DrainBlocks(ctxt.get()));

  EXPECT_EQ(4, capture.bounds.size());
  EXPECT_DOUBLE_EQ(20.0, capture.bounds[0]);  // xmin / minlon
  EXPECT_DOUBLE_EQ(10.0, capture.bounds[1]);  // ymin / minlat
  EXPECT_DOUBLE_EQ(21.0, capture.bounds[2]);  // xmax / maxlon
  EXPECT_DOUBLE_EQ(11.0, capture.bounds[3]);  // ymax / maxlat

  EXPECT_EQ(1, capture.node_count);
  EXPECT_THAT(capture.node_ids, ElementsAre(1));
  EXPECT_EQ(1, capture.way_count);
  EXPECT_THAT(capture.way_ids, ElementsAre(2));
  EXPECT_EQ(1, capture.relation_count);
  EXPECT_THAT(capture.relation_ids, ElementsAre(3));

  // Test OSM_ResetReading on XML
  OSM_ResetReading(ctxt.get());
  capture = ParserCapture();
  EXPECT_EQ(OSM_EOF, DrainBlocks(ctxt.get()));
  EXPECT_EQ(1, capture.node_count);
  EXPECT_EQ(1, capture.way_count);
  EXPECT_EQ(1, capture.relation_count);
}

TEST(OsmParserTest, XmlOsmosisBoundBoxFormat) {
  const std::string path = "/vsimem/xml_osmosis_bound.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <bound box='10.0,20.0,11.0,21.0'/>\n"
      "  <node id='1' lat='10.5' lon='20.5'/>\n"
      "</osm>\n";

  VsiMemTempWrapper wrapper(path, xml);
  ParserCapture capture;
  OsmContextUniquePtr ctxt(OSM_Open(path.c_str(), NotifyNodesCbk, NotifyWayCbk,
                                    NotifyRelationCbk, NotifyBoundsCbk,
                                    &capture));
  ASSERT_NE(nullptr, ctxt);

  EXPECT_EQ(OSM_EOF, DrainBlocks(ctxt.get()));

  EXPECT_EQ(4, capture.bounds.size());
  EXPECT_DOUBLE_EQ(20.0, capture.bounds[0]);
  EXPECT_DOUBLE_EQ(10.0, capture.bounds[1]);
  EXPECT_DOUBLE_EQ(21.0, capture.bounds[2]);
  EXPECT_DOUBLE_EQ(11.0, capture.bounds[3]);
  EXPECT_EQ(1, capture.node_count);
}

TEST(OsmParserTest, XmlReallocationsTagsAndMembers) {
  const std::string path = "/vsimem/xml_realloc.osm";
  std::string xml = "<osm version='0.6'>\n";

  // Node with > 260 tags to trigger pasTags reallocation (initial 256)
  xml.append("  <node id='1' lat='1.0' lon='2.0'>\n");
  for (int i = 0; i < 270; ++i) {
    xml.append("    <tag k='key_")
        .append(std::to_string(i))
        .append("' v='val_")
        .append(std::to_string(i))
        .append("'/>\n");
  }
  xml.append("  </node>\n");

  // Relation with > 2005 members to trigger pasMembers reallocation (initial
  // 2000)
  xml.append("  <relation id='2'>\n");
  for (int i = 0; i < 2010; ++i) {
    xml.append("    <member type='node' ref='")
        .append(std::to_string(i))
        .append("' role='r'/>\n");
  }
  xml.append("  </relation>\n");
  xml.append("</osm>\n");

  VsiMemTempWrapper wrapper(path, xml);
  ParserCapture capture;
  OsmContextUniquePtr ctxt(OSM_Open(path.c_str(), NotifyNodesCbk, NotifyWayCbk,
                                    NotifyRelationCbk, NotifyBoundsCbk,
                                    &capture));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_EOF, DrainBlocks(ctxt.get()));
  EXPECT_EQ(1, capture.node_count);
  EXPECT_EQ(1, capture.relation_count);
}

TEST(OsmParserTest, XmlTooManyNodeRefsInWayLogsError) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/xml_too_many_refs.osm";
  std::string xml = "<osm version='0.6'>\n  <way id='99'>\n";
  // Initial nNodeRefsAllocated is 10000. Add 10005 refs to trigger warning.
  for (int i = 0; i < 10005; ++i) {
    xml.append("    <nd ref='1'/>\n");
  }
  xml.append("  </way>\n</osm>\n");

  VsiMemTempWrapper wrapper(path, xml);
  ParserCapture capture;
  OsmContextUniquePtr ctxt(OSM_Open(path.c_str(), NotifyNodesCbk, NotifyWayCbk,
                                    NotifyRelationCbk, NotifyBoundsCbk,
                                    &capture));
  ASSERT_NE(nullptr, ctxt);
  CPLErrorReset();
  EXPECT_EQ(OSM_EOF, DrainBlocks(ctxt.get()));
  EXPECT_EQ(1, capture.way_count);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too many nodes referenced"));
}

TEST(OsmParserTest, XmlInvalidCoordinatesLogsError) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/xml_invalid_coords.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='1' lat='999.0' lon='0.0'/>\n"
      "</osm>\n";

  VsiMemTempWrapper wrapper(path, xml);
  ParserCapture capture;
  OsmContextUniquePtr ctxt(OSM_Open(path.c_str(), NotifyNodesCbk, NotifyWayCbk,
                                    NotifyRelationCbk, NotifyBoundsCbk,
                                    &capture));
  ASSERT_NE(nullptr, ctxt);
  CPLErrorReset();
  EXPECT_EQ(OSM_EOF, DrainBlocks(ctxt.get()));
  EXPECT_EQ(0, capture.node_count);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid lon="));
}

TEST(OsmParserTest, XmlSyntaxErrorFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/xml_syntax_err.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='1'></unmatched_tag>\n"
      "</osm>\n";

  VsiMemTempWrapper wrapper(path, xml);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, XmlTooMuchDataWithoutEventFails) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/xml_no_event.osm";
  // 10 buffers of 64KB (640KB) without any event triggers line 2623
  std::string xml = "<osm version='0.6'>\n<!-- ";
  xml.append(660 * 1024, ' ');
  xml.append(" -->\n</osm>\n");

  VsiMemTempWrapper wrapper(path, xml);
  OsmContextUniquePtr ctxt(
      OSM_Open(path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ctxt);
  EXPECT_EQ(OSM_ERROR, OSM_ProcessBlock(ctxt.get()));
}

TEST(OsmParserTest, XmlStringBufferTooSmallLogsError) {
  WithQuietHandler quiet;
  const std::string path = "/vsimem/xml_str_overflow.osm";
  // Cumulative string length in a single element > 1MB (nStrAllocated =
  // 1024*1024)
  std::string xml =
      "<osm version='0.6'>\n  <node id='1' lat='0.0' lon='0.0'>\n";
  std::string big_val(50000, 'x');
  for (int i = 0; i < 25; ++i) {  // 25 * 50,000 = 1,250,000 bytes > 1MB
    xml.append("    <tag k='k_")
        .append(std::to_string(i))
        .append("' v='")
        .append(big_val)
        .append("'/>\n");
  }
  xml.append("  </node>\n</osm>\n");

  VsiMemTempWrapper wrapper(path, xml);
  ParserCapture capture;
  OsmContextUniquePtr ctxt(OSM_Open(path.c_str(), NotifyNodesCbk, NotifyWayCbk,
                                    NotifyRelationCbk, NotifyBoundsCbk,
                                    &capture));
  ASSERT_NE(nullptr, ctxt);
  CPLErrorReset();
  EXPECT_EQ(OSM_EOF, DrainBlocks(ctxt.get()));
  EXPECT_EQ(1, capture.node_count);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("String buffer too small"));
}

// TODO: b/335317901 - Cover ReadBlobHeader malformed-field exceptions,
// MAX_BLOB_SIZE, and N_MAX_JOBS accumulation limits.

}  // namespace
}  // namespace autotest2
