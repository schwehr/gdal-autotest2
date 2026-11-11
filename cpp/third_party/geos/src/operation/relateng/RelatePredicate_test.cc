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

#include "third_party/geos/include/geos/operation/relateng/RelatePredicate.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/BasicPredicate.h"
#include "third_party/geos/include/geos/operation/relateng/IMPredicate.h"
#include "third_party/geos/include/geos/operation/relateng/RelateGeometry.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNG.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyPredicate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::relateng::BasicPredicate;
using geos::operation::relateng::IMPredicate;
using geos::operation::relateng::RelateGeometry;
using geos::operation::relateng::RelateNG;
using geos::operation::relateng::RelatePredicate;
using geos::operation::relateng::TopologyPredicate;

// TopologyPredicate exposes requireCovers(bool) and init(Envelope, Envelope)
// publicly, whereas BasicPredicate marks requireCovers(bool) protected and
// IMPredicate hides the Envelope overload of init() with init(int, int).
TopologyPredicate& AsTopologyPredicate(TopologyPredicate& pred) { return pred; }

class RelatePredicateTest : public ::testing::Test {
 protected:
  RelatePredicateTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(RelatePredicateTest, FactoryMetadataAndRequirementFlags) {
  constexpr bool kGeomA = RelateGeometry::GEOM_A;
  constexpr bool kGeomB = RelateGeometry::GEOM_B;

  std::unique_ptr<BasicPredicate> p_intersects = RelatePredicate::intersects();
  EXPECT_EQ(p_intersects->name(), "intersects");
  EXPECT_FALSE(p_intersects->requireSelfNoding());
  EXPECT_TRUE(p_intersects->requireInteraction());
  EXPECT_FALSE(AsTopologyPredicate(*p_intersects).requireCovers(kGeomA));
  EXPECT_FALSE(AsTopologyPredicate(*p_intersects).requireCovers(kGeomB));
  EXPECT_FALSE(p_intersects->requireExteriorCheck(kGeomA));
  EXPECT_FALSE(p_intersects->requireExteriorCheck(kGeomB));

  std::unique_ptr<BasicPredicate> p_disjoint = RelatePredicate::disjoint();
  EXPECT_EQ(p_disjoint->name(), "disjoint");
  EXPECT_FALSE(p_disjoint->requireSelfNoding());
  EXPECT_FALSE(p_disjoint->requireInteraction());
  EXPECT_FALSE(AsTopologyPredicate(*p_disjoint).requireCovers(kGeomA));
  EXPECT_FALSE(AsTopologyPredicate(*p_disjoint).requireCovers(kGeomB));
  EXPECT_FALSE(p_disjoint->requireExteriorCheck(kGeomA));
  EXPECT_FALSE(p_disjoint->requireExteriorCheck(kGeomB));

  std::unique_ptr<IMPredicate> p_contains = RelatePredicate::contains();
  EXPECT_EQ(p_contains->name(), "contains");
  EXPECT_TRUE(p_contains->requireSelfNoding());
  EXPECT_TRUE(p_contains->requireInteraction());
  EXPECT_TRUE(AsTopologyPredicate(*p_contains).requireCovers(kGeomA));
  EXPECT_FALSE(AsTopologyPredicate(*p_contains).requireCovers(kGeomB));
  EXPECT_FALSE(p_contains->requireExteriorCheck(kGeomA));
  EXPECT_TRUE(p_contains->requireExteriorCheck(kGeomB));

  std::unique_ptr<IMPredicate> p_within = RelatePredicate::within();
  EXPECT_EQ(p_within->name(), "within");
  EXPECT_TRUE(p_within->requireSelfNoding());
  EXPECT_TRUE(p_within->requireInteraction());
  EXPECT_FALSE(AsTopologyPredicate(*p_within).requireCovers(kGeomA));
  EXPECT_TRUE(AsTopologyPredicate(*p_within).requireCovers(kGeomB));
  EXPECT_TRUE(p_within->requireExteriorCheck(kGeomA));
  EXPECT_FALSE(p_within->requireExteriorCheck(kGeomB));

  std::unique_ptr<IMPredicate> p_covers = RelatePredicate::covers();
  EXPECT_EQ(p_covers->name(), "covers");
  EXPECT_TRUE(p_covers->requireSelfNoding());
  EXPECT_TRUE(p_covers->requireInteraction());
  EXPECT_TRUE(AsTopologyPredicate(*p_covers).requireCovers(kGeomA));
  EXPECT_FALSE(AsTopologyPredicate(*p_covers).requireCovers(kGeomB));
  EXPECT_FALSE(p_covers->requireExteriorCheck(kGeomA));
  EXPECT_TRUE(p_covers->requireExteriorCheck(kGeomB));

  std::unique_ptr<IMPredicate> p_covered_by = RelatePredicate::coveredBy();
  EXPECT_EQ(p_covered_by->name(), "coveredBy");
  EXPECT_TRUE(p_covered_by->requireSelfNoding());
  EXPECT_TRUE(p_covered_by->requireInteraction());
  EXPECT_FALSE(AsTopologyPredicate(*p_covered_by).requireCovers(kGeomA));
  EXPECT_TRUE(AsTopologyPredicate(*p_covered_by).requireCovers(kGeomB));
  EXPECT_TRUE(p_covered_by->requireExteriorCheck(kGeomA));
  EXPECT_FALSE(p_covered_by->requireExteriorCheck(kGeomB));

  std::unique_ptr<IMPredicate> p_crosses = RelatePredicate::crosses();
  EXPECT_EQ(p_crosses->name(), "crosses");
  EXPECT_TRUE(p_crosses->requireSelfNoding());
  EXPECT_TRUE(p_crosses->requireInteraction());
  EXPECT_FALSE(AsTopologyPredicate(*p_crosses).requireCovers(kGeomA));
  EXPECT_FALSE(AsTopologyPredicate(*p_crosses).requireCovers(kGeomB));
  EXPECT_TRUE(p_crosses->requireExteriorCheck(kGeomA));
  EXPECT_TRUE(p_crosses->requireExteriorCheck(kGeomB));

  std::unique_ptr<IMPredicate> p_equals = RelatePredicate::equalsTopo();
  EXPECT_EQ(p_equals->name(), "equals");
  EXPECT_TRUE(p_equals->requireSelfNoding());
  EXPECT_FALSE(p_equals->requireInteraction());
  EXPECT_FALSE(AsTopologyPredicate(*p_equals).requireCovers(kGeomA));
  EXPECT_FALSE(AsTopologyPredicate(*p_equals).requireCovers(kGeomB));
  EXPECT_TRUE(p_equals->requireExteriorCheck(kGeomA));
  EXPECT_TRUE(p_equals->requireExteriorCheck(kGeomB));

  std::unique_ptr<IMPredicate> p_overlaps = RelatePredicate::overlaps();
  EXPECT_EQ(p_overlaps->name(), "overlaps");
  EXPECT_TRUE(p_overlaps->requireSelfNoding());
  EXPECT_TRUE(p_overlaps->requireInteraction());
  EXPECT_FALSE(AsTopologyPredicate(*p_overlaps).requireCovers(kGeomA));
  EXPECT_FALSE(AsTopologyPredicate(*p_overlaps).requireCovers(kGeomB));
  EXPECT_TRUE(p_overlaps->requireExteriorCheck(kGeomA));
  EXPECT_TRUE(p_overlaps->requireExteriorCheck(kGeomB));

  std::unique_ptr<IMPredicate> p_touches = RelatePredicate::touches();
  EXPECT_EQ(p_touches->name(), "touches");
  EXPECT_TRUE(p_touches->requireSelfNoding());
  EXPECT_TRUE(p_touches->requireInteraction());
  EXPECT_FALSE(AsTopologyPredicate(*p_touches).requireCovers(kGeomA));
  EXPECT_FALSE(AsTopologyPredicate(*p_touches).requireCovers(kGeomB));
  EXPECT_TRUE(p_touches->requireExteriorCheck(kGeomA));
  EXPECT_TRUE(p_touches->requireExteriorCheck(kGeomB));

  std::unique_ptr<TopologyPredicate> p_matches_disjoint =
      RelatePredicate::matches("FF*FF****");
  EXPECT_EQ(p_matches_disjoint->name(), "IMPattern");
  EXPECT_TRUE(p_matches_disjoint->requireSelfNoding());
  EXPECT_FALSE(p_matches_disjoint->requireInteraction());
  EXPECT_FALSE(p_matches_disjoint->requireCovers(kGeomA));
  EXPECT_FALSE(p_matches_disjoint->requireCovers(kGeomB));
  EXPECT_TRUE(p_matches_disjoint->requireExteriorCheck(kGeomA));
  EXPECT_TRUE(p_matches_disjoint->requireExteriorCheck(kGeomB));

  std::unique_ptr<TopologyPredicate> p_matches_interact =
      RelatePredicate::matches("T********");
  EXPECT_TRUE(p_matches_interact->requireInteraction());

  std::ostringstream oss_base;
  oss_base << AsTopologyPredicate(*p_intersects);
  EXPECT_EQ(oss_base.str(), "intersects");

  std::ostringstream oss_im;
  oss_im << *p_contains;
  EXPECT_FALSE(oss_im.str().empty());
}

TEST_F(RelatePredicateTest, IntersectsAndDisjointStateMachine) {
  const Envelope env_a(0.0, 10.0, 0.0, 10.0);
  const Envelope env_overlap(5.0, 15.0, 5.0, 15.0);
  const Envelope env_far(20.0, 30.0, 20.0, 30.0);
  const Envelope env_null;

  // Disjoint or null envelopes determine both predicates in init().
  {
    auto pred = RelatePredicate::intersects();
    pred->init(env_a, env_far);
    EXPECT_TRUE(pred->isKnown());
    EXPECT_FALSE(pred->value());
  }
  {
    auto pred = RelatePredicate::intersects();
    pred->init(env_a, env_null);
    EXPECT_TRUE(pred->isKnown());
    EXPECT_FALSE(pred->value());
  }
  {
    auto pred = RelatePredicate::disjoint();
    pred->init(env_a, env_far);
    EXPECT_TRUE(pred->isKnown());
    EXPECT_TRUE(pred->value());
  }
  {
    auto pred = RelatePredicate::disjoint();
    pred->init(env_null, env_a);
    EXPECT_TRUE(pred->isKnown());
    EXPECT_TRUE(pred->value());
  }

  // Overlapping envelopes with exterior-only update remain unknown until
  // finish.
  {
    auto pred = RelatePredicate::intersects();
    pred->init(Dimension::A, Dimension::A);
    pred->init(env_a, env_overlap);
    EXPECT_FALSE(pred->isKnown());
    pred->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::A);
    pred->updateDimension(Location::EXTERIOR, Location::EXTERIOR, Dimension::A);
    EXPECT_FALSE(pred->isKnown());
    pred->finish();
    EXPECT_TRUE(pred->isKnown());
    EXPECT_FALSE(pred->value());
  }
  {
    auto pred = RelatePredicate::disjoint();
    pred->init(Dimension::A, Dimension::L);
    pred->init(env_a, env_overlap);
    EXPECT_FALSE(pred->isKnown());
    pred->updateDimension(Location::EXTERIOR, Location::BOUNDARY, Dimension::L);
    pred->updateDimension(Location::EXTERIOR, Location::EXTERIOR, Dimension::A);
    EXPECT_FALSE(pred->isKnown());
    pred->finish();
    EXPECT_TRUE(pred->isKnown());
    EXPECT_TRUE(pred->value());
  }

  // Any non-exterior/non-exterior interaction short-circuits immediately and is
  // preserved across finish().
  {
    auto pred = RelatePredicate::intersects();
    pred->init(env_a, env_overlap);
    pred->updateDimension(Location::BOUNDARY, Location::INTERIOR, Dimension::P);
    EXPECT_TRUE(pred->isKnown());
    EXPECT_TRUE(pred->value());
    pred->finish();
    EXPECT_TRUE(pred->value());
  }
  {
    auto pred = RelatePredicate::disjoint();
    pred->init(env_a, env_overlap);
    pred->updateDimension(Location::INTERIOR, Location::BOUNDARY, Dimension::P);
    EXPECT_TRUE(pred->isKnown());
    EXPECT_FALSE(pred->value());
    pred->finish();
    EXPECT_FALSE(pred->value());
  }
}

TEST_F(RelatePredicateTest, ContainsWithinCoversCoveredByStateMachine) {
  const Envelope big(0.0, 10.0, 0.0, 10.0);
  const Envelope small(2.0, 8.0, 2.0, 8.0);
  const Envelope shifted(5.0, 15.0, 5.0, 15.0);
  const Envelope env_null;

  // Dimension incompatibility.
  {
    auto p = RelatePredicate::contains();
    p->init(Dimension::P, Dimension::A);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::within();
    p->init(Dimension::A, Dimension::L);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::covers();
    p->init(Dimension::False, Dimension::P);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::coveredBy();
    p->init(Dimension::L, Dimension::False);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }

  // Point vs zero-length LineString dimension compatibility is allowed in init.
  {
    auto c = RelatePredicate::contains();
    c->init(Dimension::P, Dimension::L);
    EXPECT_FALSE(c->isKnown());

    auto w = RelatePredicate::within();
    w->init(Dimension::L, Dimension::P);
    EXPECT_FALSE(w->isKnown());

    auto cov = RelatePredicate::covers();
    cov->init(Dimension::P, Dimension::L);
    EXPECT_FALSE(cov->isKnown());

    auto cb = RelatePredicate::coveredBy();
    cb->init(Dimension::L, Dimension::P);
    EXPECT_FALSE(cb->isKnown());
  }

  // Envelope non-cover (and null envelope) short-circuits.
  {
    auto p = RelatePredicate::contains();
    p->init(Dimension::A, Dimension::A);
    AsTopologyPredicate(*p).init(big, shifted);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::contains();
    p->init(Dimension::A, Dimension::False);
    AsTopologyPredicate(*p).init(big, env_null);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::within();
    p->init(Dimension::A, Dimension::A);
    AsTopologyPredicate(*p).init(big, small);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::covers();
    p->init(Dimension::A, Dimension::A);
    AsTopologyPredicate(*p).init(small, big);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::coveredBy();
    p->init(Dimension::A, Dimension::A);
    AsTopologyPredicate(*p).init(big, small);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }

  // Exterior intersection short-circuits.
  {
    auto p = RelatePredicate::contains();
    p->init(Dimension::A, Dimension::L);
    AsTopologyPredicate(*p).init(big, small);
    p->updateDimension(Location::EXTERIOR, Location::INTERIOR, Dimension::L);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::within();
    p->init(Dimension::L, Dimension::A);
    AsTopologyPredicate(*p).init(small, big);
    p->updateDimension(Location::BOUNDARY, Location::EXTERIOR, Dimension::P);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::covers();
    p->init(Dimension::A, Dimension::L);
    AsTopologyPredicate(*p).init(big, small);
    p->updateDimension(Location::EXTERIOR, Location::BOUNDARY, Dimension::P);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::coveredBy();
    p->init(Dimension::L, Dimension::A);
    AsTopologyPredicate(*p).init(small, big);
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::L);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }

  // Boundary-only intersection distinguishes contains/within from
  // covers/coveredBy.
  {
    auto c = RelatePredicate::contains();
    c->init(Dimension::A, Dimension::L);
    AsTopologyPredicate(*c).init(big, small);
    c->updateDimension(Location::BOUNDARY, Location::INTERIOR, Dimension::L);
    EXPECT_FALSE(c->isKnown());
    c->finish();
    EXPECT_FALSE(c->value());

    auto cov = RelatePredicate::covers();
    cov->init(Dimension::A, Dimension::L);
    AsTopologyPredicate(*cov).init(big, small);
    cov->updateDimension(Location::BOUNDARY, Location::INTERIOR, Dimension::L);
    EXPECT_FALSE(cov->isKnown());
    cov->finish();
    EXPECT_TRUE(cov->value());

    auto w = RelatePredicate::within();
    w->init(Dimension::L, Dimension::A);
    AsTopologyPredicate(*w).init(small, big);
    w->updateDimension(Location::INTERIOR, Location::BOUNDARY, Dimension::L);
    EXPECT_FALSE(w->isKnown());
    w->finish();
    EXPECT_FALSE(w->value());

    auto cb = RelatePredicate::coveredBy();
    cb->init(Dimension::L, Dimension::A);
    AsTopologyPredicate(*cb).init(small, big);
    cb->updateDimension(Location::INTERIOR, Location::BOUNDARY, Dimension::L);
    EXPECT_FALSE(cb->isKnown());
    cb->finish();
    EXPECT_TRUE(cb->value());
  }

  // Interior-interior intersection yields true on finish; no intersection
  // yields false on finish.
  {
    auto c = RelatePredicate::contains();
    c->init(Dimension::A, Dimension::P);
    AsTopologyPredicate(*c).init(big, small);
    c->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    EXPECT_FALSE(c->isKnown());
    c->finish();
    EXPECT_TRUE(c->value());

    auto w = RelatePredicate::within();
    w->init(Dimension::P, Dimension::A);
    AsTopologyPredicate(*w).init(small, big);
    w->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    EXPECT_FALSE(w->isKnown());
    w->finish();
    EXPECT_TRUE(w->value());

    auto cov_no_int = RelatePredicate::covers();
    cov_no_int->init(Dimension::A, Dimension::P);
    AsTopologyPredicate(*cov_no_int).init(big, small);
    cov_no_int->updateDimension(Location::INTERIOR, Location::EXTERIOR,
                                Dimension::A);
    EXPECT_FALSE(cov_no_int->isKnown());
    cov_no_int->finish();
    EXPECT_FALSE(cov_no_int->value());

    auto cb_no_int = RelatePredicate::coveredBy();
    cb_no_int->init(Dimension::P, Dimension::A);
    AsTopologyPredicate(*cb_no_int).init(small, big);
    cb_no_int->updateDimension(Location::EXTERIOR, Location::INTERIOR,
                               Dimension::A);
    EXPECT_FALSE(cb_no_int->isKnown());
    cb_no_int->finish();
    EXPECT_FALSE(cb_no_int->value());
  }
}

TEST_F(RelatePredicateTest, CrossesEqualsOverlapsTouchesStateMachine) {
  // Crosses: P/P and A/A short-circuit false.
  {
    auto p = RelatePredicate::crosses();
    p->init(Dimension::P, Dimension::P);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::crosses();
    p->init(Dimension::A, Dimension::A);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  // Crosses L/L: 1D I/I short-circuits false; 0D I/I waits for finish -> true;
  // disjoint lines wait for finish -> false.
  {
    auto p = RelatePredicate::crosses();
    p->init(Dimension::L, Dimension::L);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::L);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::crosses();
    p->init(Dimension::L, Dimension::L);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_TRUE(p->value());
  }
  {
    auto p = RelatePredicate::crosses();
    p->init(Dimension::L, Dimension::L);
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::L);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_FALSE(p->value());
  }
  // Crosses dimA < dimB and dimA > dimB: both short-circuit true when I/I and
  // interior-vs-exterior both exist, and stay unknown then false otherwise.
  {
    auto p = RelatePredicate::crosses();
    p->init(Dimension::L, Dimension::A);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::L);
    EXPECT_FALSE(p->isKnown());
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::L);
    EXPECT_TRUE(p->isKnown());
    EXPECT_TRUE(p->value());
  }
  {
    auto p = RelatePredicate::crosses();
    p->init(Dimension::P, Dimension::A);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::crosses();
    p->init(Dimension::A, Dimension::L);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::L);
    EXPECT_FALSE(p->isKnown());
    p->updateDimension(Location::EXTERIOR, Location::INTERIOR, Dimension::L);
    EXPECT_TRUE(p->isKnown());
    EXPECT_TRUE(p->value());
  }
  {
    auto p = RelatePredicate::crosses();
    p->init(Dimension::A, Dimension::P);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_FALSE(p->value());
  }

  // EqualsTopo: null envelopes, mixed null/non-null, unequal envelopes,
  // exterior short-circuits, full equality, and dim mismatch on finish.
  {
    auto p = RelatePredicate::equalsTopo();
    p->init(Dimension::P, Dimension::A);
    AsTopologyPredicate(*p).init(Envelope(), Envelope());
    EXPECT_TRUE(p->isKnown());
    EXPECT_TRUE(p->value());
  }
  {
    auto p = RelatePredicate::equalsTopo();
    p->init(Dimension::P, Dimension::A);
    AsTopologyPredicate(*p).init(Envelope(), Envelope(0.0, 1.0, 0.0, 1.0));
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::equalsTopo();
    p->init(Dimension::A, Dimension::A);
    AsTopologyPredicate(*p).init(Envelope(0.0, 1.0, 0.0, 1.0),
                                 Envelope(0.0, 2.0, 0.0, 1.0));
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  for (const auto& loc_pair : std::vector<std::pair<Location, Location>>{
           {Location::INTERIOR, Location::EXTERIOR},
           {Location::BOUNDARY, Location::EXTERIOR},
           {Location::EXTERIOR, Location::INTERIOR},
           {Location::EXTERIOR, Location::BOUNDARY},
       }) {
    auto p = RelatePredicate::equalsTopo();
    const Envelope env(0.0, 5.0, 0.0, 5.0);
    p->init(Dimension::A, Dimension::A);
    AsTopologyPredicate(*p).init(env, env);
    EXPECT_FALSE(p->isKnown());
    p->updateDimension(loc_pair.first, loc_pair.second, Dimension::P);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::equalsTopo();
    const Envelope env(0.0, 5.0, 0.0, 5.0);
    p->init(Dimension::A, Dimension::A);
    AsTopologyPredicate(*p).init(env, env);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::A);
    p->updateDimension(Location::BOUNDARY, Location::BOUNDARY, Dimension::L);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_TRUE(p->value());
  }
  {
    auto p = RelatePredicate::equalsTopo();
    const Envelope pt_env(1.0, 1.0, 1.0, 1.0);
    p->init(Dimension::P, Dimension::L);
    AsTopologyPredicate(*p).init(pt_env, pt_env);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_FALSE(p->value());
  }

  // Overlaps: unequal dims, P/P and A/A short-circuit true, incomplete P/P,
  // L/L short-circuit true vs 0D L/L finish false, and False/False finish.
  {
    auto p = RelatePredicate::overlaps();
    p->init(Dimension::L, Dimension::A);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::overlaps();
    p->init(Dimension::P, Dimension::P);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::P);
    EXPECT_FALSE(p->isKnown());
    p->updateDimension(Location::EXTERIOR, Location::INTERIOR, Dimension::P);
    EXPECT_TRUE(p->isKnown());
    EXPECT_TRUE(p->value());
  }
  {
    auto p = RelatePredicate::overlaps();
    p->init(Dimension::P, Dimension::P);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::P);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::overlaps();
    p->init(Dimension::A, Dimension::A);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::A);
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::A);
    EXPECT_FALSE(p->isKnown());
    p->updateDimension(Location::EXTERIOR, Location::INTERIOR, Dimension::A);
    EXPECT_TRUE(p->isKnown());
    EXPECT_TRUE(p->value());
  }
  {
    auto p = RelatePredicate::overlaps();
    p->init(Dimension::L, Dimension::L);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::L);
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::L);
    EXPECT_FALSE(p->isKnown());
    p->updateDimension(Location::EXTERIOR, Location::INTERIOR, Dimension::L);
    EXPECT_TRUE(p->isKnown());
    EXPECT_TRUE(p->value());
  }
  {
    auto p = RelatePredicate::overlaps();
    p->init(Dimension::L, Dimension::L);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::L);
    p->updateDimension(Location::EXTERIOR, Location::INTERIOR, Dimension::L);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::overlaps();
    p->init(Dimension::L, Dimension::L);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::L);
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::L);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::overlaps();
    p->init(Dimension::False, Dimension::False);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_FALSE(p->value());
  }

  // Touches: P/P short-circuit, I/I short-circuit false, B/B finish true,
  // exterior-only finish false.
  {
    auto p = RelatePredicate::touches();
    p->init(Dimension::P, Dimension::P);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::touches();
    p->init(Dimension::A, Dimension::A);
    p->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    EXPECT_TRUE(p->isKnown());
    EXPECT_FALSE(p->value());
  }
  {
    auto p = RelatePredicate::touches();
    p->init(Dimension::A, Dimension::A);
    p->updateDimension(Location::BOUNDARY, Location::BOUNDARY, Dimension::L);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_TRUE(p->value());
  }
  {
    auto p = RelatePredicate::touches();
    p->init(Dimension::P, Dimension::A);
    p->updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::P);
    EXPECT_FALSE(p->isKnown());
    p->finish();
    EXPECT_FALSE(p->value());
  }
}

TEST_F(RelatePredicateTest, EndToEndRelateNGPredicates) {
  auto box = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto box_dup = ReadWkt("POLYGON ((10 0, 10 10, 0 10, 0 0, 10 0))");
  auto inner_box = ReadWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
  auto shifted_box = ReadWkt("POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))");
  auto adj_box = ReadWkt("POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))");
  auto far_box = ReadWkt("POLYGON ((30 30, 40 30, 40 40, 30 40, 30 30))");
  auto bdy_line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto adj_line = ReadWkt("LINESTRING (0 0, 0 10)");
  auto seg_a = ReadWkt("LINESTRING (0 5, 10 5)");
  auto seg_b = ReadWkt("LINESTRING (5 5, 15 5)");
  auto cross_line = ReadWkt("LINESTRING (-5 5, 15 5)");
  auto diag_line = ReadWkt("LINESTRING (5 -5, 5 15)");
  auto zero_line = ReadWkt("LINESTRING (5 5, 5 5)");
  auto pt_inside = ReadWkt("POINT (5 5)");
  auto pt_bdy = ReadWkt("POINT (0 5)");
  auto mp_cross = ReadWkt("MULTIPOINT ((5 5), (20 20))");
  auto mp_overlap = ReadWkt("MULTIPOINT ((5 5), (30 30))");
  auto empty_pt = ReadWkt("POINT EMPTY");
  auto empty_poly = ReadWkt("POLYGON EMPTY");

  EXPECT_TRUE(RelateNG::relate(box.get(), shifted_box.get(),
                               *RelatePredicate::intersects()));
  EXPECT_FALSE(RelateNG::relate(box.get(), far_box.get(),
                                *RelatePredicate::intersects()));

  EXPECT_TRUE(
      RelateNG::relate(box.get(), far_box.get(), *RelatePredicate::disjoint()));
  EXPECT_FALSE(RelateNG::relate(box.get(), shifted_box.get(),
                                *RelatePredicate::disjoint()));

  EXPECT_TRUE(RelateNG::relate(box.get(), inner_box.get(),
                               *RelatePredicate::contains()));
  EXPECT_FALSE(RelateNG::relate(box.get(), bdy_line.get(),
                                *RelatePredicate::contains()));
  EXPECT_FALSE(
      RelateNG::relate(box.get(), pt_bdy.get(), *RelatePredicate::contains()));
  EXPECT_TRUE(RelateNG::relate(zero_line.get(), pt_inside.get(),
                               *RelatePredicate::contains()));

  EXPECT_TRUE(
      RelateNG::relate(inner_box.get(), box.get(), *RelatePredicate::within()));
  EXPECT_FALSE(
      RelateNG::relate(bdy_line.get(), box.get(), *RelatePredicate::within()));

  EXPECT_TRUE(
      RelateNG::relate(box.get(), bdy_line.get(), *RelatePredicate::covers()));
  EXPECT_TRUE(
      RelateNG::relate(box.get(), pt_bdy.get(), *RelatePredicate::covers()));
  EXPECT_FALSE(RelateNG::relate(box.get(), shifted_box.get(),
                                *RelatePredicate::covers()));

  EXPECT_TRUE(RelateNG::relate(bdy_line.get(), box.get(),
                               *RelatePredicate::coveredBy()));
  EXPECT_TRUE(
      RelateNG::relate(pt_bdy.get(), box.get(), *RelatePredicate::coveredBy()));
  EXPECT_FALSE(RelateNG::relate(shifted_box.get(), box.get(),
                                *RelatePredicate::coveredBy()));

  EXPECT_TRUE(RelateNG::relate(cross_line.get(), diag_line.get(),
                               *RelatePredicate::crosses()));
  EXPECT_TRUE(RelateNG::relate(cross_line.get(), box.get(),
                               *RelatePredicate::crosses()));
  EXPECT_TRUE(
      RelateNG::relate(mp_cross.get(), box.get(), *RelatePredicate::crosses()));
  EXPECT_TRUE(
      RelateNG::relate(box.get(), mp_cross.get(), *RelatePredicate::crosses()));
  EXPECT_FALSE(RelateNG::relate(box.get(), shifted_box.get(),
                                *RelatePredicate::crosses()));

  EXPECT_TRUE(RelateNG::relate(box.get(), box_dup.get(),
                               *RelatePredicate::equalsTopo()));
  EXPECT_TRUE(RelateNG::relate(empty_pt.get(), empty_poly.get(),
                               *RelatePredicate::equalsTopo()));
  EXPECT_FALSE(RelateNG::relate(box.get(), inner_box.get(),
                                *RelatePredicate::equalsTopo()));

  EXPECT_TRUE(RelateNG::relate(box.get(), shifted_box.get(),
                               *RelatePredicate::overlaps()));
  EXPECT_TRUE(
      RelateNG::relate(seg_a.get(), seg_b.get(), *RelatePredicate::overlaps()));
  EXPECT_TRUE(RelateNG::relate(mp_cross.get(), mp_overlap.get(),
                               *RelatePredicate::overlaps()));
  EXPECT_FALSE(RelateNG::relate(box.get(), inner_box.get(),
                                *RelatePredicate::overlaps()));

  EXPECT_TRUE(
      RelateNG::relate(box.get(), adj_box.get(), *RelatePredicate::touches()));
  EXPECT_TRUE(RelateNG::relate(bdy_line.get(), adj_line.get(),
                               *RelatePredicate::touches()));
  EXPECT_TRUE(
      RelateNG::relate(pt_bdy.get(), box.get(), *RelatePredicate::touches()));
  EXPECT_FALSE(RelateNG::relate(pt_inside.get(), pt_inside.get(),
                                *RelatePredicate::touches()));
  EXPECT_FALSE(RelateNG::relate(box.get(), shifted_box.get(),
                                *RelatePredicate::touches()));

  EXPECT_TRUE(RelateNG::relate(box.get(), adj_box.get(),
                               *RelatePredicate::matches("FF2F11212")));
  EXPECT_FALSE(RelateNG::relate(box.get(), adj_box.get(),
                                *RelatePredicate::matches("T********")));
  EXPECT_THROW(RelatePredicate::matches("SHORT"),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(RelateNG::relate(box.get(), adj_box.get(),
                                *RelatePredicate::matches("TTTT")),
               geos::util::IllegalArgumentException);

  // Verify RelatePredicate factories work with prepared RelateNG evaluation.
  auto prepared_box = RelateNG::prepare(box.get());
  EXPECT_TRUE(
      prepared_box->evaluate(inner_box.get(), *RelatePredicate::contains()));
  EXPECT_TRUE(
      prepared_box->evaluate(adj_box.get(), *RelatePredicate::touches()));
  EXPECT_TRUE(
      prepared_box->evaluate(far_box.get(), *RelatePredicate::disjoint()));
  EXPECT_TRUE(
      prepared_box->evaluate(bdy_line.get(), *RelatePredicate::covers()));
  EXPECT_TRUE(
      prepared_box->evaluate(shifted_box.get(), *RelatePredicate::overlaps()));
  EXPECT_TRUE(
      prepared_box->evaluate(box_dup.get(), *RelatePredicate::equalsTopo()));
  EXPECT_TRUE(prepared_box->evaluate(adj_box.get(),
                                     *RelatePredicate::matches("FF2F11212")));
}

}  // namespace
}  // namespace geos_google
