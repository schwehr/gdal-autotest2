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
// Unit tests for CLI numeric printf format validation and bounded formatting
// helpers (proj/src/apps/utils.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/apps/utils.cpp
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/apps/utils.h

#include "third_party/proj4/proj/src/apps/utils.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/temp_file.h"

namespace proj {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;

struct FileCloser {
  void operator()(std::FILE* fp) const noexcept {
    if (fp != nullptr) {
      std::fclose(fp);
    }
  }
};

using ScopedFile = std::unique_ptr<std::FILE, FileCloser>;

// Redirects STDERR_FILENO to a scoped TempFile under ::testing::TempDir() so
// stderr diagnostics from limited_fprintf_for_number() are captured cleanly
// without touching /tmp.
class ScopedStderrCapture {
 public:
  explicit ScopedStderrCapture(std::string_view temp_filename)
      : temp_(temp_filename) {
    std::fflush(stderr);
    const int fd = ::open(temp_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
      return;
    }
    saved_stderr_fd_ = ::dup(STDERR_FILENO);
    if (saved_stderr_fd_ < 0) {
      ::close(fd);
      return;
    }
    if (::dup2(fd, STDERR_FILENO) < 0) {
      ::close(saved_stderr_fd_);
      saved_stderr_fd_ = -1;
    }
    ::close(fd);
  }

  ~ScopedStderrCapture() { Restore(); }

  ScopedStderrCapture(const ScopedStderrCapture&) = delete;
  ScopedStderrCapture& operator=(const ScopedStderrCapture&) = delete;

  bool is_active() const noexcept { return saved_stderr_fd_ >= 0; }

  std::string StopAndRead() {
    Restore();
    std::ifstream ifs(temp_.path(), std::ios::binary);
    if (!ifs) {
      return {};
    }
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
  }

 private:
  void Restore() {
    if (saved_stderr_fd_ >= 0) {
      std::fflush(stderr);
      ::dup2(saved_stderr_fd_, STDERR_FILENO);
      ::close(saved_stderr_fd_);
      saved_stderr_fd_ = -1;
    }
  }

  TempFile temp_;
  int saved_stderr_fd_ = -1;
};

struct FormatInvocationResult {
  std::string file_output;
  std::string stderr_output;
};

FormatInvocationResult RunLimitedFprintf(std::string_view tag,
                                         const char* format_string,
                                         double value) {
  const TempFile out_temp(std::string("utils_test/") + std::string(tag) +
                          "_out.txt");
  ScopedFile file(std::fopen(out_temp.c_str(), "wb+"));
  EXPECT_NE(nullptr, file.get());
  if (file == nullptr) {
    return {};
  }

  ScopedStderrCapture stderr_capture(std::string("utils_test/") +
                                     std::string(tag) + "_err.txt");
  EXPECT_TRUE(stderr_capture.is_active());
  limited_fprintf_for_number(file.get(), format_string, value);
  std::string err = stderr_capture.StopAndRead();

  std::fflush(file.get());
  std::fseek(file.get(), 0, SEEK_SET);
  std::string out;
  char buf[512];
  while (true) {
    const std::size_t n = std::fread(buf, 1, sizeof(buf), file.get());
    if (n == 0) {
      break;
    }
    out.append(buf, n);
  }
  return {.file_output = std::move(out), .stderr_output = std::move(err)};
}

TEST(UtilsTest, ValidateFormStringAcceptsAllSupportedSpecifiers) {
  constexpr const char* kValidFormats[] = {
      // Bare specifiers (length 2; exercises all 6 short-circuit branches).
      "%e",
      "%E",
      "%f",
      "%F",
      "%g",
      "%G",
      // '+' flag, width boundaries ('0' and '9'), dot, and precision.
      "%+e",
      "%+E",
      "%+f",
      "%+F",
      "%+g",
      "%+G",
      "%0f",
      "%9f",
      "%08f",
      "%10f",
      "%+012f",
      "%.f",
      "%.0f",
      "%.3e",
      "%.9G",
      "%+.4F",
      "%8.2f",
      "%08.2f",
      "%+012.4G",
      "%10.e",
      // validate_form_string_for_numbers only checks character classes, so
      // malformed ordering / large widths pass here even though
      // limited_fprintf_for_number rejects them.
      "%..2f",
      "%1+2f",
      "%1001f",
  };

  for (const char* fmt : kValidFormats) {
    EXPECT_TRUE(validate_form_string_for_numbers(fmt)) << fmt;
  }
}

TEST(UtilsTest, ValidateFormStringRejectsMalformedFormats) {
  // Note: passing nullptr is undefined behavior because upstream indexes
  // formatString[0] without a null check.
  constexpr const char* kInvalidFormats[] = {
      "",    "f",   "10.2f", " %f",    "%",   "%-10f", "% f", "%#g",
      "%*f", "%lf", "%%f",   "%10a2f", "%/f", "%:f",   "%d",  "%i",
      "%s",  "%x",  "%10",   "%+",     "%.",  "%10.2",
  };

  for (const char* fmt : kInvalidFormats) {
    EXPECT_FALSE(validate_form_string_for_numbers(fmt)) << fmt;
  }
}

TEST(UtilsTest, LimitedFprintfFormatsAllSpecifiersAndModifiers) {
  struct FormatCase {
    const char* format;
    double value;
    const char* expected;
  };
  constexpr FormatCase kCases[] = {
      // 1. Bare specifiers (MY_FPRINTF("", val)), with and without '+'.
      {.format = "%f", .value = 1.25, .expected = "1.250000"},
      {.format = "%+f", .value = 1.25, .expected = "+1.250000"},
      {.format = "%f", .value = -1.25, .expected = "-1.250000"},
      {.format = "%+f", .value = -1.25, .expected = "-1.250000"},
      {.format = "%F", .value = 1.25, .expected = "1.250000"},
      {.format = "%+F", .value = 1.25, .expected = "+1.250000"},
      {.format = "%e", .value = 1234.5, .expected = "1.234500e+03"},
      {.format = "%+E", .value = 1234.5, .expected = "+1.234500E+03"},
      {.format = "%g", .value = 12.5, .expected = "12.5"},
      {.format = "%+G", .value = 1234567.0, .expected = "+1.23457E+06"},
      // 2. Dot without precision (MY_FPRINTF(".", val)), with and without '+'.
      {.format = "%.f", .value = 12.75, .expected = "13"},
      {.format = "%+.f", .value = 12.75, .expected = "+13"},
      {.format = "%.g", .value = 5.0, .expected = "5"},
      {.format = "%+.G", .value = 5.0, .expected = "+5"},
      // 3. Dot + precision (MY_FPRINTF(".*", p, val)), with and without '+'.
      {.format = "%.3f", .value = 1.25, .expected = "1.250"},
      {.format = "%+.3f", .value = 1.25, .expected = "+1.250"},
      {.format = "%.2e", .value = 1234.5, .expected = "1.23e+03"},
      {.format = "%+.2E", .value = 1234.5, .expected = "+1.23E+03"},
      {.format = "%.3g", .value = 1234.5, .expected = "1.23e+03"},
      {.format = "%+.3G", .value = 1234.5, .expected = "+1.23E+03"},
      {.format = "%.1f", .value = -0.0, .expected = "-0.0"},
      // 4. Width without dot (MY_FPRINTF("*", w, val) and
      //    MY_FPRINTF("0*", w, val)).
      {.format = "%10f", .value = 1.5, .expected = "  1.500000"},
      {.format = "%+10f", .value = 1.5, .expected = " +1.500000"},
      {.format = "%010f", .value = 1.5, .expected = "001.500000"},
      {.format = "%+010f", .value = 1.5, .expected = "+01.500000"},
      // 5. Width + dot without precision ("*." and "0*.").
      {.format = "%8.f", .value = 12.75, .expected = "      13"},
      {.format = "%+8.f", .value = 12.75, .expected = "     +13"},
      {.format = "%08.f", .value = 12.75, .expected = "00000013"},
      {.format = "%+08.f", .value = 12.75, .expected = "+0000013"},
      {.format = "%10.e", .value = 12.75, .expected = "     1e+01"},
      {.format = "%010.E", .value = 12.75, .expected = "000001E+01"},
      // 6. Width + dot + precision ("*.*" and "0*.*").
      {.format = "%8.2f", .value = 1.25, .expected = "    1.25"},
      {.format = "%+8.2f", .value = 1.25, .expected = "   +1.25"},
      {.format = "%08.2f", .value = 1.25, .expected = "00001.25"},
      {.format = "%+08.2f", .value = 1.25, .expected = "+0001.25"},
      {.format = "%+010.2E", .value = 1234.5, .expected = "+01.23E+03"},
  };

  for (const auto& tc : kCases) {
    const FormatInvocationResult res =
        RunLimitedFprintf("valid", tc.format, tc.value);
    EXPECT_EQ(tc.expected, res.file_output) << tc.format;
    EXPECT_THAT(res.stderr_output, IsEmpty()) << tc.format;
  }
}

TEST(UtilsTest, LimitedFprintfHandlesNonFiniteValuesAndMax1000Bound) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  // Uppercase '%F' vs lowercase '%f' on non-finite doubles.
  const FormatInvocationResult inf_lower =
      RunLimitedFprintf("inf_l", "%f", kInf);
  EXPECT_EQ("inf", inf_lower.file_output);
  EXPECT_THAT(inf_lower.stderr_output, IsEmpty());

  const FormatInvocationResult inf_neg =
      RunLimitedFprintf("inf_neg", "%f", -kInf);
  EXPECT_EQ("-inf", inf_neg.file_output);
  EXPECT_THAT(inf_neg.stderr_output, IsEmpty());

  const FormatInvocationResult inf_upper =
      RunLimitedFprintf("inf_u", "%+F", kInf);
  EXPECT_EQ("+INF", inf_upper.file_output);
  EXPECT_THAT(inf_upper.stderr_output, IsEmpty());

  const FormatInvocationResult nan_lower =
      RunLimitedFprintf("nan_l", "%f", kNan);
  EXPECT_THAT(nan_lower.file_output, HasSubstr("nan"));
  EXPECT_THAT(nan_lower.stderr_output, IsEmpty());

  const FormatInvocationResult nan_upper =
      RunLimitedFprintf("nan_u", "%F", kNan);
  EXPECT_THAT(nan_upper.file_output, HasSubstr("NAN"));
  EXPECT_THAT(nan_upper.stderr_output, IsEmpty());

  // parseInt allows widths and precisions up to 1000 inclusive.
  const FormatInvocationResult width_1000 =
      RunLimitedFprintf("w1000", "%1000.1f", 1.5);
  EXPECT_EQ(std::string(997, ' ') + "1.5", width_1000.file_output);
  EXPECT_THAT(width_1000.stderr_output, IsEmpty());

  const FormatInvocationResult prec_1000 =
      RunLimitedFprintf("p1000", "%.1000f", 0.0);
  EXPECT_EQ("0." + std::string(1000, '0'), prec_1000.file_output);
  EXPECT_THAT(prec_1000.stderr_output, IsEmpty());
}

TEST(UtilsTest, LimitedFprintfRejectsInvalidFormatsAndWritesStderrDiagnostic) {
  struct InvalidCase {
    const char* format;
    double value;
    const char* expected_prefix_written;
    const char* expected_stderr;
  };
  constexpr InvalidCase kCases[] = {
      // Missing leading '%'.
      {
          .format = "",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString ''\n",
      },
      {
          .format = "10.2f",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '10.2f'\n",
      },
      // Width > 1000 or truncated right after width.
      {
          .format = "%1001f",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%1001f'\n",
      },
      {
          .format = "%01001f",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%01001f'\n",
      },
      {
          .format = "%12",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%12'\n",
      },
      // Precision > 1000 or truncated right after width.precision.
      {
          .format = "%8.1001f",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%8.1001f'\n",
      },
      {
          .format = "%8.3",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%8.3'\n",
      },
      // Precision > 1000 or truncated right after .precision without width.
      {
          .format = "%.1001f",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%.1001f'\n",
      },
      {
          .format = "%.3",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%.3'\n",
      },
      // Invalid or missing specifier caught inside MY_FPRINTF0's else branch.
      {
          .format = "%",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%'\n",
      },
      {
          .format = "%+",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%+'\n",
      },
      {
          .format = "%d",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%d'\n",
      },
      {
          .format = "%.",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%.'\n",
      },
      {
          .format = "%8.",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%8.'\n",
      },
      {
          .format = "%8.d",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%8.d'\n",
      },
      {
          .format = "%8d",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%8d'\n",
      },
      {
          .format = "%8.2d",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%8.2d'\n",
      },
      {
          .format = "%.2d",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%.2d'\n",
      },
      {
          .format = "%1+2f",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%1+2f'\n",
      },
      {
          .format = "%..2f",
          .value = 1.5,
          .expected_prefix_written = "",
          .expected_stderr = "Wrong formatString '%..2f'\n",
      },
      // Trailing characters after valid specifier: MY_FPRINTF writes the value
      // before limited_fprintf_for_number checks *ptr != 0.
      {
          .format = "%f_extra",
          .value = 1.5,
          .expected_prefix_written = "1.500000",
          .expected_stderr = "Wrong formatString '%f_extra'\n",
      },
      {
          .format = "%+8.2g!",
          .value = 1.5,
          .expected_prefix_written = "    +1.5",
          .expected_stderr = "Wrong formatString '%+8.2g!'\n",
      },
      {
          .format = "%.f?",
          .value = 1.25,
          .expected_prefix_written = "1",
          .expected_stderr = "Wrong formatString '%.f?'\n",
      },
  };

  for (const auto& tc : kCases) {
    const FormatInvocationResult res =
        RunLimitedFprintf("invalid", tc.format, tc.value);
    EXPECT_EQ(tc.expected_prefix_written, res.file_output) << tc.format;
    EXPECT_EQ(tc.expected_stderr, res.stderr_output) << tc.format;
  }
}

}  // namespace
}  // namespace proj
