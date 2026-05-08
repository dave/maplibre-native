# PMTiles

Working with PMTiles

Starting MapLibre iOS 6.10.0, [PMTiles](https://docs.protomaps.com/pmtiles/) archives are supported as tile sources. Prefix any tile source URL with `pmtiles://` to read from a PMTiles archive:

- `pmtiles://https://` — stream tiles from a remote file
- `pmtiles://file://` — read a file from the device filesystem, including the app bundle

The `pmtiles://` prefix works with any [tile source](https://maplibre.org/maplibre-style-spec/sources/) (`vector`, `raster`, `raster-dem`).

> Note: PMTiles sources do not support offline pack downloads or caching.

## Loading a style that uses PMTiles sources

Pass a style JSON URL that already defines `pmtiles://` sources to `MLNMapView`:

<!-- include-example(PMTilesStyleURL) -->

## Adding a PMTiles source

Add a source and layer after the style loads using `MLNMapViewDelegate`:

<!-- include-example(PMTilesAddSource) -->

## Loading a local PMTiles file

Bundle a `.pmtiles` file in your app target and load it using its bundle URL:

<!-- include-example(PMTilesLocalFile) -->
