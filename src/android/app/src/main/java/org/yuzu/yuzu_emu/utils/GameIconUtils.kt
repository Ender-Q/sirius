// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.drawable.LayerDrawable
import android.widget.ImageView
import androidx.core.content.res.ResourcesCompat
import androidx.core.graphics.drawable.IconCompat
import androidx.core.graphics.drawable.toBitmap
import androidx.core.graphics.drawable.toDrawable
import androidx.lifecycle.LifecycleOwner
import coil3.ImageLoader
import coil3.asImage
import coil3.decode.DataSource
import coil3.fetch.FetchResult
import coil3.fetch.Fetcher
import coil3.fetch.ImageFetchResult
import coil3.key.Keyer
import coil3.memory.MemoryCache
import coil3.request.ImageRequest
import coil3.request.Options
import coil3.request.SuccessResult
import coil3.request.error
import coil3.request.lifecycle
import coil3.request.target
import coil3.toBitmap
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.Game

class GameIconFetcher(
    private val game: Game,
    private val options: Options
) : Fetcher {
    override suspend fun fetch(): FetchResult {
        val bitmap = decodeGameIcon(game.path)!!
        val image = bitmap.asImage()

        return ImageFetchResult(
            image = image,
            isSampled = false,
            dataSource = DataSource.DISK
        )
    }

    private fun decodeGameIcon(uri: String): Bitmap? {
        val data = GameMetadata.getIcon(uri)
        return BitmapFactory.decodeByteArray(
            data,
            0,
            data.size,
            BitmapFactory.Options()
        )
    }

    class Factory : Fetcher.Factory<Game> {
        override fun create(data: Game, options: Options, imageLoader: ImageLoader): Fetcher =
            GameIconFetcher(data, options)
    }
}

class GameIconKeyer : Keyer<Game> {
    override fun key(data: Game, options: Options): String = data.path
}

object GameIconUtils {
    private val imageLoader = ImageLoader.Builder(YuzuApplication.appContext)
        .components {
            add(GameIconKeyer())
            add(GameIconFetcher.Factory())
        }
        .memoryCache {
            MemoryCache.Builder()
                .maxSizePercent(YuzuApplication.appContext, 0.25)
                .build()
        }
        .build()

    fun loadGameIcon(game: Game, imageView: ImageView) {
        val request = ImageRequest.Builder(YuzuApplication.appContext)
            .data(game)
            .target(imageView)
            .error(R.drawable.default_icon)
            .build()
        imageLoader.enqueue(request)
    }

    suspend fun getGameIcon(lifecycleOwner: LifecycleOwner, game: Game): Bitmap {
        val request = ImageRequest.Builder(YuzuApplication.appContext)
            .data(game)
            .lifecycle(lifecycleOwner)
            .error(R.drawable.default_icon)
            .build()

        val result = imageLoader.execute(request)

        return if (result is SuccessResult) {
            result.image.toBitmap()
        } else {
            val default = ResourcesCompat.getDrawable(
                YuzuApplication.appContext.resources,
                R.drawable.default_icon,
                null
            )
            default!!.toBitmap()
        }
    }

    suspend fun getShortcutIcon(lifecycleOwner: LifecycleOwner, game: Game): IconCompat {
        val layerDrawable = ResourcesCompat.getDrawable(
            YuzuApplication.appContext.resources,
            R.drawable.shortcut,
            null
        ) as LayerDrawable

        val iconBitmap = getGameIcon(lifecycleOwner, game)

        layerDrawable.setDrawableByLayerId(
            R.id.shortcut_foreground,
            iconBitmap.toDrawable(YuzuApplication.appContext.resources)
        )
        val inset = YuzuApplication.appContext.resources
            .getDimensionPixelSize(R.dimen.icon_inset)
        layerDrawable.setLayerInset(1, inset, inset, inset, inset)
        return IconCompat.createWithAdaptiveBitmap(
            layerDrawable.toBitmap(config = Bitmap.Config.ARGB_8888)
        )
    }
}
