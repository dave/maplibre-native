#pragma once

#include <mln/algorithm/contour/isolines.hpp>

namespace mln {
namespace algorithm {
namespace contour {

// The marching squares before the faster one, for the tests only.
std::vector<ContourLineString> generateContoursReference(std::span<const std::int16_t> heights,
                                                         int width,
                                                         int height,
                                                         const ContourThresholds& thresholds);

} // namespace contour
} // namespace algorithm
} // namespace mln
