#include <mln/style/sources/contour_source_impl.hpp>

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace mln {
namespace style {

namespace {

bool wholeOutputs(const algorithm::contour::IntervalSchedule& schedule) {
    for (std::size_t i = 0; i < schedule.stops.size(); i += 2) {
        if (schedule.stops[i] != std::floor(schedule.stops[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

ContourSource::Impl::Impl(std::string id_, ContourSourceOptions options_)
    : Source::Impl(SourceType::Contour, std::move(id_)),
      options(std::move(options_)) {
    if (!algorithm::contour::isValid(options.intervals)) {
        throw std::invalid_argument("contour source `" + id + "` has an invalid `intervals` schedule");
    }
    if (!algorithm::contour::isValid(options.majorMultiplier) || !wholeOutputs(options.majorMultiplier)) {
        throw std::invalid_argument("contour source `" + id + "` has an invalid `majorMultiplier` schedule");
    }
}

ContourSource::Impl::~Impl() = default;

std::optional<std::string> ContourSource::Impl::getAttribution() const {
    // Contour features are derived from an upstream `raster-dem` source;
    // attribution flows through the upstream source, not this one.
    return {};
}

} // namespace style
} // namespace mln
