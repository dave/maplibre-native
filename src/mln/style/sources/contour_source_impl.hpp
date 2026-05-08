#pragma once

#include <mln/style/source_impl.hpp>
#include <mln/style/sources/contour_source.hpp>

namespace mln {
namespace style {

class ContourSource::Impl final : public Source::Impl {
public:
    Impl(std::string id, ContourSourceOptions options);
    ~Impl() final;

    const ContourSourceOptions& getOptions() const { return options; }

    std::optional<std::string> getAttribution() const final;

private:
    ContourSourceOptions options;
};

} // namespace style
} // namespace mln
