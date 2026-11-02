package org.maplibre.android.style.sources

/**
 * The tiles a tile source's `reloadTiles` asks again for.
 * Core: `mln::style::ReloadFilter`.
 */
enum class ReloadFilter(internal val nativeValue: Int) {
    /** The last answer was "no content". */
    EMPTY(0b01),

    /** The last answer was an error. */
    FAILED(0b10),

    /** Either. */
    EMPTY_AND_FAILED(0b11)
}
