#include <mln/storage/file_source.hpp>
#include <mln/style/conversion/geojson.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/source_observer.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/sources/geojson_source_impl.hpp>
#include <mln/tile/tile.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/label_steps.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/monotonic_timer.hpp>
#include <mln/util/thread_pool.hpp>
#include <mln/util/identity.hpp>

#include <stdexcept>

namespace mln {
namespace style {

namespace {

const Immutable<GeoJSONOptions>& checkedOptions(const Immutable<GeoJSONOptions>& options) {
    if (options->labelLane && options->synchronousUpdate) {
        throw std::invalid_argument("GeoJSON source labelLane cannot be set with synchronousUpdate");
    }
    // Supercluster cuts its tiles on the render thread: the lane cannot cover it.
    if (options->labelLane && options->cluster) {
        throw std::invalid_argument("GeoJSON source labelLane cannot be set with cluster");
    }
    return options;
}

std::shared_ptr<Scheduler> labelLane() {
    auto lane = Scheduler::GetLabelLane();
    if (!lane) {
        // GetLabelLane logged the cause and asserted.
        throw std::runtime_error("GeoJSON source labelLane: the label lane cannot be made");
    }
    return lane;
}

} // namespace

// static
Immutable<GeoJSONOptions> GeoJSONOptions::defaultOptions() {
    static Immutable<GeoJSONOptions> options = makeMutable<GeoJSONOptions>();
    return options;
}

GeoJSONSource::GeoJSONSource(std::string id, Immutable<GeoJSONOptions> options)
    : Source(makeMutable<Impl>(std::move(id), checkedOptions(options))),
      parseScheduler(options->labelLane ? labelLane() : Scheduler::GetBackground()),
      // A lane source cuts on a thread of its own: geojson-vt is not
      // thread-safe, and the shared sequenced ring also serves other sources
      // and logging.
      sequencedScheduler(options->labelLane ? std::make_shared<SequencedScheduler>("Label lane s")
                                            : Scheduler::GetSequenced()) {}

GeoJSONSource::~GeoJSONSource() = default;

const GeoJSONSource::Impl& GeoJSONSource::impl() const {
    return static_cast<const Impl&>(*baseImpl);
}

/*!
 * \brief GeoJSONSource::setURL
 * Set the url to a geojson file which can be fetched
 * \param url_
 */
uint64_t GeoJSONSource::setURL(const std::string& url_) {
    url = url_;
    urlGeneration = nextDataGeneration();
    urlBody.reset();

    // Signal that the source description needs a reload
    if (loaded || req) {
        loaded = false;
        req.reset();
        observer->onSourceDescriptionChanged(*this);
    }
    return urlGeneration;
}

uint64_t GeoJSONSource::setGeoJSON(const mapbox::geojson::geojson& geoJSON) {
    return setGeoJSONData(GeoJSONData::create(geoJSON, sequencedScheduler, impl().getOptions(), getID()));
}

uint64_t GeoJSONSource::setGeoJSONData(std::shared_ptr<GeoJSONData> geoJSONData) {
    const uint64_t generation = nextDataGeneration();
    setGeoJSONData(std::move(geoJSONData), generation);
    return generation;
}

void GeoJSONSource::setGeoJSONData(std::shared_ptr<GeoJSONData> geoJSONData, uint64_t generation) {
    req.reset();
    // A URL body still being parsed is older: it must not replace this one.
    ++requestGeneration;
    baseImpl = makeMutable<Impl>(impl(), std::move(geoJSONData), landingGeneration(generation));
    observer->onSourceChanged(*this);
}

uint64_t GeoJSONSource::landingGeneration(uint64_t generation) {
    // A body made off the calling thread lands after a later generation was
    // given out (a layer change on the source, `markLayersChanged`): it takes
    // a new one, so the source's generation never goes back and the newest
    // one covers both the body and the layers.
    return generation > impl().getDataGeneration() ? generation : nextDataGeneration();
}

uint64_t GeoJSONSource::markLayersChanged() {
    baseImpl = makeMutable<Impl>(impl(), impl().getData().lock(), nextDataGeneration());
    return impl().getDataGeneration();
}

uint64_t GeoJSONSource::nextDataGeneration() {
    return ++dataGeneration;
}

uint64_t GeoJSONSource::getDataGeneration() const {
    return impl().getDataGeneration();
}

void GeoJSONSource::setGeoJSONError(uint64_t generation, const std::string& message) {
    observer->onSourceError(
        *this,
        std::make_exception_ptr(std::runtime_error("GeoJSON body " + std::to_string(generation) + ": " + message)));
}

std::shared_ptr<Scheduler> GeoJSONSource::getParseScheduler() const {
    return parseScheduler;
}

std::shared_ptr<Scheduler> GeoJSONSource::getSliceScheduler() const {
    return sequencedScheduler;
}

std::optional<std::string> GeoJSONSource::getURL() const {
    return url;
}

const GeoJSONOptions& GeoJSONSource::getOptions() const {
    return *impl().getOptions();
}

void GeoJSONSource::loadDescription(FileSource& fileSource) {
    if (!url) {
        loaded = true;
        return;
    }

    if (req) {
        return;
    }

    req = fileSource.request(Resource::source(*url), [this](const Response& res) {
        if (res.error) {
            observer->onSourceError(*this, std::make_exception_ptr(std::runtime_error(res.error->message)));
        } else if (res.notModified) {
            return;
        } else if (res.noContent) {
            observer->onSourceError(*this, std::make_exception_ptr(std::runtime_error("unexpectedly empty GeoJSON")));
        } else {
            assert(res.data);
            // The same bytes again (a revalidation, a second answer): nothing new to show.
            if (urlBody && *urlBody == *res.data) {
                return;
            }
            // A second, different body for the URL is a new body: a new
            // generation, so a loaded check never waits on a body it can no
            // longer see.
            const uint64_t generation = urlBody ? nextDataGeneration() : urlGeneration;
            urlBody = res.data;
            // Note: This task appears to be safe enough to schedule on the generic background queue.
            // This task does not reference other objects who's lifetimes are coupled with a map.
            parseScheduler->scheduleAndReplyValue(
                util::SimpleIdentity::Empty,
                /* parseInBackground */
                [data = res.data,
                 seqScheduler{sequencedScheduler},
                 options = impl().getOptions(),
                 id = getID(),
                 queued = util::LabelSteps::recording() ? util::MonotonicTimer::now()
                                                        : std::chrono::duration<double>{0}]() {
                    const util::LabelSteps::Scope step(id, "parse-url", false, queued);
                    return GeoJSONData::parse(*data, std::move(seqScheduler), options, id);
                },
                /* onParsed */
                [this, self = makeWeakPtr(), capturedReqGeneration = ++requestGeneration, generation](
                    GeoJSONData::ParseResult parsed) {
                    assert(capturedReqGeneration);
                    if (auto guard = self.lock(); self) {
                        if (capturedReqGeneration ==
                            requestGeneration) { // If a new request is being processed, ignore this impl.
                            auto* error = std::get_if<std::string>(&parsed);
                            // A body that does not parse makes the source empty,
                            // so that no tile waits for ever, and is reported.
                            auto data = error ? nullptr : std::get<std::shared_ptr<GeoJSONData>>(std::move(parsed));
                            baseImpl = makeMutable<Impl>(impl(), std::move(data), landingGeneration(generation));
                            loaded = true;
                            if (error) {
                                observer->onSourceError(*this, std::make_exception_ptr(std::runtime_error(*error)));
                            }
                            observer->onSourceLoaded(*this);
                        }
                    }
                });
        }
    });
}

bool GeoJSONSource::supportsLayerType(const mln::style::LayerTypeInfo* info) const {
    return mln::underlying_type(Tile::Kind::Geometry) == mln::underlying_type(info->tileKind);
}

Mutable<Source::Impl> GeoJSONSource::createMutable() const noexcept {
    return staticMutableCast<Source::Impl>(makeMutable<Impl>(impl()));
}

bool GeoJSONSource::isUpdateSynchronous() const noexcept {
    return baseImpl->isUpdateSynchronous();
}

void GeoJSONSource::setOverrideSynchronousUpdate(bool value) {
    if (value && impl().getOptions()->labelLane) {
        throw std::logic_error("GeoJSON source " + getID() + ": labelLane refuses a synchronous update override");
    }
    impl().setOverrideSynchronousUpdate(value);
}

} // namespace style
} // namespace mln
