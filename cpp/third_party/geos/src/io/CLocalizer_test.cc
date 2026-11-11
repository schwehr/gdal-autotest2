// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/io/CLocalizer.h"

#include <array>
#include <clocale>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

namespace geos_google {
namespace {

std::string CurrentLocale(int category) {
  const char* loc = std::setlocale(category, nullptr);
  return loc != nullptr ? std::string(loc) : std::string();
}

std::string CurrentDecimalPoint() {
  const std::lconv* lc = std::localeconv();
  return (lc != nullptr && lc->decimal_point != nullptr)
             ? std::string(lc->decimal_point)
             : std::string();
}

std::string FormatDouble(double value) {
  std::array<char, 64> buf{};
  const int len = std::snprintf(buf.data(), buf.size(), "%.2f", value);
  if (len <= 0 || len >= static_cast<int>(buf.size())) {
    return std::string();
  }
  return std::string(buf.data(), static_cast<std::size_t>(len));
}

std::string TrySetNonCNumericLocale(const std::string& fallback_locale) {
  static constexpr std::array<const char*, 8> kCandidates = {
      "de_DE.UTF-8", "fr_FR.UTF-8", "es_ES.UTF-8", "it_IT.UTF-8",
      "C.UTF-8",     "C.utf8",      "en_US.UTF-8", "",
  };
  for (const char* candidate : kCandidates) {
    const char* res = std::setlocale(LC_NUMERIC, candidate);
    if (res != nullptr && std::string(res) != "C") {
      return std::string(res);
    }
  }
  std::setlocale(LC_NUMERIC, fallback_locale.c_str());
  return std::string();
}

TEST(CLocalizerTest, SetsNumericLocaleToCAndRestoresInitialLocale) {
  const std::string initial_locale = CurrentLocale(LC_NUMERIC);
  ASSERT_FALSE(initial_locale.empty());
  absl::Cleanup restore = [&initial_locale]() {
    std::setlocale(LC_NUMERIC, initial_locale.c_str());
  };

  {
    geos::io::CLocalizer localizer;
    EXPECT_EQ(CurrentLocale(LC_NUMERIC), "C");
    EXPECT_EQ(CurrentDecimalPoint(), ".");
    EXPECT_EQ(FormatDouble(1.25), "1.25");

    char* end = nullptr;
    EXPECT_DOUBLE_EQ(std::strtod("2.75", &end), 2.75);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(*end, '\0');
  }

  EXPECT_EQ(CurrentLocale(LC_NUMERIC), initial_locale);
}

TEST(CLocalizerTest, RestoresNonCLocaleWhenAvailable) {
  const std::string initial_locale = CurrentLocale(LC_NUMERIC);
  ASSERT_FALSE(initial_locale.empty());
  absl::Cleanup restore = [&initial_locale]() {
    std::setlocale(LC_NUMERIC, initial_locale.c_str());
  };

  const std::string non_c_locale = TrySetNonCNumericLocale(initial_locale);
  if (!non_c_locale.empty()) {
    const std::string pre_decimal = CurrentDecimalPoint();
    const std::string pre_formatted = FormatDouble(1.25);

    {
      geos::io::CLocalizer localizer;
      EXPECT_EQ(CurrentLocale(LC_NUMERIC), "C");
      EXPECT_EQ(CurrentDecimalPoint(), ".");
      EXPECT_EQ(FormatDouble(1.25), "1.25");

      char* end = nullptr;
      EXPECT_DOUBLE_EQ(std::strtod("3.50", &end), 3.50);
      ASSERT_NE(end, nullptr);
      EXPECT_EQ(*end, '\0');
    }

    EXPECT_EQ(CurrentLocale(LC_NUMERIC), non_c_locale);
    EXPECT_EQ(CurrentDecimalPoint(), pre_decimal);
    EXPECT_EQ(FormatDouble(1.25), pre_formatted);
  } else {
    // Only "C"/"POSIX" locales are installed in the test environment.
    ASSERT_NE(std::setlocale(LC_NUMERIC, "POSIX"), nullptr);
    const std::string posix_name = CurrentLocale(LC_NUMERIC);
    {
      geos::io::CLocalizer localizer;
      EXPECT_EQ(CurrentLocale(LC_NUMERIC), "C");
      EXPECT_EQ(FormatDouble(1.25), "1.25");
    }
    EXPECT_EQ(CurrentLocale(LC_NUMERIC), posix_name);
  }
}

TEST(CLocalizerTest, RestoresLocaleOnExceptionUnwinding) {
  const std::string initial_locale = CurrentLocale(LC_NUMERIC);
  ASSERT_FALSE(initial_locale.empty());
  absl::Cleanup restore = [&initial_locale]() {
    std::setlocale(LC_NUMERIC, initial_locale.c_str());
  };

  const std::string non_c_locale = TrySetNonCNumericLocale(initial_locale);
  const std::string expected_restored =
      non_c_locale.empty() ? initial_locale : non_c_locale;
  ASSERT_EQ(CurrentLocale(LC_NUMERIC), expected_restored);

  try {
    geos::io::CLocalizer localizer;
    EXPECT_EQ(CurrentLocale(LC_NUMERIC), "C");
    throw std::runtime_error("simulated parse failure");
  } catch (const std::runtime_error&) {
  }

  EXPECT_EQ(CurrentLocale(LC_NUMERIC), expected_restored);
}

TEST(CLocalizerTest, NestedAndHeapAllocatedLocalizersRestoreCorrectly) {
  const std::string initial_locale = CurrentLocale(LC_NUMERIC);
  ASSERT_FALSE(initial_locale.empty());
  absl::Cleanup restore = [&initial_locale]() {
    std::setlocale(LC_NUMERIC, initial_locale.c_str());
  };

  const std::string non_c_locale = TrySetNonCNumericLocale(initial_locale);
  const std::string outer_expected =
      non_c_locale.empty() ? initial_locale : non_c_locale;

  auto outer = std::make_unique<geos::io::CLocalizer>();
  EXPECT_EQ(CurrentLocale(LC_NUMERIC), "C");

  {
    geos::io::CLocalizer inner;
    EXPECT_EQ(CurrentLocale(LC_NUMERIC), "C");
    EXPECT_EQ(FormatDouble(3.50), "3.50");
  }

  EXPECT_EQ(CurrentLocale(LC_NUMERIC), "C");
  outer.reset();
  EXPECT_EQ(CurrentLocale(LC_NUMERIC), outer_expected);
}

TEST(CLocalizerTest, DoesNotModifyOtherLocaleCategories) {
  const std::string initial_numeric = CurrentLocale(LC_NUMERIC);
  const std::string initial_ctype = CurrentLocale(LC_CTYPE);
  ASSERT_FALSE(initial_numeric.empty());
  ASSERT_FALSE(initial_ctype.empty());
  absl::Cleanup restore = [&initial_numeric]() {
    std::setlocale(LC_NUMERIC, initial_numeric.c_str());
  };

  {
    geos::io::CLocalizer localizer;
    EXPECT_EQ(CurrentLocale(LC_NUMERIC), "C");
    EXPECT_EQ(CurrentLocale(LC_CTYPE), initial_ctype);
  }

  EXPECT_EQ(CurrentLocale(LC_NUMERIC), initial_numeric);
  EXPECT_EQ(CurrentLocale(LC_CTYPE), initial_ctype);
}

}  // namespace
}  // namespace geos_google
