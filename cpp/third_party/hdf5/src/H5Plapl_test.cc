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
// Tests link access property list class registration, soft-link traversal
// limits, external link prefix/FAPL/flags/callback properties, encode/decode,
// copy/compare/close lifecycle callbacks, and link traversal behavior
// (third_party/hdf5/src/H5Plapl.c). Note H5_HAVE_PARALLEL is disabled in
// Google3.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Plapl.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kPropNlinks[] = "max soft links";
constexpr char kPropElinkPrefix[] = "external link prefix";
constexpr char kPropElinkFapl[] = "external link fapl";
constexpr char kPropElinkFlags[] = "external link flags";

struct ElinkCallbackLog {
  int call_count = 0;
  std::string parent_file;
  std::string parent_group;
  std::string child_file;
  std::string child_object;
  unsigned observed_flags = 0;
  unsigned override_flags = H5F_ACC_DEFAULT;
  bool set_override_flags = false;
  hid_t override_fapl = H5P_DEFAULT;
  bool set_override_fapl = false;
};

herr_t RecordElinkCallback(const char* parent_file_name,
                           const char* parent_group_name,
                           const char* child_file_name,
                           const char* child_object_name, unsigned* acc_flags,
                           hid_t fapl_id, void* op_data) {
  auto* log = static_cast<ElinkCallbackLog*>(op_data);
  if (log == nullptr) {
    return -1;
  }
  ++log->call_count;
  log->parent_file = parent_file_name ? parent_file_name : "";
  log->parent_group = parent_group_name ? parent_group_name : "";
  log->child_file = child_file_name ? child_file_name : "";
  log->child_object = child_object_name ? child_object_name : "";
  if (acc_flags != nullptr) {
    log->observed_flags = *acc_flags;
    if (log->set_override_flags) {
      *acc_flags = log->override_flags;
    }
  }
  if (log->set_override_fapl && fapl_id >= 0) {
    // Verify fapl_id is a valid file access property list before touching it.
    if (H5Pisa_class(fapl_id, H5P_FILE_ACCESS) <= 0) {
      return -1;
    }
  }
  return 0;
}

TEST(H5PlaplTest, ClassMetadataAndDefaultProperties) {
  char* class_name = H5Pget_class_name(H5P_LINK_ACCESS);
  ASSERT_NE(class_name, nullptr);
  absl::Cleanup free_name = [class_name] { H5free_memory(class_name); };
  EXPECT_STREQ(class_name, "link access");

  ScopedPlist parent(H5Pget_class_parent(H5P_LINK_ACCESS));
  ASSERT_TRUE(parent.valid());
  EXPECT_GT(H5Pequal(parent.get(), H5P_ROOT), 0);

  size_t nlinks = 0;
  ASSERT_GE(H5Pget_nlinks(H5P_LINK_ACCESS_DEFAULT, &nlinks), 0);
  EXPECT_EQ(nlinks, 16u);

  std::array<char, 16> prefix_buf{};
  prefix_buf.fill('Z');
  EXPECT_EQ(
      H5Pget_elink_prefix(H5P_LINK_ACCESS_DEFAULT, prefix_buf.data(),
                          prefix_buf.size()),
      0);
  EXPECT_EQ(prefix_buf[0], 'Z');

  EXPECT_EQ(H5Pget_elink_fapl(H5P_LINK_ACCESS_DEFAULT), H5P_DEFAULT);

  unsigned flags = 0x1234u;
  ASSERT_GE(H5Pget_elink_acc_flags(H5P_LINK_ACCESS_DEFAULT, &flags), 0);
  EXPECT_EQ(flags, static_cast<unsigned>(H5F_ACC_DEFAULT));

  H5L_elink_traverse_t cb_func = &RecordElinkCallback;
  void* cb_data = &nlinks;
  ASSERT_GE(H5Pget_elink_cb(H5P_LINK_ACCESS_DEFAULT, &cb_func, &cb_data), 0);
  EXPECT_EQ(cb_func, nullptr);
  EXPECT_EQ(cb_data, nullptr);
}

TEST(H5PlaplTest, NlinksSetGetValidationAndTraversal) {
  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());

  size_t nlinks = 0;
  ASSERT_GE(H5Pget_nlinks(lapl.get(), &nlinks), 0);
  EXPECT_EQ(nlinks, 16u);

  ASSERT_GE(H5Pset_nlinks(lapl.get(), 4u), 0);
  ASSERT_GE(H5Pget_nlinks(lapl.get(), &nlinks), 0);
  EXPECT_EQ(nlinks, 4u);

  ScopedPlist wrong_cls(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(wrong_cls.valid());
  ScopedPlist removed(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(removed.valid());
  ASSERT_GE(H5Premove(removed.get(), kPropNlinks), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_nlinks(lapl.get(), 0u), 0);
    EXPECT_LT(H5Pset_nlinks(H5I_INVALID_HID, 5u), 0);
    EXPECT_LT(H5Pset_nlinks(H5P_LINK_ACCESS_DEFAULT, 5u), 0);
    EXPECT_LT(H5Pset_nlinks(wrong_cls.get(), 5u), 0);
    EXPECT_LT(H5Pset_nlinks(removed.get(), 5u), 0);

    EXPECT_LT(H5Pget_nlinks(lapl.get(), nullptr), 0);
    EXPECT_LT(H5Pget_nlinks(H5I_INVALID_HID, &nlinks), 0);
    EXPECT_LT(H5Pget_nlinks(wrong_cls.get(), &nlinks), 0);
    EXPECT_LT(H5Pget_nlinks(removed.get(), &nlinks), 0);
  }

  // Functional soft-link chain test in memory: /s0 -> /s1 -> /s2 -> /target.
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup target(
      H5Gcreate2(file.get(), "/target", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(target.valid());
  ASSERT_GE(H5Lcreate_soft("/target", file.get(), "/s2", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(
      H5Lcreate_soft("/s2", file.get(), "/s1", H5P_DEFAULT, H5P_DEFAULT), 0);
  ASSERT_GE(
      H5Lcreate_soft("/s1", file.get(), "/s0", H5P_DEFAULT, H5P_DEFAULT), 0);

  ASSERT_GE(H5Pset_nlinks(lapl.get(), 2u), 0);
  {
    ScopedErrorSilence silence;
    ScopedGroup too_deep(H5Gopen2(file.get(), "/s0", lapl.get()));
    EXPECT_EQ(too_deep.get(), H5I_INVALID_HID);
  }

  ASSERT_GE(H5Pset_nlinks(lapl.get(), 3u), 0);
  ScopedGroup reached(H5Gopen2(file.get(), "/s0", lapl.get()));
  EXPECT_TRUE(reached.valid());
}

TEST(H5PlaplTest, ElinkPrefixSetGetCopyCompareEncodeDecodeAndDelete) {
  ScopedPlist lapl1(H5Pcreate(H5P_LINK_ACCESS));
  ScopedPlist lapl2(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl1.valid());
  ASSERT_TRUE(lapl2.valid());

  EXPECT_GT(H5Pequal(lapl1.get(), lapl2.get()), 0);

  constexpr char kPrefix1[] = "/opt/hdf5/ext";
  constexpr char kPrefix2[] = "/var/hdf5/other";

  ASSERT_GE(H5Pset_elink_prefix(lapl1.get(), kPrefix1), 0);
  EXPECT_EQ(H5Pequal(lapl1.get(), lapl2.get()), 0);
  EXPECT_EQ(H5Pequal(lapl2.get(), lapl1.get()), 0);

  // Overwriting frees the prior prefix via _del/_set callbacks.
  ASSERT_GE(H5Pset_elink_prefix(lapl1.get(), kPrefix2), 0);
  ASSERT_GE(H5Pset_elink_prefix(lapl1.get(), kPrefix1), 0);
  ASSERT_GE(H5Pset_elink_prefix(lapl2.get(), kPrefix2), 0);
  EXPECT_EQ(H5Pequal(lapl1.get(), lapl2.get()), 0);
  ASSERT_GE(H5Pset_elink_prefix(lapl2.get(), kPrefix1), 0);
  EXPECT_GT(H5Pequal(lapl1.get(), lapl2.get()), 0);

  // Length query with nullptr buffer returns exact byte length.
  EXPECT_EQ(H5Pget_elink_prefix(lapl1.get(), nullptr, 0),
            static_cast<ssize_t>(sizeof(kPrefix1) - 1));

  // Truncated query with size > 0 always NUL-terminates.
  std::array<char, 5> small_buf{};
  small_buf.fill('X');
  EXPECT_EQ(
      H5Pget_elink_prefix(lapl1.get(), small_buf.data(), small_buf.size()),
      static_cast<ssize_t>(sizeof(kPrefix1) - 1));
  EXPECT_THAT(small_buf, ElementsAre('/', 'o', 'p', 't', '\0'));

  // Exact query returns full prefix.
  std::array<char, 32> full_buf{};
  EXPECT_EQ(
      H5Pget_elink_prefix(lapl1.get(), full_buf.data(), full_buf.size()),
      static_cast<ssize_t>(sizeof(kPrefix1) - 1));
  EXPECT_STREQ(full_buf.data(), kPrefix1);

  // Generic H5Pget invokes H5P__lacc_elink_pref_get (caller owns xstrdup copy).
  char* dup_prefix = nullptr;
  ASSERT_GE(H5Pget(lapl1.get(), kPropElinkPrefix, &dup_prefix), 0);
  absl::Cleanup free_dup = [&dup_prefix] { H5free_memory(dup_prefix); };
  ASSERT_NE(dup_prefix, nullptr);
  EXPECT_STREQ(dup_prefix, kPrefix1);

  // Copy preserves independent prefix memory.
  ScopedPlist copied(H5Pcopy(lapl1.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(lapl1.get(), copied.get()), 0);

  // Encode/decode non-NULL prefix round-trip.
  size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(lapl1.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  ASSERT_GT(enc_size, 0u);
  std::vector<uint8_t> enc_buf(enc_size, 0);
  ASSERT_GE(H5Pencode2(lapl1.get(), enc_buf.data(), &enc_size, H5P_DEFAULT), 0);

  ScopedPlist decoded(H5Pdecode(enc_buf.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(lapl1.get(), decoded.get()), 0);

  // Encode/decode when prefix is reset to nullptr (len == 0 path).
  ASSERT_GE(H5Pset_elink_prefix(lapl1.get(), nullptr), 0);
  EXPECT_EQ(H5Pget_elink_prefix(lapl1.get(), full_buf.data(), full_buf.size()),
            0);
  char* null_dup = reinterpret_cast<char*>(0x1);
  ASSERT_GE(H5Pget(lapl1.get(), kPropElinkPrefix, &null_dup), 0);
  EXPECT_EQ(null_dup, nullptr);

  size_t enc_null_size = 0;
  ASSERT_GE(H5Pencode2(lapl1.get(), nullptr, &enc_null_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> enc_null_buf(enc_null_size, 0);
  ASSERT_GE(
      H5Pencode2(lapl1.get(), enc_null_buf.data(), &enc_null_size, H5P_DEFAULT),
      0);
  ScopedPlist decoded_null(H5Pdecode(enc_null_buf.data()));
  ASSERT_TRUE(decoded_null.valid());
  EXPECT_EQ(H5Pget_elink_prefix(decoded_null.get(), nullptr, 0), 0);

  // Empty string "" also tests len == 0 encoding branch cleanly.
  ASSERT_GE(H5Pset_elink_prefix(lapl1.get(), ""), 0);
  size_t enc_empty_size = 0;
  ASSERT_GE(H5Pencode2(lapl1.get(), nullptr, &enc_empty_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> enc_empty_buf(enc_empty_size, 0);
  ASSERT_GE(H5Pencode2(lapl1.get(), enc_empty_buf.data(), &enc_empty_size,
                       H5P_DEFAULT),
            0);
  ScopedPlist decoded_empty(H5Pdecode(enc_empty_buf.data()));
  ASSERT_TRUE(decoded_empty.valid());
  EXPECT_EQ(H5Pget_elink_prefix(decoded_empty.get(), nullptr, 0), 0);

  // Explicit H5Premove exercises H5P__lacc_elink_pref_del and subsequent failure.
  ScopedPlist wrong_cls(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(wrong_cls.valid());
  ASSERT_GE(H5Premove(copied.get(), kPropElinkPrefix), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_elink_prefix(H5I_INVALID_HID, kPrefix1), 0);
  EXPECT_LT(H5Pset_elink_prefix(H5P_LINK_ACCESS_DEFAULT, kPrefix1), 0);
  EXPECT_LT(H5Pset_elink_prefix(wrong_cls.get(), kPrefix1), 0);
  EXPECT_LT(H5Pset_elink_prefix(copied.get(), kPrefix1), 0);

  EXPECT_LT(H5Pget_elink_prefix(H5I_INVALID_HID, nullptr, 0), 0);
  EXPECT_LT(H5Pget_elink_prefix(wrong_cls.get(), nullptr, 0), 0);
  EXPECT_LT(H5Pget_elink_prefix(copied.get(), nullptr, 0), 0);
}

TEST(H5PlaplTest, ElinkFaplLifecycleValidationCompareAndEncodeDecode) {
  ScopedPlist lapl_a(H5Pcreate(H5P_LINK_ACCESS));
  ScopedPlist lapl_b(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl_a.valid());
  ASSERT_TRUE(lapl_b.valid());

  EXPECT_EQ(H5Pget_elink_fapl(lapl_a.get()), H5P_DEFAULT);

  ScopedPlist fapl1(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist fapl2(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl1.valid());
  ASSERT_TRUE(fapl2.valid());
  ASSERT_GE(H5Pset_sieve_buf_size(fapl1.get(), 16384), 0);
  ASSERT_GE(H5Pset_sieve_buf_size(fapl2.get(), 32768), 0);

  // Set non-default FAPL and confirm lapl holds an independent copy.
  ASSERT_GE(H5Pset_elink_fapl(lapl_a.get(), fapl1.get()), 0);
  ASSERT_GE(H5Pset_sieve_buf_size(fapl1.get(), 65536), 0);
  ScopedPlist got1(H5Pget_elink_fapl(lapl_a.get()));
  ASSERT_TRUE(got1.valid());
  EXPECT_NE(got1.get(), H5P_DEFAULT);
  EXPECT_NE(got1.get(), fapl1.get());
  size_t sieve_size = 0;
  ASSERT_GE(H5Pget_sieve_buf_size(got1.get(), &sieve_size), 0);
  EXPECT_EQ(sieve_size, 16384u);

  // Compare: non-default vs default in both orders, then vs different FAPL.
  EXPECT_EQ(H5Pequal(lapl_a.get(), lapl_b.get()), 0);
  EXPECT_EQ(H5Pequal(lapl_b.get(), lapl_a.get()), 0);

  ASSERT_GE(H5Pset_elink_fapl(lapl_b.get(), fapl2.get()), 0);
  EXPECT_EQ(H5Pequal(lapl_a.get(), lapl_b.get()), 0);

  // Restore lapl_a back to exact copy of fapl2 -> now equal!
  ASSERT_GE(H5Pset_elink_fapl(lapl_a.get(), fapl2.get()), 0);
  EXPECT_GT(H5Pequal(lapl_a.get(), lapl_b.get()), 0);

  // Copy preserves independent elink fapl.
  ScopedPlist copied(H5Pcopy(lapl_a.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(lapl_a.get(), copied.get()), 0);

  // Encode and decode non-default elink fapl.
  size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(lapl_a.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  ASSERT_GT(enc_size, 0u);
  std::vector<uint8_t> enc_buf(enc_size, 0);
  ASSERT_GE(H5Pencode2(lapl_a.get(), enc_buf.data(), &enc_size, H5P_DEFAULT),
            0);

  ScopedPlist decoded(H5Pdecode(enc_buf.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(lapl_a.get(), decoded.get()), 0);
  ScopedPlist dec_fapl(H5Pget_elink_fapl(decoded.get()));
  ASSERT_TRUE(dec_fapl.valid());
  ASSERT_GE(H5Pget_sieve_buf_size(dec_fapl.get(), &sieve_size), 0);
  EXPECT_EQ(sieve_size, 32768u);

  // Resetting elink fapl to H5P_DEFAULT exercises default-FAPL encode/decode.
  ASSERT_GE(H5Pset_elink_fapl(lapl_a.get(), H5P_DEFAULT), 0);
  EXPECT_EQ(H5Pget_elink_fapl(lapl_a.get()), H5P_DEFAULT);

  size_t enc_def_size = 0;
  ASSERT_GE(H5Pencode2(lapl_a.get(), nullptr, &enc_def_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> enc_def_buf(enc_def_size, 0);
  ASSERT_GE(
      H5Pencode2(lapl_a.get(), enc_def_buf.data(), &enc_def_size, H5P_DEFAULT),
      0);
  ScopedPlist decoded_def(H5Pdecode(enc_def_buf.data()));
  ASSERT_TRUE(decoded_def.valid());
  EXPECT_EQ(H5Pget_elink_fapl(decoded_def.get()), H5P_DEFAULT);

  // Corrupting the nested FAPL payload triggers H5P__lacc_elink_fapl_dec error.
  std::vector<uint8_t> corrupt_buf = enc_buf;
  // Locate the inner encoded FAPL header (version byte followed by
  // H5P_TYPE_FILE_ACCESS = 4, right after "external link fapl\0", flag=1,
  // enc_size=..., varlen=...). Find the property name and corrupt inner vers.
  const std::string prop_str(kPropElinkFapl);
  for (size_t i = 0; i + prop_str.size() + 6 < corrupt_buf.size(); ++i) {
    if (std::memcmp(corrupt_buf.data() + i, prop_str.data(),
                    prop_str.size() + 1) == 0) {
      const size_t flag_idx = i + prop_str.size() + 1;
      const uint8_t enc_len_bytes = corrupt_buf[flag_idx + 1];
      const size_t inner_hdr_idx = flag_idx + 2 + enc_len_bytes;
      if (inner_hdr_idx < corrupt_buf.size()) {
        corrupt_buf[inner_hdr_idx] = 0x7f;  // Invalid property list version
      }
      break;
    }
  }

  ScopedPlist wrong_cls(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(wrong_cls.valid());
  ASSERT_GE(H5Premove(copied.get(), kPropElinkFapl), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Pdecode(corrupt_buf.data()), H5I_INVALID_HID);
  EXPECT_LT(H5Pset_elink_fapl(H5I_INVALID_HID, fapl1.get()), 0);
  EXPECT_LT(H5Pset_elink_fapl(H5P_LINK_ACCESS_DEFAULT, fapl1.get()), 0);
  EXPECT_LT(H5Pset_elink_fapl(wrong_cls.get(), fapl1.get()), 0);
  EXPECT_LT(H5Pset_elink_fapl(lapl_a.get(), H5I_INVALID_HID), 0);
  EXPECT_LT(H5Pset_elink_fapl(lapl_a.get(), wrong_cls.get()), 0);
  EXPECT_LT(H5Pset_elink_fapl(copied.get(), H5P_DEFAULT), 0);

  EXPECT_EQ(H5Pget_elink_fapl(H5I_INVALID_HID), H5I_INVALID_HID);
  EXPECT_EQ(H5Pget_elink_fapl(wrong_cls.get()), H5I_INVALID_HID);
  EXPECT_EQ(H5Pget_elink_fapl(copied.get()), H5I_INVALID_HID);
}

TEST(H5PlaplTest, ElinkAccFlagsAndCallbackValidation) {
  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());

  constexpr unsigned kValidFlags[] = {
      H5F_ACC_DEFAULT,
      H5F_ACC_RDONLY,
      H5F_ACC_RDWR,
      H5F_ACC_RDONLY | H5F_ACC_SWMR_READ,
      H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
  };
  for (unsigned expected : kValidFlags) {
    ASSERT_GE(H5Pset_elink_acc_flags(lapl.get(), expected), 0);
    unsigned actual = 0xdeadbeefu;
    ASSERT_GE(H5Pget_elink_acc_flags(lapl.get(), &actual), 0);
    EXPECT_EQ(actual, expected);
  }
  EXPECT_GE(H5Pget_elink_acc_flags(lapl.get(), nullptr), 0);

  ElinkCallbackLog log{};
  ASSERT_GE(H5Pset_elink_cb(lapl.get(), &RecordElinkCallback, &log), 0);
  H5L_elink_traverse_t got_func = nullptr;
  void* got_data = nullptr;
  ASSERT_GE(H5Pget_elink_cb(lapl.get(), &got_func, &got_data), 0);
  EXPECT_EQ(got_func, &RecordElinkCallback);
  EXPECT_EQ(got_data, &log);
  EXPECT_GE(H5Pget_elink_cb(lapl.get(), nullptr, nullptr), 0);

  // Clearing both callback and op_data succeeds.
  ASSERT_GE(H5Pset_elink_cb(lapl.get(), nullptr, nullptr), 0);
  ASSERT_GE(H5Pget_elink_cb(lapl.get(), &got_func, &got_data), 0);
  EXPECT_EQ(got_func, nullptr);
  EXPECT_EQ(got_data, nullptr);

  ScopedPlist wrong_cls(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(wrong_cls.valid());
  ScopedPlist removed_flags(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(removed_flags.valid());
  ASSERT_GE(H5Premove(removed_flags.get(), kPropElinkFlags), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_elink_acc_flags(lapl.get(), H5F_ACC_TRUNC), 0);
  EXPECT_LT(H5Pset_elink_acc_flags(lapl.get(), 0x12345u), 0);
  EXPECT_LT(H5Pset_elink_acc_flags(H5I_INVALID_HID, H5F_ACC_RDONLY), 0);
  EXPECT_LT(H5Pset_elink_acc_flags(H5P_LINK_ACCESS_DEFAULT, H5F_ACC_RDONLY), 0);
  EXPECT_LT(H5Pset_elink_acc_flags(wrong_cls.get(), H5F_ACC_RDONLY), 0);
  EXPECT_LT(H5Pset_elink_acc_flags(removed_flags.get(), H5F_ACC_RDONLY), 0);

  unsigned flag_out = 0;
  EXPECT_LT(H5Pget_elink_acc_flags(H5I_INVALID_HID, &flag_out), 0);
  EXPECT_LT(H5Pget_elink_acc_flags(wrong_cls.get(), &flag_out), 0);
  // Note: upstream H5Pget_elink_acc_flags passes 0 instead of FAIL on internal
  // H5P_get failure (line 1113), pushing an error while returning 0.
  H5Pget_elink_acc_flags(removed_flags.get(), &flag_out);
  EXPECT_GT(H5Eget_num(H5E_DEFAULT), 0);
  H5Eclear2(H5E_DEFAULT);

  EXPECT_LT(H5Pset_elink_cb(lapl.get(), nullptr, &log), 0);
  EXPECT_LT(H5Pset_elink_cb(H5I_INVALID_HID, &RecordElinkCallback, &log), 0);
  EXPECT_LT(
      H5Pset_elink_cb(H5P_LINK_ACCESS_DEFAULT, &RecordElinkCallback, &log), 0);
  EXPECT_LT(H5Pset_elink_cb(wrong_cls.get(), &RecordElinkCallback, &log), 0);
  EXPECT_LT(H5Pget_elink_cb(H5I_INVALID_HID, &got_func, &got_data), 0);
  EXPECT_LT(H5Pget_elink_cb(wrong_cls.get(), &got_func, &got_data), 0);
}

TEST(H5PlaplTest, ExternalLinkTraversalUsesPrefixFaplFlagsAndCallback) {
  TempDir dir("h5plapl_elink");
  const std::filesystem::path target_dir = dir.child("sub");
  ASSERT_TRUE(std::filesystem::create_directories(target_dir));
  const std::filesystem::path target_file = target_dir / "target.h5";
  const std::filesystem::path main_file = dir.child("main.h5");

  // 1. Create the external target HDF5 file containing group /ext_grp.
  {
    ScopedFile target(
        H5Fcreate(target_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(target.valid());
    ScopedGroup grp(H5Gcreate2(target.get(), "/ext_grp", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }

  // 2. Create main.h5 with an external link referencing relative "target.h5".
  {
    ScopedFile main_f(
        H5Fcreate(main_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(main_f.valid());
    ASSERT_GE(H5Lcreate_external("target.h5", "/ext_grp", main_f.get(),
                                 "/ext_link", H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  ScopedFile main_f(H5Fopen(main_file.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(main_f.valid());

  // 3. Without prefix, traversing "target.h5" fails in dir (since it is in
  //    sub/).
  {
    ScopedErrorSilence silence;
    ScopedGroup missing(H5Gopen2(main_f.get(), "/ext_link", H5P_DEFAULT));
    EXPECT_EQ(missing.get(), H5I_INVALID_HID);
  }

  // 4. Configure LAPL with prefix, custom FAPL, acc flags, and callback.
  ScopedPlist elink_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(elink_fapl.valid());
  ASSERT_GE(H5Pset_sieve_buf_size(elink_fapl.get(), 4096), 0);

  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());
  ASSERT_GE(H5Pset_elink_prefix(lapl.get(), target_dir.c_str()), 0);
  ASSERT_GE(H5Pset_elink_fapl(lapl.get(), elink_fapl.get()), 0);
  ASSERT_GE(H5Pset_elink_acc_flags(lapl.get(), H5F_ACC_RDONLY), 0);

  ElinkCallbackLog log{};
  log.set_override_fapl = true;
  ASSERT_GE(H5Pset_elink_cb(lapl.get(), &RecordElinkCallback, &log), 0);

  ScopedGroup opened(H5Gopen2(main_f.get(), "/ext_link", lapl.get()));
  ASSERT_TRUE(opened.valid());
  EXPECT_EQ(log.call_count, 1);
  EXPECT_THAT(log.parent_file, HasSubstr("main.h5"));
  EXPECT_EQ(log.child_file, "target.h5");
  EXPECT_EQ(log.child_object, "/ext_grp");
  EXPECT_EQ(log.observed_flags, static_cast<unsigned>(H5F_ACC_RDONLY));
}

}  // namespace
}  // namespace hdf5
