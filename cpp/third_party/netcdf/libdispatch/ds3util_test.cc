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

// Unit tests for //third_party/netcdf/libdispatch/ds3util.c (S3 URL parsing,
// canonical rebuilding, NCS3INFO lifecycle, AWS environment capture, INI
// profile parsing, credential/region/profile priority resolution).
//
// Stateful environment, global-state, and filesystem credential tests execute
// inside isolated forked children via RunInForkedChild() for order
// independence and TSan safety.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/ds3util.c

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/libdispatch/ds3util_shim.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/ncbytes.h"
#include "third_party/netcdf/include/ncglobal.h"
#include "third_party/netcdf/include/nclist.h"
#include "third_party/netcdf/include/ncrc.h"
#include "third_party/netcdf/include/ncs3sdk.h"
#include "third_party/netcdf/include/ncuri.h"
#include "third_party/netcdf/include/ncutil.h"
int nc_initialize(void);
void awsdumpprofile(struct AWSprofile* p);
void awsdumpprofiles(NClist* profiles);
void awsprofiles(void);
}  // extern "C"

namespace netcdf {
namespace {

char* HeapDup(const char* s) {
  const size_t len = std::strlen(s) + 1;
  char* out = static_cast<char*>(std::malloc(len));
  if (out != nullptr) {
    std::memcpy(out, s, len);
  }
  return out;
}

bool WriteTextFile(const std::filesystem::path& path,
                   std::string_view content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return false;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.flush();
  return out.good();
}

TEST(Ds3utilTest, S3CloneClearAndDumpInfo) {
  NCS3INFO* null_clone = reinterpret_cast<NCS3INFO*>(0x1);
  ASSERT_EQ(NC_NOERR, NC_s3clone(nullptr, &null_clone));
  EXPECT_EQ(nullptr, null_clone);

  NCS3INFO empty{};
  EXPECT_EQ(NC_NOERR, NC_s3clone(&empty, nullptr));
  EXPECT_STREQ("host=null region=null bucket=null rootkey=null profile=null",
               NC_s3dumps3info(&empty));
  EXPECT_EQ(NC_NOERR, NC_s3clear(nullptr));
  EXPECT_EQ(NC_NOERR, NC_s3clear(&empty));

  NCS3INFO src{};
  src.host = HeapDup("s3.us-west-2.amazonaws.com");
  src.region = HeapDup("us-west-2");
  src.bucket = HeapDup("obs-bucket");
  src.rootkey = HeapDup("/dir/data.nc");
  src.profile = HeapDup("default");
  src.svc = NCS3;
  const absl::Cleanup clear_src = [&] { NC_s3clear(&src); };

  const char* dump = NC_s3dumps3info(&src);
  EXPECT_STREQ(
      "host=s3.us-west-2.amazonaws.com region=us-west-2 bucket=obs-bucket "
      "rootkey=/dir/data.nc profile=default",
      dump);

  NCS3INFO* cloned = nullptr;
  ASSERT_EQ(NC_NOERR, NC_s3clone(&src, &cloned));
  ASSERT_NE(nullptr, cloned);
  const absl::Cleanup free_cloned = [&] {
    NC_s3clear(cloned);
    std::free(cloned);
  };

  EXPECT_NE(src.host, cloned->host);
  EXPECT_STREQ(src.host, cloned->host);
  EXPECT_NE(src.region, cloned->region);
  EXPECT_STREQ(src.region, cloned->region);
  EXPECT_NE(src.bucket, cloned->bucket);
  EXPECT_STREQ(src.bucket, cloned->bucket);
  EXPECT_NE(src.rootkey, cloned->rootkey);
  EXPECT_STREQ(src.rootkey, cloned->rootkey);
  EXPECT_NE(src.profile, cloned->profile);
  EXPECT_STREQ(src.profile, cloned->profile);
}

TEST(Ds3utilTest, IsS3RecognizesProtocolsModesAndHostSuffixes) {
  NCS3SVC svc = NCS3;
  EXPECT_EQ(0, NC_iss3(nullptr, &svc));

  struct IsS3Case {
    const char* name;
    const char* uri;
    int expected;
    bool check_unk_svc;
  };
  constexpr IsS3Case kCases[] = {
      {"s3_proto", "s3://mybucket/obj.nc", 1, false},
      {"gs3_proto", "gs3://mybucket/obj.nc", 1, false},
      {"mode_s3", "https://example.com/obj.nc#mode=s3", 1, false},
      {"mode_gs3", "https://example.com/obj.nc#mode=gs3", 1, false},
      {"aws_host", "https://mybucket.s3.amazonaws.com/obj.nc", 1, false},
      {"gcs_host", "https://storage.googleapis.com/mybucket/obj.nc", 1, false},
      {"plain_https", "https://example.com/obj.nc", 0, true},
      {"long_other_host", "https://storage.example.org/obj.nc", 0, true},
      {"file_no_host", "file:///local/path/obj.nc", 0, true},
  };

  for (const IsS3Case& tc : kCases) {
    SCOPED_TRACE(tc.name);
    NCURI* uri = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse(tc.uri, &uri));
    const absl::Cleanup free_uri = [&] { ncurifree(uri); };
    NCS3SVC out_svc = NCS3;
    EXPECT_EQ(tc.expected, NC_iss3(uri, &out_svc));
    if (tc.check_unk_svc) {
      EXPECT_EQ(NCS3UNK, out_svc);
      EXPECT_EQ(0, NC_iss3(uri, nullptr));
    }
  }
}

TEST(Ds3utilTest, S3UrlRebuildHandlesAllUrlFormatsAndErrorCases) {
  ASSERT_EQ(NC_NOERR, nc_initialize());

  EXPECT_EQ(NC_EURL, NC_s3urlrebuild(nullptr, nullptr, nullptr));

  // Missing host and empty host/path segments fail with NC_EURL.
  {
    NCURI* uri = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse("file:///local/a.nc", &uri));
    const absl::Cleanup free_uri = [&] { ncurifree(uri); };
    EXPECT_EQ(NC_EURL, NC_s3urlrebuild(uri, nullptr, nullptr));
  }
  for (const char* bad_delim :
       {"https://a..b/bucket/key", "https://s3.amazonaws.com/b//k"}) {
    NCURI* uri = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse(bad_delim, &uri));
    const absl::Cleanup free_uri = [&] { ncurifree(uri); };
    EXPECT_EQ(NC_EURL, NC_s3urlrebuild(uri, nullptr, nullptr));
  }
  // Bad AWS segment counts and malformed 5-segment hosts fail with NC_EURL.
  for (const char* bad_aws :
       {"https://a.b.c.d.amazonaws.com/key",
        "https://mybucket.nots3.us-west-2.amazonaws.com/key"}) {
    NCURI* uri = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse(bad_aws, &uri));
    const absl::Cleanup free_uri = [&] { ncurifree(uri); };
    EXPECT_EQ(NC_EURL, NC_s3urlrebuild(uri, nullptr, nullptr));
  }
  // Path-style URL with no bucket returns NC_ES3 when s3 is null.
  {
    NCURI* uri = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse("https://s3.amazonaws.com/", &uri));
    const absl::Cleanup free_uri = [&] { ncurifree(uri); };
    EXPECT_EQ(NC_ES3, NC_s3urlrebuild(uri, nullptr, nullptr));
  }

  static_assert(sizeof(char*) == 8,
                "Format (9) test bucket length matches LP64 sizeof(char*)");
  struct FormatCase {
    const char* name;
    const char* input;
    const char* expected_bucket;
    const char* expected_region;
    NCS3SVC expected_svc;
    const char* expected_host_prefix;
  };
  // Note: In ds3util.c's Format (9), removing segment 0 leaves segment index 2
  // pointing at the 4th original host label ("custom"), and `url->host +
  // sizeof(bucket) + 1` advances past the 8-byte bucket name ("mybucket") on
  // 64-bit LP64 hosts.
  constexpr FormatCase kFormats[] = {
      {"fmt1_virtual_region",
       "https://mybucket.s3.us-west-2.amazonaws.com/dir/obj.nc", "mybucket",
       "us-west-2", NCS3, "s3.us-west-2.amazonaws.com"},
      {"fmt2_virtual_default",
       "https://mybucket.s3.amazonaws.com/dir/obj.nc#AWS.REGION=us-east-1",
       "mybucket", "us-east-1", NCS3, "s3.us-east-1.amazonaws.com"},
      {"fmt3_path_region",
       "https://s3.eu-central-1.amazonaws.com/mybucket/dir/obj.nc", "mybucket",
       "eu-central-1", NCS3, "s3.eu-central-1.amazonaws.com"},
      {"fmt4_path_default",
       "https://s3.amazonaws.com/mybucket/dir/obj.nc#AWS.REGION=us-east-1",
       "mybucket", "us-east-1", NCS3, "s3.us-east-1.amazonaws.com"},
      {"fmt5_s3_scheme", "s3://mybucket/dir/obj.nc#AWS.REGION=us-west-1",
       "mybucket", "us-west-1", NCS3, "s3.us-west-1.amazonaws.com"},
      {"fmt6_google_https",
       "https://storage.googleapis.com/mybucket/dir/"
       "obj.nc#AWS.REGION=us-east-1",
       "mybucket", "us-east-1", NCS3GS, "storage.googleapis.com"},
      {"fmt7_gs3_scheme", "gs3://mybucket/dir/obj.nc#AWS.REGION=us-east-1",
       "mybucket", "us-east-1", NCS3GS, "storage.googleapis.com"},
      {"fmt8_other_custom_port",
       "https://minio.example.com:9000/mybucket/dir/"
       "obj.nc#AWS.REGION=us-east-1",
       "mybucket", "us-east-1", NCS3UNK, "minio.example.com"},
      {"fmt8_two_seg_host",
       "https://s3.local/mybucket/dir/obj.nc#AWS.REGION=us-east-1", "mybucket",
       "us-east-1", NCS3UNK, "s3.local"},
      {"fmt9_other_virtual",
       "https://mybucket.s3.us-west-2.custom.example/dir/obj.nc", "mybucket",
       "custom", NCS3UNK, "s3.us-west-2.custom.example"},
      {"fmt10_other_path_region",
       "https://s3.ap-south-1.custom.example/mybucket/dir/obj.nc", "mybucket",
       "ap-south-1", NCS3UNK, "s3.ap-south-1.custom.example"},
  };

  for (const FormatCase& tc : kFormats) {
    SCOPED_TRACE(tc.name);
    NCURI* in_uri = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse(tc.input, &in_uri));
    const absl::Cleanup free_in = [&] { ncurifree(in_uri); };

    NCS3INFO s3{};
    const absl::Cleanup clear_s3 = [&] { NC_s3clear(&s3); };
    NCURI* out_uri = nullptr;
    ASSERT_EQ(NC_NOERR, NC_s3urlrebuild(in_uri, &s3, &out_uri));
    ASSERT_NE(nullptr, out_uri);
    const absl::Cleanup free_out = [&] { ncurifree(out_uri); };

    EXPECT_STREQ(tc.expected_bucket, s3.bucket);
    EXPECT_STREQ(tc.expected_region, s3.region);
    EXPECT_EQ(tc.expected_svc, s3.svc);
    EXPECT_STREQ("https", out_uri->protocol);
    EXPECT_STREQ(tc.expected_host_prefix, out_uri->host);
    EXPECT_STREQ("/mybucket/dir/obj.nc", out_uri->path);
    EXPECT_EQ(1, NC_testmode(out_uri, "s3"));
  }

  // Fallback to caller-provided s3->region and s3->bucket when absent in URL.
  // NC_s3urlrebuild overwrites s3.region/s3.bucket with newly malloc'd strings
  // without freeing prior caller values, so stack buffers avoid leaks.
  {
    NCURI* in_uri = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse("https://s3.amazonaws.com/", &in_uri));
    const absl::Cleanup free_in = [&] { ncurifree(in_uri); };

    char region_buf[] = "ap-southeast-2";
    char bucket_buf[] = "presetbucket";
    NCS3INFO s3{};
    s3.region = region_buf;
    s3.bucket = bucket_buf;
    NCURI* out_uri = nullptr;
    ASSERT_EQ(NC_NOERR, NC_s3urlrebuild(in_uri, &s3, &out_uri));
    const absl::Cleanup free_out = [&] { ncurifree(out_uri); };
    const absl::Cleanup clear_s3 = [&] { NC_s3clear(&s3); };

    EXPECT_STREQ("ap-southeast-2", s3.region);
    EXPECT_STREQ("presetbucket", s3.bucket);
    EXPECT_STREQ("s3.ap-southeast-2.amazonaws.com", out_uri->host);
    EXPECT_STREQ("/presetbucket", out_uri->path);
  }

  // Valid URL with null s3 and null newurlp succeeds without leaking, and
  // preset region with missing bucket fails with NC_ES3.
  {
    NCURI* uri = nullptr;
    ASSERT_EQ(
        NC_NOERR,
        ncuriparse("https://s3.us-east-1.amazonaws.com/mybucket/k", &uri));
    const absl::Cleanup free_uri = [&] { ncurifree(uri); };
    EXPECT_EQ(NC_NOERR, NC_s3urlrebuild(uri, nullptr, nullptr));
  }
  {
    NCURI* uri = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse("https://s3.amazonaws.com/", &uri));
    const absl::Cleanup free_uri = [&] { ncurifree(uri); };
    char region_buf[] = "ap-southeast-2";
    NCS3INFO s3{};
    s3.region = region_buf;
    EXPECT_EQ(NC_ES3, NC_s3urlrebuild(uri, &s3, nullptr));
  }
}

TEST(Ds3utilTest, S3UrlProcessExtractsHostPortRootKeyAndProfile) {
  ASSERT_EQ(NC_NOERR, nc_initialize());

  NCS3INFO bad_s3{};
  EXPECT_EQ(NC_EURL, NC_s3urlprocess(nullptr, &bad_s3, nullptr));
  {
    NCURI* valid_uri = nullptr;
    ASSERT_EQ(NC_NOERR,
              ncuriparse("https://s3.us-east-1.amazonaws.com/mybucket/k",
                         &valid_uri));
    const absl::Cleanup free_valid = [&] { ncurifree(valid_uri); };
    EXPECT_EQ(NC_EURL, NC_s3urlprocess(valid_uri, nullptr, nullptr));
  }
  {
    NCURI* file_uri = nullptr;
    ASSERT_EQ(NC_NOERR,
              ncuriparse("file:///local/a.nc#AWS.PROFILE=no", &file_uri));
    const absl::Cleanup free_file = [&] { ncurifree(file_uri); };
    const absl::Cleanup clear_bad = [&] { NC_s3clear(&bad_s3); };
    EXPECT_EQ(NC_EURL, NC_s3urlprocess(file_uri, &bad_s3, nullptr));
  }

  // Host with explicit port.
  {
    NCURI* uri = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse("https://obj.example:8443/bkt/sub/a.nc"
                                   "#AWS.REGION=eu-west-1&AWS.PROFILE=no",
                                   &uri));
    const absl::Cleanup free_uri = [&] { ncurifree(uri); };

    NCS3INFO s3{};
    const absl::Cleanup clear_s3 = [&] { NC_s3clear(&s3); };
    NCURI* out_uri = nullptr;
    ASSERT_EQ(NC_NOERR, NC_s3urlprocess(uri, &s3, &out_uri));
    const absl::Cleanup free_out = [&] { ncurifree(out_uri); };

    EXPECT_STREQ("obj.example:8443", s3.host);
    EXPECT_STREQ("bkt", s3.bucket);
    EXPECT_STREQ("eu-west-1", s3.region);
    EXPECT_STREQ("no", s3.profile);
    EXPECT_STREQ("/sub/a.nc", s3.rootkey);
  }

  // Host without port and URL without extra rootkey path segments.
  {
    NCURI* uri = nullptr;
    ASSERT_EQ(NC_NOERR,
              ncuriparse("s3://onlybucket#AWS.REGION=us-east-2&AWS.PROFILE=no",
                         &uri));
    const absl::Cleanup free_uri = [&] { ncurifree(uri); };

    NCS3INFO s3{};
    const absl::Cleanup clear_s3 = [&] { NC_s3clear(&s3); };
    ASSERT_EQ(NC_NOERR, NC_s3urlprocess(uri, &s3, nullptr));

    EXPECT_STREQ("s3.us-east-2.amazonaws.com", s3.host);
    EXPECT_STREQ("onlybucket", s3.bucket);
    EXPECT_STREQ("us-east-2", s3.region);
    EXPECT_STREQ("/", s3.rootkey);
  }
}

TEST(Ds3utilTest, S3SdkEnvironmentCapturesAwsEnvVariables) {
  // Default region fallback when no AWS_* env vars are set.
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    ::unsetenv(AWS_ENV_REGION);
    ::unsetenv(AWS_ENV_DEFAULT_REGION);
    ::unsetenv(AWS_ENV_ACCESS_KEY_ID);
    ::unsetenv(AWS_ENV_SECRET_ACCESS_KEY);
    ::unsetenv(AWS_ENV_CONFIG_FILE);
    ::unsetenv(AWS_ENV_PROFILE);

    NCglobalstate* gs = NC_getglobalstate();
    ds3util_shim_clear_aws(gs);
    NC_s3sdkenvironment();
    const absl::Cleanup cleanup = [&] { ds3util_shim_clear_aws(gs); };

    if (gs->aws.default_region == nullptr ||
        std::strcmp(gs->aws.default_region, AWS_GLOBAL_DEFAULT_REGION) != 0 ||
        gs->aws.access_key_id != nullptr || gs->aws.config_file != nullptr ||
        gs->aws.profile != nullptr || gs->aws.secret_access_key != nullptr) {
      return false;
    }

    // Existing default_region is preserved when AWS_* env vars are unset.
    std::free(gs->aws.default_region);
    gs->aws.default_region = HeapDup("eu-north-1");
    NC_s3sdkenvironment();
    return gs->aws.default_region != nullptr &&
           std::strcmp(gs->aws.default_region, "eu-north-1") == 0;
  }));

  // AWS_DEFAULT_REGION + keys/config/profile captured and read by "no" profile.
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    ::unsetenv(AWS_ENV_REGION);
    ::setenv(AWS_ENV_DEFAULT_REGION, "ap-northeast-2", 1);
    ::setenv(AWS_ENV_ACCESS_KEY_ID, "ENVKEYID", 1);
    ::setenv(AWS_ENV_SECRET_ACCESS_KEY, "ENVSECRET", 1);
    ::setenv(AWS_ENV_CONFIG_FILE,
             (std::string(::testing::TempDir()) + "/ignored_cfg").c_str(), 1);
    ::setenv(AWS_ENV_PROFILE, "staging", 1);

    NCglobalstate* gs = NC_getglobalstate();
    ds3util_shim_clear_aws(gs);
    NC_s3sdkenvironment();
    const absl::Cleanup cleanup = [&] { ds3util_shim_clear_aws(gs); };

    if (!gs->aws.default_region ||
        std::strcmp(gs->aws.default_region, "ap-northeast-2") != 0 ||
        !gs->aws.access_key_id ||
        std::strcmp(gs->aws.access_key_id, "ENVKEYID") != 0 ||
        !gs->aws.secret_access_key ||
        std::strcmp(gs->aws.secret_access_key, "ENVSECRET") != 0 ||
        !gs->aws.config_file ||
        std::strcmp(
            gs->aws.config_file,
            (std::string(::testing::TempDir()) + "/ignored_cfg").c_str()) !=
            0 ||
        !gs->aws.profile || std::strcmp(gs->aws.profile, "staging") != 0) {
      return false;
    }

    const char* region = nullptr;
    const char* access_id = nullptr;
    const char* secret = nullptr;
    NC_s3getcredentials("no", &region, &access_id, &secret);
    return region != nullptr && std::strcmp(region, "ap-northeast-2") == 0 &&
           access_id != nullptr && std::strcmp(access_id, "ENVKEYID") == 0 &&
           secret != nullptr && std::strcmp(secret, "ENVSECRET") == 0;
  }));

  // AWS_REGION takes precedence over AWS_DEFAULT_REGION.
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    ::setenv(AWS_ENV_REGION, "ca-central-1", 1);
    ::setenv(AWS_ENV_DEFAULT_REGION, "us-east-2", 1);
    ::unsetenv(AWS_ENV_ACCESS_KEY_ID);
    ::unsetenv(AWS_ENV_SECRET_ACCESS_KEY);
    ::unsetenv(AWS_ENV_CONFIG_FILE);
    ::unsetenv(AWS_ENV_PROFILE);

    NCglobalstate* gs = NC_getglobalstate();
    ds3util_shim_clear_aws(gs);
    NC_s3sdkenvironment();
    const absl::Cleanup cleanup = [&] { ds3util_shim_clear_aws(gs); };

    return gs->aws.default_region != nullptr &&
           std::strcmp(gs->aws.default_region, "ca-central-1") == 0;
  }));
}

TEST(Ds3utilTest,
     AwsLoadCredentialsParsesFilesMergesProfilesAndResolvesPriority) {
  TempDir aws_dir("ds3_aws_valid");
  const std::filesystem::path extra_cfg = aws_dir.child("extra_cfg");
  const std::filesystem::path aws_config = aws_dir.child(".aws/config");
  const std::filesystem::path aws_creds = aws_dir.child(".aws/credentials");

  ASSERT_TRUE(WriteTextFile(extra_cfg,
                            "; comment at start of line\n"
                            "[default]\n"
                            "\tregion = us-west-1\n"
                            "\n"
                            "[profile work]\n"
                            "region = eu-west-1\n"
                            "empty_val =\n"));
  ASSERT_TRUE(WriteTextFile(aws_config,
                            "[profile work]\n"
                            "region = eu-central-1\n"
                            "token = val123\n"
                            "\n"
                            "[solo]\n"
                            "; comment inside entries\n"
                            "note = val;inline\n"));
  ASSERT_TRUE(WriteTextFile(aws_creds,
                            "[default]\n"
                            "aws_access_key_id = DEFID\n"
                            "aws_secret_access_key = DEFSEC\n"
                            "\n"
                            "[work]\n"
                            "aws_access_key_id = WORKID\n"
                            "aws_secret_access_key = WORKSEC\n"
                            "; eof comment without newline"));

  // Child 1: load extra_cfg + .aws/config + .aws/credentials via
  // NC_TEST_AWS_DIR, verify profile lookup, merging, credentials, priority
  // order, and dumpers.
  ASSERT_TRUE(RunInForkedChild([&]() -> bool {
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    ::unsetenv(AWS_ENV_ACCESS_KEY_ID);
    ::unsetenv(AWS_ENV_SECRET_ACCESS_KEY);
    ::unsetenv(NCRCENVIGNORE);
    ::setenv("NC_TEST_AWS_DIR", aws_dir.c_str(), 1);

    NCglobalstate* gs = NC_getglobalstate();
    ds3util_shim_clear_aws(gs);
    const absl::Cleanup cleanup_aws = [&] { ds3util_shim_clear_aws(gs); };
    gs->rcinfo->ignore = 0;
    // NC_aws_load_credentials prepends aws_root / home, so config_file must be
    // relative to NC_TEST_AWS_DIR here.
    gs->aws.config_file = HeapDup("extra_cfg");

    if (NC_aws_load_credentials(gs) != NC_NOERR) {
      return false;
    }

    struct AWSprofile* prof = nullptr;
    if (NC_authgets3profile("work", &prof) != NC_NOERR || prof == nullptr) {
      return false;
    }
    if (NC_authgets3profile("work", nullptr) != NC_NOERR) {
      return false;
    }
    if (NC_authgets3profile("nonexistent", &prof) != NC_NOERR ||
        prof != nullptr) {
      return false;
    }

    const char* val = nullptr;
    if (NC_s3profilelookup(nullptr, "region", &val) != NC_ES3) {
      return false;
    }
    if (NC_s3profilelookup("nonexistent", "region", &val) != NC_NOERR ||
        val != nullptr) {
      return false;
    }
    if (NC_s3profilelookup("work", "region", nullptr) != NC_NOERR) {
      return false;
    }
    if (NC_s3profilelookup("default", "region", &val) != NC_NOERR ||
        val == nullptr || std::strcmp(val, "us-west-1") != 0) {
      return false;
    }
    if (NC_s3profilelookup("work", "region", &val) != NC_NOERR ||
        val == nullptr || std::strcmp(val, "eu-central-1") != 0) {
      return false;
    }
    if (NC_s3profilelookup("work", "token", &val) != NC_NOERR ||
        val == nullptr || std::strcmp(val, "val123") != 0) {
      return false;
    }
    if (NC_s3profilelookup("work", "empty_val", &val) != NC_NOERR ||
        val == nullptr || std::strcmp(val, "") != 0) {
      return false;
    }
    if (NC_s3profilelookup("work", "missing_key", &val) != NC_NOERR ||
        val != nullptr) {
      return false;
    }
    if (NC_s3profilelookup("solo", "note", &val) != NC_NOERR ||
        val == nullptr || std::strcmp(val, "val;inline") != 0) {
      return false;
    }

    const char* reg = nullptr;
    const char* ak = nullptr;
    const char* sk = nullptr;
    NC_s3getcredentials("work", &reg, &ak, &sk);
    if (!reg || std::strcmp(reg, "eu-central-1") != 0 || !ak ||
        std::strcmp(ak, "WORKID") != 0 || !sk ||
        std::strcmp(sk, "WORKSEC") != 0) {
      return false;
    }
    const char* dreg = nullptr;
    const char* dak = nullptr;
    const char* dsk = nullptr;
    NC_s3getcredentials("default", &dreg, &dak, &dsk);
    if (!dreg || std::strcmp(dreg, "us-west-1") != 0 || !dak ||
        std::strcmp(dak, "DEFID") != 0 || !dsk ||
        std::strcmp(dsk, "DEFSEC") != 0) {
      return false;
    }
    NC_s3getcredentials(nullptr, nullptr, nullptr, nullptr);

    // Non-matching URI with no fragment and no .rc entry used to test
    // NC_getdefaults3region() fallthroughs cleanly (upstream NC_rclookupx()
    // dereferences uri->path unconditionally when uri == NULL).
    NCURI* plain_uri = nullptr;
    if (ncuriparse("https://plain.example.com/other/k", &plain_uri) !=
        NC_NOERR) {
      return false;
    }
    const absl::Cleanup free_plain = [&] { ncurifree(plain_uri); };

    // Priority 1: URI fragment.
    {
      NCURI* frag_uri = nullptr;
      if (ncuriparse("https://rc.example.com/b/k#AWS.PROFILE=solo&"
                     "AWS.REGION=sa-east-1",
                     &frag_uri) != NC_NOERR) {
        return false;
      }
      const absl::Cleanup free_frag = [&] { ncurifree(frag_uri); };
      const char* p_out = nullptr;
      const char* r_out = nullptr;
      if (NC_getactives3profile(frag_uri, &p_out) != NC_NOERR || !p_out ||
          std::strcmp(p_out, "solo") != 0) {
        return false;
      }
      if (NC_getdefaults3region(frag_uri, &r_out) != NC_NOERR || !r_out ||
          std::strcmp(r_out, "sa-east-1") != 0) {
        return false;
      }
    }

    // Priority 2: .rc entry.
    if (NC_rcfile_insert(AWS_RC_PROFILE, "rc.example.com", "/b/k", "solo") !=
            NC_NOERR ||
        NC_rcfile_insert(AWS_RC_REGION, "rc.example.com", "/b/k",
                         "ap-south-2") != NC_NOERR) {
      return false;
    }
    {
      NCURI* rc_uri = nullptr;
      if (ncuriparse("https://rc.example.com/b/k", &rc_uri) != NC_NOERR) {
        return false;
      }
      const absl::Cleanup free_rc = [&] { ncurifree(rc_uri); };
      const char* p_out = nullptr;
      const char* r_out = nullptr;
      if (NC_getactives3profile(rc_uri, &p_out) != NC_NOERR || !p_out ||
          std::strcmp(p_out, "solo") != 0) {
        return false;
      }
      if (NC_getdefaults3region(rc_uri, &r_out) != NC_NOERR || !r_out ||
          std::strcmp(r_out, "ap-south-2") != 0) {
        return false;
      }
    }

    // Unknown gs->aws.profile falls through to "default".
    gs->aws.profile = HeapDup("unknown_profile");
    {
      const char* p_out = nullptr;
      if (NC_getactives3profile(nullptr, &p_out) != NC_NOERR || !p_out ||
          std::strcmp(p_out, "default") != 0) {
        return false;
      }
    }
    std::free(gs->aws.profile);
    gs->aws.profile = nullptr;

    // Priority 3: gs->aws.profile + profile region lookup.
    gs->aws.profile = HeapDup("work");
    {
      const char* p_out = nullptr;
      const char* r_out = nullptr;
      if (NC_getactives3profile(nullptr, &p_out) != NC_NOERR || !p_out ||
          std::strcmp(p_out, "work") != 0) {
        return false;
      }
      if (p_out != gs->aws.profile) {
        std::free(const_cast<char*>(p_out));
      }
      if (NC_getdefaults3region(plain_uri, &r_out) != NC_NOERR || !r_out ||
          std::strcmp(r_out, "eu-central-1") != 0) {
        return false;
      }
    }

    // Priority 4: "default" profile when gs->aws.profile is unset.
    std::free(gs->aws.profile);
    gs->aws.profile = nullptr;
    {
      const char* p_out = nullptr;
      const char* r_out = nullptr;
      if (NC_getactives3profile(nullptr, &p_out) != NC_NOERR || !p_out ||
          std::strcmp(p_out, "default") != 0) {
        return false;
      }
      if (NC_getdefaults3region(plain_uri, &r_out) != NC_NOERR || !r_out ||
          std::strcmp(r_out, "us-west-1") != 0) {
        return false;
      }
    }

    // Dumpers: NULL profile, profile with NULL entries, and loaded list.
    awsdumpprofile(nullptr);
    struct AWSprofile bare{};
    char bare_name[] = "bare";
    bare.name = bare_name;
    awsdumpprofile(&bare);
    awsprofiles();

    // Empty profile list leaves active profile NULL and NC_s3urlprocess
    // defaults profile to "no".
    NC_s3freeprofilelist(gs->rcinfo->s3profiles);
    gs->rcinfo->s3profiles = nclistnew();
    {
      const char* p_empty = reinterpret_cast<const char*>(0x1);
      if (NC_getactives3profile(nullptr, &p_empty) != NC_NOERR ||
          p_empty != nullptr) {
        return false;
      }
      NCURI* s3_uri = nullptr;
      if (ncuriparse("https://s3.us-east-1.amazonaws.com/mybucket/k",
                     &s3_uri) != NC_NOERR) {
        return false;
      }
      const absl::Cleanup free_s3_uri = [&] { ncurifree(s3_uri); };
      NCS3INFO empty_s3{};
      const absl::Cleanup clear_empty_s3 = [&] { NC_s3clear(&empty_s3); };
      if (NC_s3urlprocess(s3_uri, &empty_s3, nullptr) != NC_NOERR ||
          !empty_s3.profile || std::strcmp(empty_s3.profile, "no") != 0) {
        return false;
      }
    }

    // Priority 5 ("no" profile) + default_region vs AWS_GLOBAL_DEFAULT_REGION.
    struct AWSprofile* no_prof =
        static_cast<struct AWSprofile*>(std::calloc(1, sizeof(*no_prof)));
    no_prof->name = HeapDup("no");
    no_prof->entries = nclistnew();
    nclistpush(gs->rcinfo->s3profiles, no_prof);

    const char* p_no = nullptr;
    const char* r_def = nullptr;
    if (NC_getactives3profile(nullptr, &p_no) != NC_NOERR || !p_no ||
        std::strcmp(p_no, "no") != 0) {
      return false;
    }
    if (NC_getactives3profile(nullptr, nullptr) != NC_NOERR) {
      return false;
    }
    std::free(gs->aws.default_region);
    gs->aws.default_region = nullptr;
    if (NC_getdefaults3region(plain_uri, &r_def) != NC_NOERR || !r_def ||
        std::strcmp(r_def, "us-east-1") != 0) {
      return false;
    }
    gs->aws.default_region = HeapDup("me-south-1");
    if (NC_getdefaults3region(plain_uri, &r_def) != NC_NOERR || !r_def ||
        std::strcmp(r_def, "me-south-1") != 0) {
      return false;
    }
    if (NC_getdefaults3region(plain_uri, nullptr) != NC_NOERR) {
      return false;
    }

    NC_s3freeprofilelist(nullptr);
    return true;
  }));

  // Child 2: NC_TEST_AWS_DIR unset falls back to gstate->home; missing
  // .aws/config logs warning while existing .aws/credentials still loads.
  TempDir home_dir("ds3_aws_home");
  ASSERT_TRUE(WriteTextFile(home_dir.child(".aws/credentials"),
                            "[homeprof]\n"
                            "aws_access_key_id = HK\n"));
  ASSERT_TRUE(RunInForkedChild([&]() -> bool {
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    ::unsetenv("NC_TEST_AWS_DIR");
    ::unsetenv(AWS_ENV_ACCESS_KEY_ID);
    ::unsetenv(AWS_ENV_SECRET_ACCESS_KEY);

    NCglobalstate* gs = NC_getglobalstate();
    ds3util_shim_clear_aws(gs);
    const absl::Cleanup cleanup_aws = [&] { ds3util_shim_clear_aws(gs); };
    std::free(gs->home);
    gs->home = HeapDup(home_dir.c_str());
    gs->aws.access_key_id = HeapDup("ONLY_ID");
    NC_s3freeprofilelist(gs->rcinfo->s3profiles);
    gs->rcinfo->s3profiles = nullptr;

    if (NC_aws_load_credentials(gs) != NC_NOERR) {
      return false;
    }
    struct AWSprofile* prof = nullptr;
    return NC_authgets3profile("homeprof", &prof) == NC_NOERR &&
           prof != nullptr;
  }));
}

TEST(Ds3utilTest, AwsLoadCredentialsRejectsMalformedIniFiles) {
  TempDir bad_dir("ds3_aws_bad");
  struct BadIniCase {
    const char* name;
    const char* body;
    bool leading_slash_cfg;
  };
  constexpr BadIniCase kBadCases[] = {
      {"expected_open_bracket", "key = val\n", false},
      {"empty_section_header", "[]\n", false},
      {"profile_keyword_without_name", "[profile]\n", false},
      {"unclosed_section_header", "[unclosed\n", false},
      {"entry_missing_equals", "[sec]\nbadkey value\n", false},
      {"entry_invalid_value_token", "[sec]\nk = [\n", false},
      {"entry_unexpected_equals", "[sec]\n=\n", false},
      {"non_leading_semicolon", "[sec]\n ;inline\n", false},
      {"leading_slash_config_file", "not_a_section = 1\n", true},
  };

  for (const BadIniCase& tc : kBadCases) {
    SCOPED_TRACE(tc.name);
    const std::string cfg_rel = std::string(tc.name) + ".ini";
    const std::filesystem::path bad_cfg = bad_dir.child(cfg_rel);
    ASSERT_TRUE(WriteTextFile(bad_cfg, tc.body));

    EXPECT_TRUE(RunInForkedChild([&]() -> bool {
      if (nc_initialize() != NC_NOERR) {
        return false;
      }
      ::unsetenv(AWS_ENV_ACCESS_KEY_ID);
      ::unsetenv(AWS_ENV_SECRET_ACCESS_KEY);
      ::setenv("NC_TEST_AWS_DIR", tc.leading_slash_cfg ? "" : bad_dir.c_str(),
               1);

      NCglobalstate* gs = NC_getglobalstate();
      ds3util_shim_clear_aws(gs);
      gs->aws.config_file =
          HeapDup(tc.leading_slash_cfg ? bad_cfg.c_str() : cfg_rel.c_str());
      const absl::Cleanup cleanup = [&] { ds3util_shim_clear_aws(gs); };

      return NC_aws_load_credentials(gs) == NC_EINVAL;
    }));
  }
}

}  // namespace
}  // namespace netcdf
