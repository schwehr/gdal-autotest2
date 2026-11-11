// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for libtiff/tif_extension.c (TIFFGetTagListCount,
// TIFFGetTagListEntry, TIFFAccessTagMethods, TIFFGetClientInfo, and
// TIFFSetClientInfo).

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

constexpr uint32_t kCustomLongTag = 65001;
constexpr uint32_t kCustomAsciiTag = 65002;
constexpr uint32_t kCustomShortArrayTag = 65003;
constexpr uint32_t kCustomPseudoTag = 0x10001U;
constexpr const char* kHookStateKey = "tif_extension_test.tag_hook_state";

constexpr std::array<TIFFFieldInfo, 3> kCustomTagInfos = {{
    {kCustomLongTag, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0,
     "GoogleTestCustomLong"},
    {kCustomAsciiTag, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII, FIELD_CUSTOM, 1,
     0, "GoogleTestCustomAscii"},
    {kCustomShortArrayTag, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SHORT,
     FIELD_CUSTOM, 1, 1, "GoogleTestCustomShortArray"},
}};

constexpr std::array<TIFFFieldInfo, 1> kCustomPseudoTagInfos = {{
    {kCustomPseudoTag, 0, 0, TIFF_ANY, FIELD_PSEUDO, 1, 0,
     "GoogleTestPseudoTag"},
}};

void RegisterCustomTagsExtender(TIFF* tif) {
  TIFFMergeFieldInfo(tif, kCustomTagInfos.data(), kCustomTagInfos.size());
}

struct TagHookState {
  TIFFVSetMethod parent_vset = nullptr;
  TIFFVGetMethod parent_vget = nullptr;
  TIFFPrintMethod parent_printdir = nullptr;
  uint32_t pseudo_value = 0;
  int vset_calls = 0;
  int vget_calls = 0;
  int print_calls = 0;
  long last_print_flags = 0;  // NOLINT(runtime/int) - matches TIFFPrintMethod.
};

int CustomVSetField(TIFF* tif, uint32_t tag, va_list ap) {
  auto* state =
      static_cast<TagHookState*>(TIFFGetClientInfo(tif, kHookStateKey));
  if (state == nullptr) return 0;
  ++state->vset_calls;
  if (tag == kCustomPseudoTag) {
    state->pseudo_value = va_arg(ap, uint32_t);
    return 1;
  }
  return state->parent_vset != nullptr ? (*state->parent_vset)(tif, tag, ap)
                                       : 0;
}

int CustomVGetField(TIFF* tif, uint32_t tag, va_list ap) {
  auto* state =
      static_cast<TagHookState*>(TIFFGetClientInfo(tif, kHookStateKey));
  if (state == nullptr) return 0;
  ++state->vget_calls;
  if (tag == kCustomPseudoTag) {
    *va_arg(ap, uint32_t*) = state->pseudo_value;
    return 1;
  }
  return state->parent_vget != nullptr ? (*state->parent_vget)(tif, tag, ap)
                                       : 0;
}

void CustomPrintDir(TIFF* tif, std::FILE* fd,
                    long flags) {  // NOLINT(runtime/int) - TIFFPrintMethod ABI.
  auto* state =
      static_cast<TagHookState*>(TIFFGetClientInfo(tif, kHookStateKey));
  if (state == nullptr) return;
  ++state->print_calls;
  state->last_print_flags = flags;
  if (fd != nullptr) {
    std::fprintf(fd, "  CustomPseudoTag: %u\n", state->pseudo_value);
  }
  if (state->parent_printdir != nullptr) {
    (*state->parent_printdir)(tif, fd, flags);
  }
}

size_t CountClientInfoLinks(const TIFF* tif) {
  size_t count = 0;
  for (const TIFFClientInfoLink* link = tif->tif_clientinfo; link != nullptr;
       link = link->next) {
    ++count;
  }
  return count;
}

TEST(TifExtensionTest, TagListEmptyAndOutOfBoundsReturnsSentinel) {
  for (const char* mode : {"w", "w8"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open(mode);
    ASSERT_NE(tif, nullptr);

    EXPECT_EQ(TIFFGetTagListCount(tif.get()), 0);
    EXPECT_EQ(TIFFGetTagListEntry(tif.get(), -1), static_cast<uint32_t>(-1));
    EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 0), static_cast<uint32_t>(-1));
    EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 1), static_cast<uint32_t>(-1));
    EXPECT_EQ(TIFFGetTagListEntry(tif.get(), std::numeric_limits<int>::min()),
              static_cast<uint32_t>(-1));
    EXPECT_EQ(TIFFGetTagListEntry(tif.get(), std::numeric_limits<int>::max()),
              static_cast<uint32_t>(-1));

    // Standard non-FIELD_CUSTOM tags must not enter the custom tag list.
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);

    EXPECT_EQ(TIFFGetTagListCount(tif.get()), 0);
    EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 0), static_cast<uint32_t>(-1));
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  }
}

TEST(TifExtensionTest, TagListTracksStandardCustomFieldsAndUnset) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "Artist One"), 1);
  EXPECT_EQ(TIFFGetTagListCount(tif.get()), 1);
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 0),
            static_cast<uint32_t>(TIFFTAG_ARTIST));
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), -1), static_cast<uint32_t>(-1));
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 1), static_cast<uint32_t>(-1));

  // Updating an existing custom tag keeps the tag list count unchanged.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "Artist Two"), 1);
  EXPECT_EQ(TIFFGetTagListCount(tif.get()), 1);
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 0),
            static_cast<uint32_t>(TIFFTAG_ARTIST));

  std::array<uint8_t, 5> xml_payload = {'<', 'x', '/', '>', '\0'};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COPYRIGHT, "Copyright 2026"), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_STONITS, double{1.5}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_XMLPACKET,
                         static_cast<uint32_t>(xml_payload.size()),
                         xml_payload.data()),
            1);

  ASSERT_EQ(TIFFGetTagListCount(tif.get()), 4);
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 0),
            static_cast<uint32_t>(TIFFTAG_ARTIST));
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 1),
            static_cast<uint32_t>(TIFFTAG_COPYRIGHT));
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 2),
            static_cast<uint32_t>(TIFFTAG_STONITS));
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 3),
            static_cast<uint32_t>(TIFFTAG_XMLPACKET));
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 4), static_cast<uint32_t>(-1));

  // Unsetting a custom tag compacts the custom value list.
  EXPECT_EQ(TIFFUnsetField(tif.get(), TIFFTAG_COPYRIGHT), 1);
  ASSERT_EQ(TIFFGetTagListCount(tif.get()), 3);
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 0),
            static_cast<uint32_t>(TIFFTAG_ARTIST));
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 1),
            static_cast<uint32_t>(TIFFTAG_STONITS));
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 2),
            static_cast<uint32_t>(TIFFTAG_XMLPACKET));
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 3), static_cast<uint32_t>(-1));

  // Resetting to a fresh directory clears custom tags.
  EXPECT_EQ(TIFFCreateDirectory(tif.get()), 0);
  EXPECT_EQ(TIFFGetTagListCount(tif.get()), 0);
  EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 0), static_cast<uint32_t>(-1));
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
}

TEST(TifExtensionTest, TagListRoundTripsRegisteredTagsAcrossDirectories) {
  ScopedTagExtender extender(&RegisterCustomTagsExtender);

  struct FormatCase {
    std::string_view label;
    const char* write_mode;
  };
  constexpr auto kFormats = std::to_array<FormatCase>({
      {.label = "ClassicLE", .write_mode = "wl"},
      {.label = "ClassicBE", .write_mode = "wb"},
      {.label = "BigTiffLE", .write_mode = "wl8"},
      {.label = "BigTiffBE", .write_mode = "wb8"},
  });

  for (const auto& fmt : kFormats) {
    SCOPED_TRACE(fmt.label);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(fmt.write_mode);
      ASSERT_NE(writer, nullptr);

      // Directory 0: standard + registered custom tags.
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{2}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISBLACK),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}),
                1);

      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDESCRIPTION, "DirZero"),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), kCustomLongTag, uint32_t{0x12345678}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), kCustomAsciiTag, "ExtValue"), 1);
      std::array<uint16_t, 3> shorts = {10, 200, 3000};
      ASSERT_EQ(
          TIFFSetField(writer.get(), kCustomShortArrayTag,
                       static_cast<uint16_t>(shorts.size()), shorts.data()),
          1);

      EXPECT_EQ(TIFFGetTagListCount(writer.get()), 4);

      std::array<uint8_t, 2> row0 = {0x11, 0x22};
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

      // Directory 1: distinct subset of custom tags.
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{2}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISBLACK),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}),
                1);

      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PAGENAME, "DirOne"), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), kCustomLongTag, uint32_t{99}), 1);

      EXPECT_EQ(TIFFGetTagListCount(writer.get()), 2);

      std::array<uint8_t, 2> row1 = {0x33, 0x44};
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 0, 0), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    ASSERT_EQ(TIFFGetTagListCount(reader.get()), 4);
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 0),
              static_cast<uint32_t>(TIFFTAG_IMAGEDESCRIPTION));
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 1), kCustomLongTag);
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 2), kCustomAsciiTag);
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 3), kCustomShortArrayTag);
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), -1), static_cast<uint32_t>(-1));
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 4), static_cast<uint32_t>(-1));

    uint32_t got_long = 0;
    const char* got_ascii = nullptr;
    uint16_t got_count = 0;
    const void* got_shorts_raw = nullptr;
    ASSERT_EQ(TIFFGetField(reader.get(), kCustomLongTag, &got_long), 1);
    ASSERT_EQ(TIFFGetField(reader.get(), kCustomAsciiTag, &got_ascii), 1);
    ASSERT_EQ(TIFFGetField(reader.get(), kCustomShortArrayTag, &got_count,
                           &got_shorts_raw),
              1);
    EXPECT_EQ(got_long, uint32_t{0x12345678});
    ASSERT_NE(got_ascii, nullptr);
    EXPECT_EQ(std::string_view(got_ascii), "ExtValue");
    ASSERT_EQ(got_count, 3);
    const auto* got_shorts = static_cast<const uint16_t*>(got_shorts_raw);
    EXPECT_EQ(got_shorts[0], 10);
    EXPECT_EQ(got_shorts[1], 200);
    EXPECT_EQ(got_shorts[2], 3000);

    ASSERT_EQ(TIFFReadDirectory(reader.get()), 1);
    ASSERT_EQ(TIFFGetTagListCount(reader.get()), 2);
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 0),
              static_cast<uint32_t>(TIFFTAG_PAGENAME));
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 1), kCustomLongTag);
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 2), static_cast<uint32_t>(-1));

    uint32_t got_dir1_long = 0;
    const char* got_pagename = nullptr;
    ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_PAGENAME, &got_pagename), 1);
    ASSERT_EQ(TIFFGetField(reader.get(), kCustomLongTag, &got_dir1_long), 1);
    EXPECT_EQ(std::string_view(got_pagename), "DirOne");
    EXPECT_EQ(got_dir1_long, 99U);

    EXPECT_EQ(TIFFReadDirectory(reader.get()), 0);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  }
}

TEST(TifExtensionTest, TagListPopulatesAnonymousTagsFromRawIfd) {
  for (const bool big_tiff : {false, true}) {
    SCOPED_TRACE(big_tiff ? "BigTIFF" : "ClassicTIFF");
    RawTiffBuilder builder(/*big_endian=*/big_tiff, big_tiff);
    const std::array<uint8_t, 1> pixel = {0x7F};
    const uint64_t strip_off = builder.AppendBlob(pixel);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 1),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        big_tiff ? builder.MakeLong8(TIFFTAG_STRIPOFFSETS, strip_off)
                 : builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                                    static_cast<uint32_t>(strip_off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        big_tiff ? builder.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, pixel.size())
                 : builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                                    static_cast<uint32_t>(pixel.size())),
        builder.MakeLong(65100, 0xCAFEBABEU),
        builder.MakeAscii(65101, "AnonTag"),
    });

    MemoryTiffStream stream(builder.Take());
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetWarnAboutUnknownTags(opts.get(), 0);

    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader, nullptr);

    ASSERT_EQ(TIFFGetTagListCount(reader.get()), 2);
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 0), 65100U);
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 1), 65101U);
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), -1), static_cast<uint32_t>(-1));
    EXPECT_EQ(TIFFGetTagListEntry(reader.get(), 2), static_cast<uint32_t>(-1));
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  }
}

TEST(TifExtensionTest, AccessTagMethodsHooksVSetVGetAndPrintDir) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  TIFFTagMethods* methods = TIFFAccessTagMethods(tif.get());
  ASSERT_NE(methods, nullptr);
  EXPECT_EQ(methods, &tif->tif_tagmethods);
  EXPECT_EQ(TIFFAccessTagMethods(tif.get()), methods);
  EXPECT_NE(methods->vsetfield, nullptr);
  EXPECT_NE(methods->vgetfield, nullptr);
  EXPECT_EQ(methods->printdir, nullptr);

  ASSERT_EQ(TIFFMergeFieldInfo(tif.get(), kCustomPseudoTagInfos.data(),
                               kCustomPseudoTagInfos.size()),
            0);

  TagHookState state;
  state.parent_vset = methods->vsetfield;
  state.parent_vget = methods->vgetfield;
  state.parent_printdir = methods->printdir;
  TIFFSetClientInfo(tif.get(), &state, kHookStateKey);

  methods->vsetfield = &CustomVSetField;
  methods->vgetfield = &CustomVGetField;
  methods->printdir = &CustomPrintDir;

  // Custom pseudo-tag dispatches through hooked methods.
  EXPECT_EQ(TIFFSetField(tif.get(), kCustomPseudoTag, uint32_t{4242}), 1);
  uint32_t got_pseudo = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), kCustomPseudoTag, &got_pseudo), 1);
  EXPECT_EQ(got_pseudo, 4242U);
  EXPECT_EQ(state.vset_calls, 1);
  EXPECT_EQ(state.vget_calls, 1);

  // Standard tags delegate cleanly through parent methods.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
  uint32_t got_width = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &got_width), 1);
  EXPECT_EQ(got_width, 16U);
  EXPECT_EQ(state.vset_calls, 2);
  EXPECT_EQ(state.vget_calls, 2);

  // TIFFPrintDirectory invokes the custom printdir hook.
  TempFile print_file("tif_extension_printdir.txt");
  std::unique_ptr<std::FILE, decltype(&std::fclose)> fp(
      std::fopen(print_file.c_str(), "w+b"), &std::fclose);
  ASSERT_NE(fp, nullptr);

  constexpr long kPrintFlags = TIFFPRINT_NONE;  // NOLINT(runtime/int)
  TIFFPrintDirectory(tif.get(), fp.get(), kPrintFlags);
  EXPECT_EQ(state.print_calls, 1);
  EXPECT_EQ(state.last_print_flags, kPrintFlags);

  std::rewind(fp.get());
  std::string printed;
  std::array<char, 256> buf{};
  while (const size_t n = std::fread(buf.data(), 1, buf.size(), fp.get())) {
    printed.append(buf.data(), n);
  }
  EXPECT_NE(printed.find("CustomPseudoTag: 4242"), std::string::npos);

  // Creating a fresh directory restores default tag methods.
  ASSERT_EQ(TIFFCreateDirectory(tif.get()), 0);
  TIFFTagMethods* reset_methods = TIFFAccessTagMethods(tif.get());
  EXPECT_EQ(reset_methods, methods);
  EXPECT_NE(reset_methods->vsetfield, &CustomVSetField);
  EXPECT_NE(reset_methods->vgetfield, &CustomVGetField);
  EXPECT_EQ(reset_methods->printdir, nullptr);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
}

TEST(TifExtensionTest, ClientInfoInsertLookupUpdateAndIsolation) {
  MemoryTiffStream stream_a;
  MemoryTiffStream stream_b;
  UniqueTiff tif_a = stream_a.Open("w");
  UniqueTiff tif_b = stream_b.Open("w");
  ASSERT_NE(tif_a, nullptr);
  ASSERT_NE(tif_b, nullptr);

  // Empty client info list returns nullptr.
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "alpha"), nullptr);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), ""), nullptr);

  int val_alpha = 1;
  int val_beta = 2;
  int val_gamma = 3;
  int val_empty = 4;
  int val_prefix = 5;
  int val_super = 6;
  int val_other_handle = 7;

  // Insert multiple entries (prepended to tif_clientinfo singly-linked list).
  TIFFSetClientInfo(tif_a.get(), &val_alpha, "alpha");
  TIFFSetClientInfo(tif_a.get(), &val_beta, "beta");
  TIFFSetClientInfo(tif_a.get(), &val_gamma, "gamma");
  TIFFSetClientInfo(tif_a.get(), &val_empty, "");
  TIFFSetClientInfo(tif_a.get(), &val_prefix, "tag");
  TIFFSetClientInfo(tif_a.get(), &val_super, "tag_ext");
  EXPECT_EQ(CountClientInfoLinks(tif_a.get()), 6U);

  // Retrieve head, middle, tail, empty name, prefix vs superstring, missing.
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "tag_ext"), &val_super);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "tag"), &val_prefix);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), ""), &val_empty);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "gamma"), &val_gamma);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "beta"), &val_beta);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "alpha"), &val_alpha);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "missing"), nullptr);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "alph"), nullptr);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "alpha_extra"), nullptr);

  // Client info is strictly per-TIFF handle.
  EXPECT_EQ(TIFFGetClientInfo(tif_b.get(), "alpha"), nullptr);
  TIFFSetClientInfo(tif_b.get(), &val_other_handle, "alpha");
  EXPECT_EQ(TIFFGetClientInfo(tif_b.get(), "alpha"), &val_other_handle);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "alpha"), &val_alpha);

  // Update existing entries in place (tail, middle, head, and nullptr data).
  int val_alpha_new = 10;
  int val_super_new = 60;
  TIFFSetClientInfo(tif_a.get(), &val_alpha_new, "alpha");
  TIFFSetClientInfo(tif_a.get(), nullptr, "beta");
  TIFFSetClientInfo(tif_a.get(), &val_super_new, "tag_ext");
  EXPECT_EQ(CountClientInfoLinks(tif_a.get()), 6U);

  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "alpha"), &val_alpha_new);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "beta"), nullptr);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "gamma"), &val_gamma);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "tag_ext"), &val_super_new);

  // Verify name string is copied into library-owned memory rather than aliased.
  std::array<char, 16> stack_key = {"ephemeral_key"};
  int val_ephemeral = 99;
  TIFFSetClientInfo(tif_a.get(), &val_ephemeral, stack_key.data());
  std::memset(stack_key.data(), 'Z', stack_key.size() - 1);
  EXPECT_EQ(CountClientInfoLinks(tif_a.get()), 7U);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), "ephemeral_key"), &val_ephemeral);
  EXPECT_EQ(TIFFGetClientInfo(tif_a.get(), stack_key.data()), nullptr);

  EXPECT_EQ(stream_a.diagnostics().error_count(), 0U);
  EXPECT_EQ(stream_a.diagnostics().warning_count(), 0U);
  EXPECT_EQ(stream_b.diagnostics().error_count(), 0U);
  EXPECT_EQ(stream_b.diagnostics().warning_count(), 0U);
}

TEST(TifExtensionTest, ClientInfoMemoryAccountingAndCleanup) {
  MemoryTiffStream stream;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  stream.diagnostics().Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

  UniqueTiff tif = stream.Open("w", opts.get());
  ASSERT_NE(tif, nullptr);

  const tmsize_t base_alloc = tif->tif_cur_cumulated_mem_alloc;
  constexpr const char* kKey = "tracked_link";
  const tmsize_t expected_link_bytes =
      static_cast<tmsize_t>(sizeof(TIFFClientInfoLink) + std::strlen(kKey) + 1);

  int first_value = 1;
  int second_value = 2;
  TIFFSetClientInfo(tif.get(), &first_value, kKey);
  EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, base_alloc + expected_link_bytes);
  EXPECT_EQ(TIFFGetClientInfo(tif.get(), kKey), &first_value);

  // Updating in place performs no new allocations.
  TIFFSetClientInfo(tif.get(), &second_value, kKey);
  EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, base_alloc + expected_link_bytes);
  EXPECT_EQ(TIFFGetClientInfo(tif.get(), kKey), &second_value);

  // TIFFClose frees all client info nodes and names cleanly without errors.
  tif.reset();
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
}

}  // namespace
}  // namespace libtiff
