# PMTiles

{{ activity_source_note("PMTilesActivity.kt") }}

Starting MapLibre Android 11.7.0, [PMTiles](https://docs.protomaps.com/pmtiles/) archives are supported as tile sources. Prefix any tile source URL with `pmtiles://` to read from a PMTiles archive:

- `pmtiles://https://` — stream tiles from a remote file
- `pmtiles://asset://` — read a file bundled in the app's `assets/` folder
- `pmtiles://file://` — read a file from device storage

The `pmtiles://` prefix works with any [tile source](https://maplibre.org/maplibre-style-spec/sources/) (`vector`, `raster`, `raster-dem`).

> Note: PMTiles sources do not support offline pack downloads or caching.

## Loading a style that uses PMTiles sources

Load a style JSON that already defines `pmtiles://` sources:

```kotlin
--8<-- "MapLibreAndroidTestApp/src/main/java/org/maplibre/android/testapp/activity/sources/PMTilesActivity.kt:loadStyle"
```

## Adding a PMTiles source

Add a source and layer to an existing style:

```kotlin
--8<-- "MapLibreAndroidTestApp/src/main/java/org/maplibre/android/testapp/activity/sources/PMTilesActivity.kt:addSource"
```

## Loading a local PMTiles file

Bundle a `.pmtiles` file in `src/main/assets/` and reference it with `asset://`:

```kotlin
--8<-- "MapLibreAndroidTestApp/src/main/java/org/maplibre/android/testapp/activity/sources/PMTilesActivity.kt:loadFromAsset"
```

For files on device storage, use `file://` with a path from `getExternalFilesDir(null)` rather than a hardcoded path.
