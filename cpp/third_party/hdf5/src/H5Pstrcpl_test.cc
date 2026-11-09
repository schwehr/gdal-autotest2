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
// Tests string creation property list class initialization, character encoding
// getters/setters, encode/decode callbacks, and UTF-8 link/attribute creation
// (third_party/hdf5/src/H5Pstrcpl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pstrcpl.c

#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ScopedPClass = ScopedH5Id<H5Pclose_class>;

constexpr char kCharEncodingPropName[] = "character_encoding";

TEST(H5PstrcplTest, ReinitRegistersStringCreateClassAndSubclasses) {
  // Closing and reopening exercises H5P__strcrt_reg_prop() inside the test
  // body; ensure the library is left open for subsequent tests.
  ASSERT_GE(H5close(), 0);
  absl::Cleanup ensure_open = [] { (void)H5open(); };
  ASSERT_GE(H5open(), 0);

  ScopedPClass lcpl_parent(H5Pget_class_parent(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl_parent.valid());
  EXPECT_GT(H5Pequal(lcpl_parent.get(), H5P_STRING_CREATE), 0);

  ScopedPClass acpl_parent(H5Pget_class_parent(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(acpl_parent.valid());
  EXPECT_GT(H5Pequal(acpl_parent.get(), H5P_STRING_CREATE), 0);
}

TEST(H5PstrcplTest, DefaultEncodingIsAsciiAndNullOutputIsSafe) {
  H5T_cset_t cset = H5T_CSET_ERROR;
  ASSERT_GE(H5Pget_char_encoding(H5P_LINK_CREATE_DEFAULT, &cset), 0);
  EXPECT_EQ(cset, H5T_CSET_ASCII);

  cset = H5T_CSET_ERROR;
  ASSERT_GE(H5Pget_char_encoding(H5P_ATTRIBUTE_CREATE_DEFAULT, &cset), 0);
  EXPECT_EQ(cset, H5T_CSET_ASCII);

  // Passing nullptr for encoding is explicitly handled as a no-op.
  EXPECT_GE(H5Pget_char_encoding(H5P_LINK_CREATE_DEFAULT, nullptr), 0);

  const hid_t classes[] = {H5P_STRING_CREATE, H5P_LINK_CREATE,
                           H5P_ATTRIBUTE_CREATE};
  for (hid_t cls : classes) {
    ScopedPlist plist(H5Pcreate(cls));
    ASSERT_TRUE(plist.valid());
    EXPECT_GT(H5Pisa_class(plist.get(), H5P_STRING_CREATE), 0);

    cset = H5T_CSET_ERROR;
    ASSERT_GE(H5Pget_char_encoding(plist.get(), &cset), 0);
    EXPECT_EQ(cset, H5T_CSET_ASCII);
    EXPECT_GE(H5Pget_char_encoding(plist.get(), nullptr), 0);
  }
}

TEST(H5PstrcplTest, SetGetAndCopyRoundTripAcrossStringCreateClasses) {
  const hid_t classes[] = {H5P_STRING_CREATE, H5P_LINK_CREATE,
                           H5P_ATTRIBUTE_CREATE};
  constexpr H5T_cset_t kEncodings[] = {H5T_CSET_UTF8, H5T_CSET_ASCII};

  for (hid_t cls : classes) {
    ScopedPlist plist(H5Pcreate(cls));
    ASSERT_TRUE(plist.valid());

    for (H5T_cset_t expected : kEncodings) {
      ASSERT_GE(H5Pset_char_encoding(plist.get(), expected), 0);

      H5T_cset_t actual = H5T_CSET_ERROR;
      ASSERT_GE(H5Pget_char_encoding(plist.get(), &actual), 0);
      EXPECT_EQ(actual, expected);

      ScopedPlist copy(H5Pcopy(plist.get()));
      ASSERT_TRUE(copy.valid());
      H5T_cset_t copy_cset = H5T_CSET_ERROR;
      ASSERT_GE(H5Pget_char_encoding(copy.get(), &copy_cset), 0);
      EXPECT_EQ(copy_cset, expected);
      EXPECT_GT(H5Pequal(plist.get(), copy.get()), 0);
    }
  }
}

TEST(H5PstrcplTest, EncodeAndDecodePreserveCharacterEncoding) {
  const hid_t classes[] = {H5P_STRING_CREATE, H5P_LINK_CREATE,
                           H5P_ATTRIBUTE_CREATE};
  constexpr H5T_cset_t kEncodings[] = {H5T_CSET_ASCII, H5T_CSET_UTF8};

  for (hid_t cls : classes) {
    for (H5T_cset_t expected : kEncodings) {
      ScopedPlist plist(H5Pcreate(cls));
      ASSERT_TRUE(plist.valid());
      ASSERT_GE(H5Pset_char_encoding(plist.get(), expected), 0);

      // 1. Measure encoded buffer size (exercises *pp == nullptr in enc cb).
      size_t nalloc = 0;
      ASSERT_GE(H5Pencode2(plist.get(), nullptr, &nalloc, H5P_DEFAULT), 0);
      ASSERT_GT(nalloc, 0u);

      // 2. Encode into buffer (exercises *pp != nullptr in enc cb).
      std::vector<uint8_t> buf(nalloc, 0);
      size_t written = nalloc;
      ASSERT_GE(H5Pencode2(plist.get(), buf.data(), &written, H5P_DEFAULT), 0);
      EXPECT_EQ(written, nalloc);

      // 3. Decode back into a new plist (exercises dec cb) and verify equality.
      ScopedPlist decoded(H5Pdecode(buf.data()));
      ASSERT_TRUE(decoded.valid());

      H5T_cset_t decoded_cset = H5T_CSET_ERROR;
      ASSERT_GE(H5Pget_char_encoding(decoded.get(), &decoded_cset), 0);
      EXPECT_EQ(decoded_cset, expected);
      EXPECT_GT(H5Pequal(plist.get(), decoded.get()), 0);
    }
  }
}

TEST(H5PstrcplTest, RejectsInvalidArgumentsAndRemovedProperty) {
  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  ScopedErrorSilence silence;

  // Out-of-range character encoding values.
  EXPECT_LT(H5Pset_char_encoding(lcpl.get(), H5T_CSET_ERROR), 0);
  EXPECT_LT(H5Pset_char_encoding(lcpl.get(), static_cast<H5T_cset_t>(-2)), 0);
  EXPECT_LT(H5Pset_char_encoding(lcpl.get(), H5T_NCSET), 0);
  EXPECT_LT(H5Pset_char_encoding(lcpl.get(), H5T_CSET_RESERVED_15), 0);

  // Invalid / wrong-class / immutable default property list IDs.
  EXPECT_LT(H5Pset_char_encoding(H5I_INVALID_HID, H5T_CSET_UTF8), 0);
  EXPECT_LT(H5Pset_char_encoding(H5P_DEFAULT, H5T_CSET_UTF8), 0);
  EXPECT_LT(H5Pset_char_encoding(H5P_LINK_CREATE_DEFAULT, H5T_CSET_UTF8), 0);
  EXPECT_LT(H5Pset_char_encoding(H5P_ATTRIBUTE_CREATE_DEFAULT, H5T_CSET_UTF8),
            0);
  EXPECT_LT(H5Pset_char_encoding(fcpl.get(), H5T_CSET_UTF8), 0);

  H5T_cset_t cset = H5T_CSET_ERROR;
  EXPECT_LT(H5Pget_char_encoding(H5I_INVALID_HID, &cset), 0);
  EXPECT_LT(H5Pget_char_encoding(H5P_DEFAULT, &cset), 0);
  EXPECT_LT(H5Pget_char_encoding(H5P_FILE_CREATE_DEFAULT, &cset), 0);
  EXPECT_LT(H5Pget_char_encoding(fcpl.get(), &cset), 0);

  // Removing the underlying property forces H5P_set / H5P_get failure paths.
  ASSERT_GE(H5Premove(lcpl.get(), kCharEncodingPropName), 0);
  EXPECT_LT(H5Pset_char_encoding(lcpl.get(), H5T_CSET_UTF8), 0);
  EXPECT_LT(H5Pget_char_encoding(lcpl.get(), &cset), 0);
}

TEST(H5PstrcplTest, Utf8LinkAndAttributeNamesPersistInMemoryFile) {
  ScopedFile file = CreateInMemoryFile("strcpl_utf8.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_char_encoding(lcpl.get(), H5T_CSET_UTF8), 0);

  // Create a group using UTF-8 link character encoding.
  constexpr char kUtf8Group[] = "\xc3\xa9l\xc3\xa8ve";
  ScopedGroup grp(
      H5Gcreate2(file.get(), kUtf8Group, lcpl.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5L_info2_t linfo{};
  ASSERT_GE(H5Lget_info2(file.get(), kUtf8Group, &linfo, H5P_DEFAULT), 0);
  EXPECT_EQ(linfo.cset, H5T_CSET_UTF8);

  // Attach an attribute using UTF-8 attribute name character encoding.
  ScopedPlist acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(acpl.valid());
  ASSERT_GE(H5Pset_char_encoding(acpl.get(), H5T_CSET_UTF8), 0);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  constexpr char kUtf8Attr[] = "unit_\xc2\xb5m";
  ScopedAttr attr(H5Acreate2(grp.get(), kUtf8Attr, H5T_NATIVE_INT32,
                             space.get(), acpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  H5A_info_t ainfo{};
  ASSERT_GE(H5Aget_info(attr.get(), &ainfo), 0);
  EXPECT_EQ(ainfo.cset, H5T_CSET_UTF8);
}

}  // namespace
}  // namespace hdf5
