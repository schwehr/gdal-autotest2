#ifndef THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_MATCHERS_H_
#define THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_MATCHERS_H_

#include <iosfwd>

#include "gunit.h"
#include "ogr/ogr_spatialref.h"

// This must be defined in the same namespace as `OGRSpatialReference` (the root
// namespace); see
// go/gunitadvanced#teaching-googletest-how-to-print-your-values.
void PrintTo(const OGRSpatialReference& srs, std::ostream* os);

namespace autotest2 {

// A matcher that compares `expected` to another `OGRSpatialReference` via
// `OGRSpatialReference::IsSame`.
::testing::Matcher<OGRSpatialReference> IsSameAs(
    const OGRSpatialReference& expected);

}  // namespace autotest2

#endif  // THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_MATCHERS_H_
