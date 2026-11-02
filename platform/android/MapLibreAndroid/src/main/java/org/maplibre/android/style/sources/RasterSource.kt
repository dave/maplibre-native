package org.maplibre.android.style.sources

import androidx.annotation.Keep
import java.net.URI
import java.net.URL

/**
 * Raster source, allows using raster tiles as source.
 *
 * @see [The style specification](https://maplibre.org/maplibre-style-spec/.sources-raster)
 */
class RasterSource : Source {
    /**
     * Internal use
     *
     * @param nativePtr - pointer to native peer
     */
    @Keep
    internal constructor(nativePtr: Long) : super(nativePtr) {
    }

    /**
     * Create the raster source from an URL
     *
     * @param id  the source id
     * @param url the source url
     */
    @Deprecated("use {@link #RasterSource(String, URI)} instead")
    constructor(id: String?, url: URL) : this(id, url.toExternalForm()) {
    }

    /**
     * Create the raster source from an URI.
     *
     *
     * An URI is a combination of a protocol and a resource path.
     * The following URI protocol schemes are supported:
     *
     *
     *  * http://
     *
     *  * load resources using HyperText Transfer Protocol
     *
     *  * file://
     *
     *  * load resources from the Android file system
     *
     *  * asset://
     *
     *  * load resources from the binary packaged assets folder
     *
     *
     *
     * @param id  the source id
     * @param uri the source uri
     */
    constructor(id: String?, uri: URI) : this(id, uri.toString()) {}

    /**
     * Create the raster source from an URI.
     *
     *
     * An URI is a combination of a protocol and a resource path.
     * The following URI protocol schemes are supported:
     *
     *
     *  * http://
     *
     *  * load resources using HyperText Transfer Protocol
     *
     *  * file://
     *
     *  * load resources from the Android file system
     *
     *  * asset://
     *
     *  * load resources from the binary packaged assets folder
     *
     *
     *
     * @param id  the source id
     * @param uri the source uri
     */
    constructor(id: String?, uri: String?) : super() {
        initialize(id, uri, DEFAULT_TILE_SIZE)
    }

    /**
     * Create the raster source from an URI with a specific tile size.
     *
     *
     * An URI is a combination of a protocol and a resource path.
     * The following URI protocol schemes are supported:
     *
     *
     *  * http://
     *
     *  * load resources using HyperText Transfer Protocol
     *
     *  * file://
     *
     *  * load resources from the Android file system
     *
     *  * asset://
     *
     *  * load resources from the binary packaged assets folder
     *
     *
     *
     * @param id       the source id
     * @param uri      the source uri
     * @param tileSize the tile size
     */
    constructor(id: String?, uri: String?, tileSize: Int) : super() {
        initialize(id, uri, tileSize)
    }

    /**
     * Create the raster source from a [TileSet]
     *
     * @param id      the source id
     * @param tileSet the [TileSet]
     */
    constructor(id: String?, tileSet: TileSet) : super() {
        initialize(id, tileSet.toValueObject(), DEFAULT_TILE_SIZE)
    }

    /**
     * Create the raster source from a [TileSet] with a specific tile size
     *
     * @param id       the source id
     * @param tileSet  the [TileSet]
     * @param tileSize tje tile size
     */
    constructor(id: String?, tileSet: TileSet, tileSize: Int) : super() {
        initialize(id, tileSet.toValueObject(), tileSize)
    }

    /**
     * @return The url or null
     */
    @get:Deprecated("use {@link #getUri()} instead")
    val url: String?
        get() {
            checkThread()
            return nativeGetUrl()
        }

    /**
     * Get the source URI.
     *
     * @return The uri or null
     */
    val uri: String?
        get() {
            checkThread()
            return nativeGetUrl()
        }

    /**
     * Asks the network again for each tile of this source whose last answer was
     * empty (HTTP 204, or 404 for a tile) or an error, and that the map needs now.
     *
     * Each tile keeps what it draws until its new answer lands. Tiles that have data
     * are not asked again, and none is cleared, so the tiles on screen do not flash.
     * A tile that the map does not need now asks again when the map needs it.
     * Core: `mln::style::TileSource::reloadTiles`.
     */
    fun reloadEmptyTiles() {
        checkThread()
        nativeReloadEmptyTiles()
    }

    /**
     * Asks the network or the resource provider again for each tile of this source whose
     * last answer matches [filter] and that the map needs now.
     *
     * Each tile keeps what it draws until its new answer lands. Tiles that have data are
     * not asked again, and none is cleared, so the tiles on screen do not flash. A tile
     * that the map does not need now asks again when the map needs it.
     * Core: `mln::style::TileSource::reloadTiles`.
     *
     * @param filter the last answers to ask again for
     */
    fun reloadTiles(filter: ReloadFilter) {
        checkThread()
        nativeReloadTiles(filter.nativeValue)
    }

    @Keep
    protected external fun initialize(layerId: String?, payload: Any?, tileSize: Int)

    @Keep
    @Throws(Throwable::class)
    protected external fun finalize()

    @Keep
    protected external fun nativeGetUrl(): String

    @Keep
    private external fun nativeReloadEmptyTiles()

    @Keep
    private external fun nativeReloadTiles(filter: Int)

    companion object {
        const val DEFAULT_TILE_SIZE = 512
    }
}
