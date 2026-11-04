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
// White-box tests for HFAEntry tree navigation, field access, and binary
// serialization (hfa_p.h / hfaentry.cpp).
//
// Note: White-box tests depend on hfa_p.h internals, assume little-endian host
// byte order (HFAStandard is a no-op on CPL_LSB), and use POSIX open_memstream.
//
// See also:
//   https://gdal.org/drivers/raster/hfa.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfaentry.cpp

#include <cstdio>
#include <cstring>
#include <memory>
#include <span>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/hfa/hfa.h"
#include "third_party/gdal/frmts/hfa/hfa_p.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/hfaentry_test";

// Serialized Ehfa_Entry header field byte offsets (hfaentry.cpp HFAEntry::New):
//   0..3  nNextPos (GUInt32)
//   4..7  nPrevPos (GUInt32)
//   8..11 nParentPos (GUInt32)
//  12..15 nChildPos (GUInt32)
//  16..19 nDataPos (GUInt32)
//  20..23 nDataSize (GUInt32)
//  24..87 szName (64 bytes)
//  88..119 szType (32 bytes)
constexpr size_t kEntryChildPos = 12;
constexpr size_t kEntryDataPos = 16;
constexpr size_t kEntryDataSize = 20;
static_assert(sizeof(GUInt32) == 4);
static_assert(kEntryChildPos < kEntryDataPos && kEntryDataPos < kEntryDataSize);

struct HfaCloser {
  void operator()(HFAHandle hfa) const {
    if (hfa != nullptr) {
      EXPECT_EQ(CE_None, HFAClose(hfa));
    }
  }
};

using HfaPtr = std::unique_ptr<hfainfo, HfaCloser>;

template <typename T>
void WriteHfaLe(GByte* dst, size_t offset, T val) {
  HFAStandard(static_cast<int>(sizeof(T)), &val);
  std::memcpy(dst + offset, &val, sizeof(T));
}

template <typename Func>
std::string CaptureMemStream(Func&& dump_fn) {
  char* buf = nullptr;
  size_t size = 0;
  std::unique_ptr<char, FreeDeleter> owned_buf;
  FILE* fp = open_memstream(&buf, &size);
  if (fp == nullptr) {
    ADD_FAILURE() << "open_memstream failed";
    return "";
  }
  dump_fn(fp);
  EXPECT_EQ(0, std::fclose(fp));
  owned_buf.reset(buf);
  return owned_buf == nullptr ? "" : std::string(owned_buf.get(), size);
}

std::vector<GByte> ReadVsiMemBytes(const char* path) {
  vsi_l_offset length = 0;
  GByte* raw = VSIGetMemFileBuffer(path, &length, FALSE);
  if (raw == nullptr || length == 0) {
    return {};
  }
  return std::vector<GByte>(raw, raw + static_cast<size_t>(length));
}

void WriteVsiMemBytes(const char* path, std::span<const GByte> bytes) {
  std::unique_ptr<GByte, decltype(&VSIFree)> copy(
      static_cast<GByte*>(VSIMalloc(bytes.size())), VSIFree);
  ASSERT_NE(nullptr, copy);
  std::memcpy(copy.get(), bytes.data(), bytes.size());
  EXPECT_EQ(0,
            VSIFCloseL(VSIFileFromMemBuffer(path, copy.release(), bytes.size(),
                                            /*bTakeOwnership=*/TRUE)));
}

void AddCustomType(HFAHandle hfa, const char* defn) {
  auto type = std::make_unique<HFAType>();
  ASSERT_NE(nullptr, type->Initialize(defn));
  ASSERT_TRUE(type->CompleteDefn(hfa->poDictionary));
  hfa->poDictionary->AddType(type.release());
}

TEST(HfaEntryTest, InMemoryTreeCreationAndFieldAccess) {
  constexpr char kPath[] = "/vsimem/hfaentry_test/tree_test.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  HfaPtr hfa(HFACreateLL(kPath));
  ASSERT_NE(nullptr, hfa);

  // 1. Stack-constructed root-like entry with poParent == nullptr.
  HFAEntry standalone(hfa.get(), "StandaloneNode", "Esta_Statistics", nullptr);
  EXPECT_STREQ("StandaloneNode", standalone.GetName());
  EXPECT_STREQ("Esta_Statistics", standalone.GetType());
  EXPECT_EQ(0u, standalone.GetFilePos());
  EXPECT_NE(nullptr, standalone.GetTypeObject());

  // 2. Create child and sibling entries under hfa->poRoot.
  HFAEntry* layer =
      HFAEntry::New(hfa.get(), "Layer_1", "Eimg_Layer", hfa->poRoot);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(CE_None, layer->SetIntField("width", 128));
  EXPECT_EQ(CE_None, layer->SetIntField("height", 64));
  EXPECT_EQ(CE_None, layer->SetStringField("layerType", "athematic"));
  EXPECT_EQ(CE_None, layer->SetStringField("pixelType", "u8"));
  EXPECT_EQ(CE_None, layer->SetIntField("blockWidth", 32));
  EXPECT_EQ(CE_None, layer->SetIntField("blockHeight", 32));

  // Second and third siblings exercise the poPrev->poNext traversal loop.
  HFAEntry* stats =
      HFAEntry::New(hfa.get(), "Statistics", "Esta_Statistics", layer);
  ASSERT_NE(nullptr, stats);
  EXPECT_EQ(CE_None, stats->SetDoubleField("minimum", 1.25));
  EXPECT_EQ(CE_None, stats->SetDoubleField("maximum", 254.75));
  EXPECT_EQ(CE_None, stats->SetDoubleField("mean", 100.5));

  HFAEntry* ext =
      HFAEntry::New(hfa.get(), "ExternalRaster", "ImgExternalRaster", layer);
  ASSERT_NE(nullptr, ext);
  ASSERT_NE(nullptr, ext->MakeData(64));
  EXPECT_EQ(CE_None, ext->SetIntField("layerStackValidFlagsOffset[0]", 0x1234));
  EXPECT_EQ(CE_None, ext->SetIntField("layerStackValidFlagsOffset[1]", 0x56));

  HFAEntry* third =
      HFAEntry::New(hfa.get(), "ThirdChild", "Esta_Statistics", layer);
  ASSERT_NE(nullptr, third);
  ASSERT_NE(nullptr, third->MakeData());

  // 3. Flush to disk and reopen to read back from file.
  ASSERT_EQ(CE_None, hfa->poRoot->FlushToDisk());
  EXPECT_NE(0u, layer->GetFilePos());
  EXPECT_NE(0u, layer->GetDataPos());
  EXPECT_GT(layer->GetDataSize(), 0u);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);

  HFAEntry* found_layer = hfa->poRoot->GetNamedChild("Layer_1");
  ASSERT_NE(nullptr, found_layer);
  EXPECT_NE(nullptr, found_layer->GetData());

  CPLErr err = CE_Failure;
  EXPECT_EQ(128, found_layer->GetIntField("width", &err));
  EXPECT_EQ(CE_None, err);
  EXPECT_EQ(64, found_layer->GetIntField("height"));
  EXPECT_STREQ("athematic", found_layer->GetStringField("layerType", &err));
  EXPECT_EQ(CE_None, err);

  // 4. Nested path via GetNamedChild and node:field syntax.
  HFAEntry* found_stats = hfa->poRoot->GetNamedChild("Layer_1.Statistics");
  ASSERT_NE(nullptr, found_stats);
  EXPECT_DOUBLE_EQ(1.25, found_stats->GetDoubleField("minimum", &err));
  EXPECT_EQ(CE_None, err);
  EXPECT_DOUBLE_EQ(254.75, found_stats->GetDoubleField("maximum", &err));
  EXPECT_EQ(CE_None, err);
  EXPECT_EQ(1, found_stats->GetFieldCount("minimum"));

  EXPECT_EQ(128, found_layer->GetIntField("Statistics:width", &err));
  EXPECT_EQ(CE_None, err);
  EXPECT_EQ(1, found_layer->GetFieldCount("Statistics:width"));

  // 5. GetBigIntField on 2-element LONG array.
  HFAEntry* found_ext = hfa->poRoot->GetNamedChild("Layer_1.ExternalRaster");
  ASSERT_NE(nullptr, found_ext);
  constexpr GIntBig kExpectedBig =
      static_cast<GIntBig>(0x1234) + (static_cast<GIntBig>(0x56) << 32);
  EXPECT_EQ(kExpectedBig,
            found_ext->GetBigIntField("layerStackValidFlagsOffset", &err));
  EXPECT_EQ(CE_None, err);

  // 6. FindChildren by name, by type, and by both.
  EXPECT_EQ(1u, hfa->poRoot->FindChildren("Statistics", nullptr).size());
  EXPECT_EQ(2u, hfa->poRoot->FindChildren(nullptr, "Esta_Statistics").size());
  EXPECT_EQ(1u,
            hfa->poRoot->FindChildren("ThirdChild", "Esta_Statistics").size());
  EXPECT_EQ(nullptr, hfa->poRoot->GetNamedChild("Layer_1.DoesNotExist"));
  EXPECT_EQ(nullptr, hfa->poRoot->GetNamedChild("DoesNotExist.Child"));
}

TEST(HfaEntryTest, ErrorPathsAndInvalidLookups) {
  constexpr char kPath[] = "/vsimem/hfaentry_test/errors.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  HfaPtr hfa(HFACreateLL(kPath));
  ASSERT_NE(nullptr, hfa);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);

  // 1. Setup valid child entries before error checks.
  HFAEntry* stats =
      HFAEntry::New(hfa.get(), "Stats", "Esta_Statistics", hfa->poRoot);
  ASSERT_NE(nullptr, stats);
  EXPECT_EQ(CE_None, stats->SetDoubleField("minimum", 5.0));

  HFAEntry* unknown = HFAEntry::New(hfa.get(), "UnknownNode",
                                    "NoSuchDictionaryType", hfa->poRoot);
  ASSERT_NE(nullptr, unknown);

  HFAEntry* unknown_with_data =
      HFAEntry::New(hfa.get(), "UnknownData", "Esta_Statistics", hfa->poRoot);
  ASSERT_NE(nullptr, unknown_with_data);
  ASSERT_NE(nullptr, unknown_with_data->MakeData());
  ASSERT_EQ(CE_None, hfa->poRoot->FlushToDisk());

  const std::vector<GByte> file_bytes = ReadVsiMemBytes(kPath);
  ASSERT_GT(file_bytes.size(), 100u);

  // 2. Missing fields, unknown dictionary types, and truncated header reads.
  //    Serialized Ehfa_Entry is 120 bytes (24 header + 64 szName + 32 szType):
  //    offset file_size - 30 fails inside szType, file_size - 90 inside szName.
  WithQuietHandler quiet;
  CPLErrorReset();

  CPLErr err = CE_None;
  EXPECT_EQ(0, hfa->poRoot->GetIntField("nonexistent", &err));
  EXPECT_EQ(CE_Failure, err);

  err = CE_None;
  EXPECT_DOUBLE_EQ(0.0, hfa->poRoot->GetDoubleField("nonexistent", &err));
  EXPECT_EQ(CE_Failure, err);

  err = CE_None;
  EXPECT_EQ(nullptr, hfa->poRoot->GetStringField("nonexistent", &err));
  EXPECT_EQ(CE_Failure, err);

  err = CE_None;
  EXPECT_EQ(0, hfa->poRoot->GetBigIntField("nonexistent", &err));
  EXPECT_EQ(CE_Failure, err);

  err = CE_None;
  EXPECT_EQ(0, stats->GetBigIntField("minimum", &err));
  EXPECT_EQ(CE_Failure, err);

  EXPECT_EQ(-1, hfa->poRoot->GetFieldCount("MissingChild:field"));
  EXPECT_EQ(-1, hfa->poRoot->GetFieldCount("no_such_field"));
  EXPECT_EQ(CE_Failure, hfa->poRoot->SetIntField("MissingChild:field", 1));
  EXPECT_EQ(CE_Failure, hfa->poRoot->SetDoubleField("MissingChild:field", 1.0));
  EXPECT_EQ(CE_Failure,
            hfa->poRoot->SetStringField("MissingChild:field", "val"));

  EXPECT_EQ(nullptr, unknown->MakeData(32));
  EXPECT_EQ(CE_Failure, unknown->SetIntField("foo", 1));

  EXPECT_EQ(
      nullptr,
      HFAEntry::New(hfa.get(), static_cast<GUInt32>(file_bytes.size() + 100),
                    nullptr, nullptr));
  EXPECT_EQ(nullptr, HFAEntry::New(hfa.get(),
                                   static_cast<GUInt32>(file_bytes.size() - 30),
                                   nullptr, nullptr));
  EXPECT_EQ(nullptr, HFAEntry::New(hfa.get(),
                                   static_cast<GUInt32>(file_bytes.size() - 90),
                                   nullptr, nullptr));
  CPLErrorReset();
}

TEST(HfaEntryTest, ResizeFlushedEntryRemoveAndDestroyAndDump) {
  constexpr char kPath[] = "/vsimem/hfaentry_test/resize_remove.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  HfaPtr hfa(HFACreateLL(kPath));
  ASSERT_NE(nullptr, hfa);

  // 1. Create flushed sibling/child hierarchy and resize middle entry.
  HFAEntry* first =
      HFAEntry::New(hfa.get(), "First", "Esta_Statistics", hfa->poRoot);
  HFAEntry* middle =
      HFAEntry::New(hfa.get(), "Middle", "Emif_String", hfa->poRoot);
  HFAEntry* last =
      HFAEntry::New(hfa.get(), "Last", "Esta_Statistics", hfa->poRoot);
  ASSERT_NE(nullptr, first);
  ASSERT_NE(nullptr, middle);
  ASSERT_NE(nullptr, last);

  HFAEntry* grandchild =
      HFAEntry::New(hfa.get(), "GrandChild", "Esta_Statistics", middle);
  ASSERT_NE(nullptr, grandchild);
  ASSERT_NE(nullptr, first->MakeData());
  ASSERT_NE(nullptr, middle->MakeData(16));
  ASSERT_NE(nullptr, last->MakeData());
  ASSERT_NE(nullptr, grandchild->MakeData());

  ASSERT_EQ(CE_None, hfa->poRoot->FlushToDisk());
  ASSERT_NE(0u, middle->GetFilePos());

  ASSERT_NE(nullptr, middle->MakeData(64));
  EXPECT_EQ(0u, middle->GetFilePos());
  EXPECT_EQ(0u, middle->GetDataPos());
  ASSERT_EQ(CE_None, hfa->poRoot->FlushToDisk());
  EXPECT_NE(0u, middle->GetFilePos());

  // 2. DumpFieldValues with nullptr prefix, explicit prefix, and empty data.
  const std::string dump_text = CaptureMemStream([&](FILE* fp) {
    first->DumpFieldValues(fp, nullptr);
    first->DumpFieldValues(fp, "  ");
    hfa->poRoot->DumpFieldValues(fp, "root: ");
  });
  EXPECT_THAT(dump_text, HasSubstr("minimum"));

  // 3. RemoveAndDestroy across all linked-list neighbor configurations.
  EXPECT_EQ(CE_None, middle->RemoveAndDestroy());
  EXPECT_EQ(CE_None, last->RemoveAndDestroy());
  EXPECT_EQ(CE_None, first->RemoveAndDestroy());
  EXPECT_EQ(nullptr, hfa->poRoot->GetChild());

  HFAEntry* a = HFAEntry::New(hfa.get(), "A", "Esta_Statistics", hfa->poRoot);
  HFAEntry* b = HFAEntry::New(hfa.get(), "B", "Esta_Statistics", hfa->poRoot);
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  a->SetName("ARenamed");
  EXPECT_STREQ("ARenamed", a->GetName());
  EXPECT_EQ(CE_None, a->RemoveAndDestroy());
  EXPECT_EQ(b, hfa->poRoot->GetChild());

  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaEntryTest, FindChildrenRecursionLimit) {
  constexpr char kPath[] = "/vsimem/hfaentry_test/recursion.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  HfaPtr hfa(HFACreateLL(kPath));
  ASSERT_NE(nullptr, hfa);

  // FindChildren aborts when nRecLevel == 50 (hfaentry.cpp); 52 levels triggers
  // the limit and truncates unwinding ancestor results to just root's 1 child.
  HFAEntry* curr = hfa->poRoot;
  for (int i = 0; i < 52; ++i) {
    curr = HFAEntry::New(hfa.get(), "DeepNode", "Esta_Statistics", curr);
    ASSERT_NE(nullptr, curr);
  }

  WithQuietHandler quiet;
  CPLErrorReset();
  const std::vector<HFAEntry*> results =
      hfa->poRoot->FindChildren("DeepNode", nullptr);
  EXPECT_EQ(1u, results.size());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("recursion detected"));
  CPLErrorReset();
}

TEST(HfaEntryTest, CorruptPointersLoopsAndWriteFailures) {
  constexpr char kPath[] = "/vsimem/hfaentry_test/corrupt.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Setup 2 flushed child entries with data.
  HfaPtr hfa(HFACreateLL(kPath));
  ASSERT_NE(nullptr, hfa);

  HFAEntry* child1 =
      HFAEntry::New(hfa.get(), "Child1", "Esta_Statistics", hfa->poRoot);
  HFAEntry* child2 =
      HFAEntry::New(hfa.get(), "Child2", "Esta_Statistics", hfa->poRoot);
  ASSERT_NE(nullptr, child1);
  ASSERT_NE(nullptr, child2);
  ASSERT_NE(nullptr, child1->MakeData());
  ASSERT_NE(nullptr, child2->MakeData());
  ASSERT_EQ(CE_None, hfa->poRoot->FlushToDisk());

  const GUInt32 child1_pos = child1->GetFilePos();
  const GUInt32 child2_pos = child2->GetFilePos();
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 2. Sibling cycle detection in GetNext() + FlushToDisk() failure in "rb"
  //    mode.
  std::vector<GByte> bytes = ReadVsiMemBytes(kPath);
  ASSERT_GT(bytes.size(), child2_pos + 24);
  WriteHfaLe(bytes.data(), child2_pos, child1_pos);
  WriteVsiMemBytes(kPath, bytes);

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  HFAEntry* c1 = hfa->poRoot->GetChild();
  ASSERT_NE(nullptr, c1);
  HFAEntry* c2 = c1->GetNext();
  ASSERT_NE(nullptr, c2);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, c2->GetNext());
    c1->MarkDirty();
    EXPECT_EQ(CE_Failure, hfa->poRoot->FlushToDisk());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 3. Patch child2's nNextPos past EOF and child1's nDataPos to EOF.
  WriteHfaLe(bytes.data(), child2_pos,
             static_cast<GUInt32>(bytes.size() + 500));
  WriteHfaLe(bytes.data(), child1_pos + kEntryDataPos,
             static_cast<GUInt32>(bytes.size()));
  WriteVsiMemBytes(kPath, bytes);

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  c1 = hfa->poRoot->GetChild();
  ASSERT_NE(nullptr, c1);
  c2 = c1->GetNext();
  ASSERT_NE(nullptr, c2);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(nullptr, c1->GetData());
    EXPECT_EQ(nullptr, c2->GetNext());
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 4. Patch child1's nDataSize to 0xFFFFFFFF (> INT_MAX - 1) and nChildPos
  //    past EOF.
  WriteHfaLe(bytes.data(), child1_pos + kEntryDataSize, GUInt32{0xFFFFFFFFu});
  WriteHfaLe(bytes.data(), child1_pos + kEntryChildPos,
             static_cast<GUInt32>(bytes.size() + 500));
  WriteVsiMemBytes(kPath, bytes);

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  c1 = hfa->poRoot->GetChild();
  ASSERT_NE(nullptr, c1);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, c1->GetData());
    EXPECT_EQ(nullptr, c1->GetChild());
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaEntryTest, BuildEntryFromMIFObjectValidAndInvalid) {
  constexpr char kPath[] = "/vsimem/hfaentry_test/mif.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  HfaPtr hfa(HFACreateLL(kPath));
  ASSERT_NE(nullptr, hfa);

  ASSERT_NO_FATAL_FAILURE(
      AddCustomType(hfa.get(),
                    "{1:oEmif_String,type,0:pcMIFDictionary,0:pCMIFObject,}"
                    "TestMIFContainer,"));
  ASSERT_NO_FATAL_FAILURE(
      AddCustomType(hfa.get(), "{1:oTestMIFContainer,sub,}TestMIFWrapper,"));

  HFAEntry* container =
      HFAEntry::New(hfa.get(), "MifNode", "TestMIFWrapper", hfa->poRoot);
  ASSERT_NE(nullptr, container);
  constexpr int kBufSize = 256;
  constexpr size_t kMifPrefixOffset = 48;
  GByte* data = container->MakeData(kBufSize);
  ASSERT_NE(nullptr, data);
  std::memset(data, 0, kBufSize);

  // 1. Populate valid embedded MIFObject payload right after sub.type (8B
  //    header + 9B "InnerMIF\0" = 17B) and sub.MIFDictionary (8B header + 23B
  //    "{1:lanswer,}InnerMIF,.\0" = 31B), putting the sub.MIFObject (size,
  //    offset) pointer at offset 48 and its 4-byte payload at offset 56.
  ASSERT_EQ(CE_None, container->SetStringField("sub.type.string", "InnerMIF"));
  ASSERT_EQ(CE_None, container->SetStringField("sub.MIFDictionary",
                                               "{1:lanswer,}InnerMIF,."));
  WriteHfaLe(data, kMifPrefixOffset, GInt32{4});
  WriteHfaLe(data, kMifPrefixOffset + 4, GInt32{56});
  WriteHfaLe(data, 56, GInt32{42});

  std::unique_ptr<HFAEntry> mif_entry(
      HFAEntry::BuildEntryFromMIFObject(container, "sub"));
  ASSERT_NE(nullptr, mif_entry);
  EXPECT_EQ(42, mif_entry->GetIntField("answer"));

  // 2. Missing MIFDictionary, type.string, zero-length MIFObject, and invalid
  //    size prefixes.
  HFAEntry* empty_mif =
      HFAEntry::New(hfa.get(), "EmptyMIF", "TestMIFWrapper", hfa->poRoot);
  ASSERT_NE(nullptr, empty_mif);
  GByte* empty_mif_data = empty_mif->MakeData(128);
  ASSERT_NE(nullptr, empty_mif_data);
  std::memset(empty_mif_data, 0, 128);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              HFAEntry::BuildEntryFromMIFObject(container, "nonexistent"));
    EXPECT_EQ(nullptr, HFAEntry::BuildEntryFromMIFObject(empty_mif, "sub"));
    CPLErrorReset();
  }

  EXPECT_EQ(CE_None, empty_mif->SetStringField("sub.MIFDictionary",
                                               "{1:lanswer,}InnerMIF,."));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, HFAEntry::BuildEntryFromMIFObject(empty_mif, "sub"));
    CPLErrorReset();
  }

  EXPECT_EQ(CE_None, empty_mif->SetStringField("sub.type.string", "InnerMIF"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, HFAEntry::BuildEntryFromMIFObject(empty_mif, "sub"));

    WriteHfaLe(data, kMifPrefixOffset, GInt32{kBufSize});
    EXPECT_EQ(nullptr, HFAEntry::BuildEntryFromMIFObject(container, "sub"));

    WriteHfaLe(data, kMifPrefixOffset, GInt32{0});
    EXPECT_EQ(nullptr, HFAEntry::BuildEntryFromMIFObject(container, "sub"));
    CPLErrorReset();
  }
}

// TODO: b/335317901 - Cover remaining HFAEntry diagnostic/dump error paths in
// hfaentry.cpp.

}  // namespace
}  // namespace autotest2
