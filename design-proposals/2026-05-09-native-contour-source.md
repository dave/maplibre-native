# Native `contour` source for maplibre-native

**Status:** Implemented in this fork. The style-spec side is not settled ([maplibre-style-spec#623](https://github.com/maplibre/maplibre-style-spec/pull/623) is a draft), and this implementation differs from that draft: see "Style spec".
**Author:** Dave Brophy ([@dave](https://github.com/dave))
**Tracks:** [maplibre-style-spec#583](https://github.com/maplibre/maplibre-style-spec/issues/583)

## Motivation

MapLibre style authors who want elevation contour lines on a hillshade
basemap currently have to either:

1. Bake contours server-side into vector tiles and serve them as a vector
   source (extra storage + traffic; the DEM is already loaded for hillshade),
   or
2. Use [`maplibre-contour`](https://github.com/onthegomap/maplibre-contour),
   the existing TS prototype, which runs in `maplibre-gl-js` as a custom
   `addProtocol` callback. That works around the fact that gl-js's
   `source_cache.ts` doesn't support multiple consumers reading the same
   DEM tiles, by maintaining its own parallel DEM fetch path.

Neither option lands well in maplibre-native:

- Server-side baking duplicates the DEM data on disk and on the wire.
- The plugin pattern doesn't translate to the C++ renderer; there's no
  `addProtocol` equivalent for in-process tile generation.

In maplibre-native we control the tile pyramid directly, so a true
source-on-source contour generator is achievable: the DEM tiles loaded
for an existing `raster-dem` source can drive contour-line generation
in-process, with no extra fetches, by registering a tile-load listener.

This proposal implements the `contour` source type from
[maplibre-style-spec#583](https://github.com/maplibre/maplibre-style-spec/issues/583)
natively in maplibre-native's renderer, with a shared-pyramid
architecture matching the issue's stated rationale:

> One of the reasons for moving out of the plugin into maplibre would be to make use of the shared DEM tile cache between sources… terrain/hillshading/contours reading from the same tiles should be able share a source definition.
>
> — msbarry, in [#583](https://github.com/maplibre/maplibre-style-spec/issues/583)

## Proposed Change

### Style spec

A new source type `contour` with this shape:

```json
{
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["https://example.com/dem/{z}/{x}/{y}.png"],
      "encoding": "terrarium",
      "tileSize": 512,
      "maxzoom": 12
    },
    "contours": {
      "type": "contour",
      "source": "dem",
      "intervals": [200, 9, 100, 10, 50, 11, 20, 12, 10],
      "unit": "meters",
      "majorMultiplier": [5, 14, 4, 15, 5]
    }
  }
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `type` | string | yes | Always `"contour"`. |
| `source` | string | yes | ID of the upstream `raster-dem` source whose tile pyramid drives generation. |
| `intervals` | step-by-zoom number array | yes | Per-zoom contour interval in display units. Odd-length: `[output0, stop1, output1, stop2, …, outputN]`. |
| `unit` | string \| number | no, default `"meters"` | `"meters"`, `"feet"`, or a positive number that metres are multiplied by. |
| `majorMultiplier` | step-by-zoom positive-integer array | no, default `[5]` | Same odd-length step-by-zoom shape as `intervals`. Tags every Nth contour level as `major: true` at each zoom band. |

This differs from the style-spec draft #623. There, `intervals` and
`majorMultiplier` are a constant or a zoom expression, and `intervals` is
optional. A numeric `unit` is the length of a custom unit, which metres are
divided by. There is also an `overzoom` property, with a bilinear upsample of the
DEM. We keep this shape until the spec settles; this source has no `overzoom`.

Schedules are read at the map zoom a tile is drawn at: the whole zoom the tile
pyramid covers, which is the map zoom rounded.

The source emits standard vector-tile features that the existing
`LineLayer` and `SymbolLayer` consume:

| Property | Type | Description |
|---|---|---|
| `ele` | number | Elevation of the contour line in display units. A whole number if the interval is whole. |
| `interval` | number | The contour interval used at this tile's zoom (display units). |
| `major` | bool | True if this line is a multiple of `majorMultiplier × interval`. |

[`maplibre-contour`](https://github.com/onthegomap/maplibre-contour) emits other
properties (`ele` and `level`), so a style written for one needs a change for the
other.

### Architecture

The render-side contour source borrows the upstream raster-dem source's
tile pyramid via a tile-load listener API. Concretely:

```
RenderRasterDEMSource (existing)
   ├── TilePyramid<RasterDEMTile>   ← single fetch path, single cache
   ├── consumer: HillshadeLayer (existing)
   └── consumer: RenderContourSource (new) — subscribes via addTileLoadListener

RenderContourSource (new, non-fetching)
   ├── TilePyramid<ContourTile>    ← derived geometry tiles, no network I/O
   ├── on DEM-tile-loaded (z,x,y) → run marching-squares → vector features
   └── exposed to LineLayer / SymbolLayer via the standard render-tile interface
```

Two new bits of plumbing make this work:

1. **`RenderRasterDEMSource::addTileLoadListener`** publishes per-tile-load
   events to cross-source consumers. Implemented as a `ListenerSet<>`
   (handle-based fan-out container — separate utility, also useful for
   future inter-source dependencies). The listener fires after the
   existing border-stitching logic when a tile settles (it has data, it has no
   content, or it failed) and synchronously replays settled tiles when a new
   listener registers, to close the race between consumer-pyramid update and DEM
   tile arrivals. The contour source makes the tiles the DEM source makes, so each
   contour tile hears from its DEM tile, and one with no data completes empty.
2. **`TileParameters::getRenderSource`** is a `std::function<RenderSource*(const std::string&)>`
   populated by `RenderOrchestrator` and threaded into the per-frame
   `update()` calls. This is the first inter-source lookup mechanism in
   the renderer; it's deliberately minimal (just a pointer-by-id callback)
   and reusable for future source-on-source patterns.

`RenderContourSource::update()` resolves its upstream raster-dem by ID on every
call and holds it by weak pointer, because sibling sources are destroyed in no set
order. When the upstream changes it moves its listener and drops its tiles. While
there is no upstream, or its tileset has not loaded, it has no tiles. A `source` that
does not resolve is one logged error.

### Algorithm

The marching-squares core lives at `src/mln/algorithm/contour/` as a
self-contained module with no MapLibre dependencies. Highlights:

- **Single-pass cell traversal** (from `maplibre-contour`'s `isolines.ts`):
  one sweep over the height grid emits all threshold levels for each
  cell, with incremental polyline stitching during traversal. ~5× fewer
  cell touches vs the per-level pass our prior server-side implementation
  used.
- **Saddle handling (cases 5 and 10)**: emit both segments of one fixed
  pairing rather than disambiguating per-cell via the corner mean. The
  fixed-pairing output is sufficient for typical DEM data where saddle
  points are sparse and visually subtle. Center-average disambiguation
  is a future improvement; the algorithm's interface accommodates it
  without breaking existing callers.
- **Float heights**: lines come from each height with its fraction, not from
  whole metres.
- **Tile-local int32 coordinates with an extent the caller sets**
  (`ContourThresholds::extent`). The contour tile uses `util::EXTENT`, 8192.
- **Output in order of elevation**, then in scan order within a level.
- **Smoothing and simplification are a separate stage** applied by the
  contour-tile builder: Douglas-Peucker then Chaikin (2 iterations, which keeps the
  ends of an open line and treats a closed line as a ring). Keeping these out of the algorithm core makes
  it easy to test against `maplibre-contour`'s raw output and to skip a
  stage entirely when a consumer wants un-smoothed output.

### Threading

Each contour tile snapshots its DEM image synchronously on the render
thread (avoids racing with DEMData::backfillBorder, which mutates the
shared image as neighbour tiles arrive), then dispatches the build
to the background scheduler. The reply lands back on the render thread and
calls `setData()` on the tile. A `WeakPtrFactory` guards against the contour tile
being destroyed (panned away) before the reply arrives.

A DEM arrival also notifies the up to eight neighbours whose borders it filled. The
source queues tile IDs, so a batch of arrivals takes one snapshot per tile. A tile runs
one build at a time, and calls made during a build cost one more build, of the newest
image. Only the newest result is applied.

## API Modifications

### Public

- New `style::ContourSource` class at `include/mln/style/sources/contour_source.hpp`,
  matching the existing `Source` subclass pattern.
- New `style::ContourSourceOptions` struct on the same header.
- Style-spec parser recognises `"type": "contour"` and dispatches to
  `convertContourSource` in `src/mln/style/conversion/source.cpp`.

### Internal

- New `SourceType::Contour` enum value in `include/mln/style/types.hpp`.
- New `RenderContourSource` at `src/mln/renderer/sources/`.
- New `ContourTile` at `src/mln/tile/`.
- `RenderRasterDEMSource` gains the listener API.
- `TileParameters` gains the `getRenderSource` callback.
- `ListenerSet<>` utility added at `src/mln/util/listener_set.hpp`.

No existing public API breaks. The `getRenderSource` callback in
`TileParameters` is internal (not part of any SDK public surface).

## Migration Plan and Compatibility

This is a strictly additive change at the style-spec level:

- Existing styles continue to work unchanged.
- A style author opting in to the new source type adds it as a sibling
  source pointing at any existing `raster-dem` source. The new source
  contributes vector features that style filters on `["get", "ele"]` /
  `["get", "major"]` consume.
- The TS prototype (`maplibre-contour`) remains the gl-js path until
  gl-js gains shared-pyramid support. The two emit different feature properties,
  so a style needs a change to move between them.

## Rejected Alternatives

### Loose-coupling via custom-protocol callback

Mirror the TS prototype: register a tile-URL pattern, fetch DEM tiles
ourselves under a separate cache, run the algorithm, return MVT bytes.

Rejected: defeats the main reason for going native. The whole point of
this work is to share the DEM tile pyramid that's already loaded for
hillshade. Loose coupling reproduces gl-js's known-broken-but-not-fixable
double-fetch architecture in a renderer where we can do better.

### Constructor-injection cross-source lookup

Pass `std::function<RenderSource*(const std::string&)>` to every
`RenderSource` subclass at construction time, stored as a member.

Rejected: changes 8+ existing constructor signatures with no clear
benefit over per-frame `TileParameters` injection. The lookup is only
needed during `update()` anyway, so threading it through that single
call site is the smaller blast radius.

### Multi-observer `TilePyramid`

Replace `TilePyramid::observer` with `std::vector<TileObserver*>` so
multiple consumers can subscribe directly to tile-load events at the
pyramid level.

Rejected: more invasive than needed. The `RenderRasterDEMSource` is
already the single observer of its pyramid (it forwards to its render
parent and does border-stitching); adding a fan-out side-channel on
the source rather than the pyramid keeps the change local to the one
type that has cross-source consumers, with no churn in `Tile`,
`TilePyramid`, or any other observer site.

## Testing

### Unit

- `algorithm/contour/isolines.test.cpp` — marching-squares cases, the saddle
  pairing, multi-threshold single-pass emission, output order.
- `algorithm/contour/intervals.test.cpp` — per-zoom interval lookup,
  step-expression boundary semantics.
- `algorithm/contour/units.test.cpp` — meters / feet / custom
  multiplier round-trips.
- `algorithm/contour/smoothing.test.cpp` — Douglas-Peucker invariants
  (endpoint preservation, collinear drop, threshold strictness),
  Chaikin endpoint pinning on an open line, a closed line as a ring.
- `style/sources/contour_source.test.cpp` — class construction +
  getters, JSON parser happy + rejection paths, idempotent round-trip
  (parse → reconstruct via getters → re-parse → equal).
- `util/listener_set.test.cpp` — handle-based fan-out container.
- `tile/contour_tile.test.cpp` — heights keep their fraction, one build at a
  time, fractional intervals.
- `tile/contour_fixtures.test.cpp` — real DEM tiles: lines meet across a tile
  boundary.
- `renderer/sources/render_contour_source.test.cpp` — a tile settles with data,
  no content, an error or out of bounds; the DEM source changes or goes; the
  interval at the map zoom; a bad `source`.

### Integration

- `api/contour_source.test.cpp` — a still render or a continuous map finishes
  loading when a DEM tile has no content, fails, or is out of bounds.
- Render tests in `metrics/integration/render-tests/contour-source/`, which
  run on Metal, WebGPU, Vulkan and OpenGL in CI.

## Disclosure

Significant portions of this proposal and the underlying implementation
were drafted with assistance from an AI coding agent (Anthropic's
Claude Opus 4.7) per the
[MapLibre AI Policy](https://github.com/maplibre/maplibre/blob/main/AI_POLICY.md).
All design decisions, code, and prose were reviewed and edited by the
human author; the algorithm's correctness was verified by the test
suite cited above plus on-device visual inspection.
