#include <mln/util/immutable.hpp>
#import "MLNFoundation.h"
#import "MLNShapeSource.h"

NS_ASSUME_NONNULL_BEGIN

namespace mln {
namespace style {
struct GeoJSONOptions;
}
}  // namespace mln

MLN_EXPORT
mln::Immutable<mln::style::GeoJSONOptions> MLNGeoJSONOptionsFromDictionary(
    NSDictionary<MLNShapeSourceOption, id> *options);

@interface MLNShapeSource (Private)

/**
 :nodoc:
 Debug log showing structure of an ``MLNFeature``. This method recurses in the case
 that the feature conforms to ``MLNCluster``. This method is used for testing and
 should be considered experimental, likely to be removed or changed in future
 releases.

 @param feature An object that conforms to the ``MLNFeature`` protocol.
 @param indent Used during recursion. Specify 0.
 */

- (void)debugRecursiveLogForFeature:(id<MLNFeature>)feature indent:(NSUInteger)indent;

/**
 :nodoc:
 The step recorder, for debug probes and tests. This is private API: it is
 not in the framework's headers, and it may change or go away. When on,
 every shape source's off-thread steps and tile hops record the thread they
 ran on and their times. Off by default. While it is off, a step costs one
 relaxed atomic load and allocates nothing. A step sent to its thread while
 the recorder is off is not recorded. Core: `mln::util::LabelSteps`.

 An app that calls it declares it in an Objective-C header of its own (a
 category on ``MLNShapeSource`` with these two methods).
 */
+ (void)setRecordsLabelSteps:(BOOL)recording;

/**
 :nodoc:
 The steps recorded since the last call, in the order they ended. Each is a
 dictionary with `sourceID` (NSString), `name` (NSString: "convert",
 "parse-url", "index", "slice", "tile-parse", "tile-layout", or a tile hop
 "hop-data", "hop-glyphs", "hop-images", "hop-layout", "hop-inline-layout"),
 `thread` (NSString), `hop` (NSNumber, Boolean), and `queued`, `start` and
 `end` (NSNumber, seconds on the steady clock; `queued` is 0 where the
 recorder cannot see it).
 */
+ (NSArray<NSDictionary<NSString *, id> *> *)takeLabelSteps;
@end

NS_ASSUME_NONNULL_END
