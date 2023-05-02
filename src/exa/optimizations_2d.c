/*
 * Copyright (c) GRATE-DRIVER project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define DISABLE_2D_OPTIMIZATIONS    true

static void
tegra_exa_prepare_optimized_solid_fill(PixmapPtr pixmap, Pixel color)
{
    struct tegra_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    ScrnInfoPtr scrn = xf86ScreenToScrn(pixmap->drawable.pScreen);
    struct tegra_exa *tegra = TegraPTR(scrn)->exa;
    bool cpu_access = true;
    bool optimize = true;

    if (tegra->in_2d_flush || priv->scanout)
        optimize = false;

    if (tegra_exa_pixmap_is_busy(tegra, priv))
        cpu_access = false;

    /*
     * This optimization pass skips the solid-fill drawing operation if
     * whole pixmap is filled with the same color. The operation isn't
     * canceled, but deferred.
     */

    if (DISABLE_2D_OPTIMIZATIONS) {
        cpu_access = false;
        optimize = false;
    }

    tegra->scratch.color = color;
    tegra->scratch.optimize = optimize;
    tegra->scratch.cpu_access = cpu_access;
    tegra->scratch.cpu_ptr = NULL;

    priv->freezer_lockcnt++;
}

static bool tegra_exa_optimize_solid_op(PixmapPtr pixmap,
                                        int px1, int py1,
                                        int px2, int py2)
{
    struct tegra_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    ScrnInfoPtr scrn = xf86ScreenToScrn(pixmap->drawable.pScreen);
    struct tegra_exa *tegra = TegraPTR(scrn)->exa;
    unsigned int cpp = pixmap->drawable.bitsPerPixel >> 3;
    unsigned int bytes = (px2 - px1) * (py2 - py1) * cpp;
    bool alpha_0 = 0;

    if ((cpp == 4 && !(tegra->scratch.color & 0xff000000)) ||
        (cpp == 1 && !(tegra->scratch.color & 0x00)))
    {
        if (priv->state.alpha_0 || (px1 == 0 && py1 == 0 &&
            pixmap->drawable.width == px2 &&
            pixmap->drawable.height == py2))
            alpha_0 = !DISABLE_2D_OPTIMIZATIONS;
    }

    if (priv->state.alpha_0 && !alpha_0)
        DEBUG_MSG("pixmap %p solid-fill canceled alpha_0\n", pixmap);

    if (tegra->scratch.optimize &&
        px1 == 0 && py1 == 0 &&
        pixmap->drawable.width == px2 &&
        pixmap->drawable.height == py2)
    {
        tegra_exa_cancel_deferred_operations(pixmap);

        DEBUG_MSG("pixmap %p applying deferred solid-fill optimization\n",
                  pixmap);

        tegra->scratch.cpu_access = false;

        priv->state.solid_color = tegra->scratch.color;
        priv->state.solid_fill = 1;
        priv->state.alpha_0 = alpha_0;

        return true;
    }

    if (tegra->scratch.optimize &&
        priv->state.solid_fill &&
        priv->state.solid_color == tegra->scratch.color &&
        bytes < TEGRA_EXA_CPU_FILL_MIN_SIZE)
    {
        DEBUG_MSG("pixmap %p partial solid-fill optimized out\n", pixmap);
        return true;
    }

    if (tegra->scratch.optimize && priv->state.solid_fill) {
        tegra_exa_flush_deferred_operations(pixmap, true, true, true);

        if (tegra->scratch.cpu_access && tegra_exa_pixmap_is_busy(tegra, priv))
            tegra->scratch.cpu_access = false;
    }

    /*
     * It's much more optimal to perform small write-only operations on CPU
     * if GPU isn't touching pixmap. The job submission overhead is too big
     * + this allows to perform operation in parallel with GPU.
     */
    if (tegra->scratch.cpu_access && bytes < TEGRA_EXA_CPU_FILL_MIN_SIZE &&
        (tegra->scratch.cpu_ptr || tegra_exa_prepare_cpu_access(pixmap, EXA_PREPARE_DEST,
                                                                &tegra->scratch.cpu_ptr,
                                                                false)))
    {
        DEBUG_MSG("pixmap %p partial solid-fill optimized to a CPU-fill\n",
                  pixmap);

        pixman_fill(tegra->scratch.cpu_ptr,
                    pixmap->devKind / 4,
                    pixmap->drawable.bitsPerPixel,
                    px1, py1, px2 - px1, py2 - py1,
                    tegra->scratch.color);

        priv->state.alpha_0 = alpha_0;

        return true;
    }

    priv->state.alpha_0 = alpha_0;

    return false;
}

static void tegra_exa_complete_solid_fill_optimization(PixmapPtr pixmap)
{
    struct tegra_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    ScrnInfoPtr scrn = xf86ScreenToScrn(pixmap->drawable.pScreen);
    struct tegra_exa *tegra = TegraPTR(scrn)->exa;

    if (tegra->scratch.ops)
        tegra_exa_flush_deferred_operations(pixmap, true, true, true);

    if (tegra->scratch.cpu_ptr) {
        tegra->scratch.cpu_ptr = NULL;
        tegra_exa_finish_cpu_access(pixmap, EXA_PREPARE_DEST);
    }

    tegra->scratch.optimize = false;

    priv->freezer_lockcnt--;
}

static void tegra_exa_perform_deferred_solid_fill(PixmapPtr pixmap, bool accel)
{
    struct tegra_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    ScrnInfoPtr scrn = xf86ScreenToScrn(pixmap->drawable.pScreen);
    struct tegra_exa *tegra = TegraPTR(scrn)->exa;

    if (priv->state.solid_fill) {
        DEBUG_MSG("pixmap %p %d:%d:%d performing deferred solid-fill (%08lx)\n",
                  pixmap,
                  pixmap->drawable.width,
                  pixmap->drawable.height,
                  pixmap->drawable.bitsPerPixel,
                  priv->state.solid_color);
        priv->state.solid_fill = 0;

        tegra_exa_wrap_state(tegra, &tegra->opt_state[TEGRA_OPT_SOLID]);
        tegra_exa_fill_pixmap_data(priv, accel, priv->state.solid_color);
        tegra_exa_unwrap_state(tegra, &tegra->opt_state[TEGRA_OPT_SOLID]);
    }
}

static bool tegra_exa_optimize_same_color_copy(struct tegra_pixmap *src_priv,
                                               struct tegra_pixmap *dst_priv)
{
    if (src_priv->state.solid_fill && dst_priv->state.solid_fill &&
        src_priv->state.solid_color == dst_priv->state.solid_color)
        return true;

    return false;
}

static void tegra_exa_prepare_optimized_copy(PixmapPtr src_pixmap,
                                             PixmapPtr dst_pixmap,
                                             int op, Pixel planemask)
{
    struct tegra_pixmap *src_priv = exaGetPixmapDriverPrivate(src_pixmap);
    struct tegra_pixmap *dst_priv = exaGetPixmapDriverPrivate(dst_pixmap);
    ScrnInfoPtr scrn = xf86ScreenToScrn(dst_pixmap->drawable.pScreen);
    struct tegra_exa *tegra = TegraPTR(scrn)->exa;
    bool optimize = true;

    /*
     * This optimization pass turns copy operations into a solid color
     * fill, it also cancels deferred solid fill operation of dst_pixmap
     * if src_pixmap is copied over the whole dst_pixmap.
     */

    if (!src_priv->state.solid_fill && !dst_priv->state.solid_fill &&
        !src_priv->state.alpha_0    && !dst_priv->state.alpha_0)
        optimize = false;

    if (DISABLE_2D_OPTIMIZATIONS)
        optimize = false;

    if (optimize && src_priv->state.solid_fill) {
        if (tegra_exa_optimize_same_color_copy(src_priv, dst_priv)) {
            DEBUG_MSG("pixmap %p -> %p copy optimized out to a same-color solid-fill\n",
                      src_pixmap, dst_pixmap);
        } else {
            DEBUG_MSG("pixmap %p -> %p copy optimized to a partial solid-fill\n",
                      src_pixmap, dst_pixmap);
        }

        tegra_exa_wrap_state(tegra, &tegra->opt_state[TEGRA_OPT_COPY]);

        optimize = tegra_exa_prepare_solid_2d(dst_pixmap, op, planemask,
                                              src_priv->state.solid_color);

        tegra_exa_unwrap_state(tegra, &tegra->opt_state[TEGRA_OPT_COPY]);
    }

    tegra->scratch.optimize = optimize;

    src_priv->freezer_lockcnt++;
    dst_priv->freezer_lockcnt++;

    ACCEL_MSG("\n");
}

static bool tegra_exa_optimize_copy_op(PixmapPtr dst_pixmap,
                                       int dst_x, int dst_y,
                                       int width, int height)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(dst_pixmap->drawable.pScreen);
    struct tegra_exa *tegra = TegraPTR(scrn)->exa;
    PixmapPtr src_pixmap = tegra->scratch.src;

    ACCEL_MSG("\n");

    if (tegra->scratch.optimize) {
        struct tegra_pixmap *src_priv = exaGetPixmapDriverPrivate(src_pixmap);
        struct tegra_pixmap *dst_priv = exaGetPixmapDriverPrivate(dst_pixmap);

        if (src_priv->state.solid_fill) {
            if (tegra_exa_optimize_same_color_copy(src_priv, dst_priv))
                return true;

            tegra_exa_wrap_state(tegra, &tegra->opt_state[TEGRA_OPT_COPY]);
            tegra_exa_solid_2d(dst_pixmap, dst_x, dst_y, dst_x + width, dst_y + height);
            tegra_exa_unwrap_state(tegra, &tegra->opt_state[TEGRA_OPT_COPY]);

            return true;
        } else if (dst_x == 0 && dst_y == 0 &&
                   dst_pixmap->drawable.width == width &&
                   dst_pixmap->drawable.height == height) {
            tegra_exa_cancel_deferred_operations(dst_pixmap);

            if (dst_priv->state.alpha_0 && !src_priv->state.alpha_0)
                DEBUG_MSG("pixmap %p copy canceled alpha_0\n", dst_pixmap);

            dst_priv->state.alpha_0 = src_priv->state.alpha_0;
        } else {
            if (!src_priv->state.alpha_0 && dst_priv->state.alpha_0) {
                DEBUG_MSG("pixmap %p copy canceled alpha_0\n", dst_pixmap);

                dst_priv->state.alpha_0 = 0;
            }
        }
    }

    return false;
}

static void tegra_exa_complete_solid_fill_copy_optimization(PixmapPtr dst_pixmap)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(dst_pixmap->drawable.pScreen);
    struct tegra_exa *tegra = TegraPTR(scrn)->exa;

    tegra_exa_wrap_state(tegra, &tegra->opt_state[TEGRA_OPT_COPY]);
    tegra_exa_done_solid_2d(dst_pixmap);
    tegra_exa_unwrap_state(tegra, &tegra->opt_state[TEGRA_OPT_COPY]);

    ACCEL_MSG("\n");
}

static void tegra_exa_complete_copy_optimization(PixmapPtr dst_pixmap)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(dst_pixmap->drawable.pScreen);
    struct tegra_exa *tegra = TegraPTR(scrn)->exa;
    PixmapPtr src_pixmap = tegra->scratch.src;
    struct tegra_pixmap *src_priv = exaGetPixmapDriverPrivate(src_pixmap);
    struct tegra_pixmap *dst_priv = exaGetPixmapDriverPrivate(dst_pixmap);

    if (tegra->scratch.optimize && src_priv->state.solid_fill) {
        tegra_exa_complete_solid_fill_copy_optimization(dst_pixmap);
    } else if (tegra->scratch.ops) {
        tegra_exa_flush_deferred_operations(src_pixmap, true, false, true);
        tegra_exa_flush_deferred_operations(dst_pixmap, true, true, true);
    }

    tegra->scratch.optimize = false;

    src_priv->freezer_lockcnt--;
    dst_priv->freezer_lockcnt--;

    ACCEL_MSG("\n");
}

static void tegra_exa_flush_deferred_2d_operations(PixmapPtr pixmap,
                                                   bool accel,
                                                   bool flush_reads,
                                                   bool flush_writes)
{
    struct tegra_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    ScrnInfoPtr scrn = xf86ScreenToScrn(pixmap->drawable.pScreen);
    struct tegra_exa *tegra = TegraPTR(scrn)->exa;

    if (tegra->in_2d_flush)
        return;

    if (DISABLE_2D_OPTIMIZATIONS) {
        assert(!priv->state.solid_fill);
        return;
    }

    if (priv->state.solid_fill)
        DEBUG_MSG("pixmap %p flushing deferred operations\n", pixmap);

    tegra->in_2d_flush = true;
    tegra_exa_perform_deferred_solid_fill(pixmap, accel);
    tegra->in_2d_flush = false;
}

/* vim: set et sts=4 sw=4 ts=4: */
