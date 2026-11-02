# Contour source

A `contour` source draws elevation contour lines from the tiles of a `raster-dem`
source. It fetches nothing of its own: it reads the DEM tiles that the map has already
loaded, so the DEM data is fetched and cached once.

The source is style JSON only. There is no `MLNContourSource` or Kotlin
`ContourSource`, and `style::ContourSource` is the only API for it in C++. The type
follows the proposal in
[maplibre-style-spec#583](https://github.com/maplibre/maplibre-style-spec/issues/583),
which is not yet in the style spec, so the spec validator does not accept it.

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
      "intervals": [200, 10, 100, 11, 20, 12, 10],
      "unit": "meters",
      "majorMultiplier": [1, 11, 5, 12, 10]
    }
  },
  "layers": [
    {
      "id": "contour-lines",
      "type": "line",
      "source": "contours",
      "paint": {"line-width": ["case", ["get", "major"], 1.2, 0.6]}
    }
  ]
}
```

## Properties

| Property | Type | Required | Meaning |
|---|---|---|---|
| `source` | string | yes | The ID of a `raster-dem` source in the same style. |
| `intervals` | array of numbers | yes | The distance between lines, in `unit`, by zoom. |
| `unit` | string or number | no, default `"meters"` | `"meters"`, `"feet"`, or a positive number that metres are multiplied by to give the unit. |
| `majorMultiplier` | array of numbers | no, default `[5]` | Every Nth line is a major line, by zoom. The outputs are whole numbers. |

`intervals` and `majorMultiplier` are step schedules: `[output0, stop1, output1, ...,
stopN, outputN]`, with an odd number of entries, as in a `step` expression. Zoom below
`stop1` uses `output0`. A zoom at or above `stopK` uses `outputK`. Stops rise strictly,
and outputs are above 0.

A schedule is read at the map zoom a tile is drawn at. That zoom is whole: the map
zoom rounded, because a contour source covers the zoom its `raster-dem` source covers.
So a band starts half a zoom before its stop. It goes on following the map zoom above
the DEM's `maxzoom`.

An invalid property makes the style parser reject the source. In C++,
`style::ContourSource` throws `std::invalid_argument` for an invalid schedule. A `source`
that is not in the style, or is not a `raster-dem` source, logs one error, and the
contour source draws nothing until the source is there.

## Features

Each tile holds one line feature per contour line in the tile, in order of elevation.
A contour tile has one layer, so a style layer needs no `source-layer`.

| Property | Type | Meaning |
|---|---|---|
| `ele` | number | The elevation of the line, in `unit`. A whole number if the interval is. |
| `interval` | number | The value of `intervals` for this tile. |
| `major` | boolean | The line's elevation is a multiple of `interval × majorMultiplier`. |

## Missing data

A contour tile completes with no lines if its DEM tile has no content or fails to
load. The contour source makes the tiles its DEM source makes, so a view outside the
DEM's `bounds` has none. A still render or a snapshot of such a view finishes.
