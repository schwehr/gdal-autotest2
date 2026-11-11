// Tests for third_party/kakadu/vendor/coresys/compressed/codestream.cpp.

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"

namespace kdu_core {
namespace {

TEST(KduThreadsTest, Create) {
  kdu_thread_env env;
  env.create();

  for (int i=0; i < 10; i++) {
    EXPECT_TRUE(env.add_thread());
  }
  env.destroy();
}

// TODO(b/309473610): More tests please!

}  // namespace
}  // namespace kdu_core
