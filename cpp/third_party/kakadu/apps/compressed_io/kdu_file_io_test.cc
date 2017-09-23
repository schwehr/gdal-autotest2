// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for capps/compressed_io/kdu_file_io.h.

#include "apps/compressed_io/kdu_file_io.h"

#include <memory>
#include <string>

#include "logging.h"
#include "coresys/common/kdu_messaging.h"
#include "coresys/common/kdu_ubiquitous.h"
#include "file/base/path.h"
#include "gunit.h"
#include "third_party/absl/memory/memory.h"

namespace kdu_supp {
namespace {

// TODO(schwehr): Move this to a util library and provide a gunit main that
// registers the handler.
class GlogKakaduErrorHandler : public kdu_message {
 public:
  GlogKakaduErrorHandler() : kdu_message() {}
  virtual ~GlogKakaduErrorHandler() { delete handler_; }
  virtual void put_text(const char* string) { LOG(ERROR) << string; }
  virtual void put_text(const kdu_uint16* string) { LOG(ERROR) << string; }
  virtual void flush(bool end_of_message = false) {
    if (end_of_message) {
      LOG(ERROR) << "About to throw 42.";
      throw 42;  // arbitrary
    }
  }
  static void Init() {
    CHECK(handler_ == nullptr) << "Cannot double initialized.";
    handler_ = new GlogKakaduErrorHandler();
    kdu_customize_errors(handler_);
  }

 private:
  static GlogKakaduErrorHandler* handler_;
};
GlogKakaduErrorHandler* GlogKakaduErrorHandler::handler_ = nullptr;

class Environment : public ::testing::Environment {
 public:
  virtual void SetUp() { kdu_customize_errors(new GlogKakaduErrorHandler()); }
};

REGISTER_MODULE_INITIALIZER(glog_kakadu, {
  ::testing::AddGlobalTestEnvironment(new Environment);
});

constexpr char kTestData[] =
    "cpp/third_party/kakadu/apps/compressed_io/testdata/";

TEST(KduFileIoTest, OpenNonexistentFile) {
  // This test verifies that KduFileIo has the named property.

  EXPECT_THROW(new kdu_simple_file_source("/does/not/exist"), int);

  auto input = gtl::MakeUnique<kdu_simple_file_source>();
  EXPECT_FALSE(input->exists());
  EXPECT_THROW(input->open("/does_not_exist"), int);
  EXPECT_FALSE(input->exists());

  // Operator ! is the opposite of exists().
  EXPECT_TRUE(!(*input.get()));
}

TEST(KduFileIoTest, ReadAnImage) {
  // This test verifies that KduFileIo has the named property.

  const string filepath = file::JoinPath(FLAGS_test_srcdir, string(kTestData),
                                         "byte_without_geotransform.jp2");

  kdu_simple_file_source input(filepath.c_str());
  EXPECT_TRUE(input.exists());
  EXPECT_FALSE(!input);

  EXPECT_EQ(KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE,
            input.get_capabilities());

  EXPECT_EQ(0, input.get_pos());
  EXPECT_TRUE(input.seek(1));
  EXPECT_EQ(1, input.get_pos());
  EXPECT_TRUE(input.seek(0));
  EXPECT_EQ(0, input.get_pos());

  // Verify the first few bytes.
  {
    constexpr int num_bytes = 10;
    kdu_byte buf[num_bytes + 1] = {};
    EXPECT_EQ(num_bytes, input.read(buf, num_bytes));
    EXPECT_EQ(0, buf[0]);
    constexpr kdu_byte expected[num_bytes + 1] =
        "\x00\x00\x00\x0c\x6a\x50\x20\x20\x0d\x0a";
    for (int i = 0; i < num_bytes; i++) EXPECT_EQ(buf[i], expected[i]);
  }

  ASSERT_TRUE(input.seek(0));

  // Read the entire file.
  constexpr int expected_bytes = 182;
  constexpr int num_bytes = expected_bytes + 10;
  kdu_byte buf[num_bytes + 1] = {};
  EXPECT_EQ(expected_bytes, input.read(buf, num_bytes));
}

// TODO(schwehr): Test the rest of kdu_file_io.h.

}  // namespace
}  // namespace kdu_supp
