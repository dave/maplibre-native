#include <mln/test/util.hpp>
#include <mln/text/placement.hpp>

using namespace mln;

// The short placement period for new symbol buckets lasts until a placement
// runs, not only for the frame that added them: a frame inside the last
// placement's period skips the placement, and the frames after it add no
// buckets.
TEST(PlacementController, NewBucketsKeepTheShortPeriodUntilAPlacementRuns) {
    PlacementController controller;
    EXPECT_FALSE(controller.updatePeriodOverride(false, false));

    auto added = controller.updatePeriodOverride(true, false);
    ASSERT_TRUE(added);
    EXPECT_EQ(Milliseconds(30), *added);

    // The placement was skipped in that frame; the next frame adds nothing.
    auto next = controller.updatePeriodOverride(false, false);
    ASSERT_TRUE(next);
    EXPECT_EQ(Milliseconds(30), *next);

    controller.setPlacement(makeMutable<Placement>());
    EXPECT_FALSE(controller.updatePeriodOverride(false, false));
}

TEST(PlacementController, ATiltedViewKeepsTheFullPeriod) {
    PlacementController controller;
    EXPECT_FALSE(controller.updatePeriodOverride(true, true));
    // The buckets are still pending: when the view is no longer tilted, the
    // short period applies until a placement runs.
    EXPECT_TRUE(controller.updatePeriodOverride(false, false));
}
