#include "autotest2/cpp/util/matchers.h"

#include <iosfwd>

#include "ogr/ogr_spatialref.h"

void PrintTo(const OGRSpatialReference& srs, std::ostream* os) {
  char* pretty_wkt = nullptr;
  srs.exportToPrettyWkt(&pretty_wkt, false);
  *os << pretty_wkt;
  free(pretty_wkt);
}

namespace autotest2 {
namespace {

class IsSameAsMatcher {
 public:
  using is_gtest_matcher = void;

  explicit IsSameAsMatcher(const OGRSpatialReference& expected)
      : expected_(expected) {}

  bool MatchAndExplain(const OGRSpatialReference& actual, std::ostream*) const {
    return actual.IsSame(&expected_);
  }

  void DescribeTo(std::ostream* os) const {
    *os << "is the same as ";
    ::PrintTo(expected_, os);
  }

  void DescribeNegationTo(std::ostream* os) const {
    *os << "is not the same as ";
    ::PrintTo(expected_, os);
  }

 private:
  const OGRSpatialReference& expected_;
};

}  // namespace

::testing::Matcher<OGRSpatialReference> IsSameAs(
    const OGRSpatialReference& expected) {
  return IsSameAsMatcher(expected);
}

}  // namespace autotest2
