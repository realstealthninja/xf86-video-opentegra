/*
 * Copyright © 2014 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "driver.h"

#define ErrorMsg(fmt, args...)                                              \
    xf86DrvMsg(-1, X_ERROR, "%s:%d/%s(): " fmt, __FILE__,                   \
               __LINE__, __func__, ##args)

static unsigned TegraPixmapSize(TegraPixmapPtr pixmap);
static unsigned long TegraEXAPixmapOffset(PixmapPtr pix);
static struct drm_tegra_bo * TegraEXAPixmapBO(PixmapPtr pix);
static Bool TegraEXAPrepareCPUAccess(PixmapPtr pPix, int idx, void **ptr);
static void TegraEXAFinishCPUAccess(PixmapPtr pPix, int idx);
static Bool TegraEXAIsPoolPixmap(PixmapPtr pix);

uint64_t tegra_profiler_seqno;

#include "exa_mm_pool.c"
#include "exa_mm.c"
#include "exa_helpers.c"
#include "exa_2d.c"
#include "exa_composite_2d.c"
#include "exa_composite_3d.c"
#include "exa_composite.c"
#include "exa_mm_fridge.c"

#define TEGRA_MALLOC_TRIM_THRESHOLD     256

static Bool TegraEXAIsPoolPixmap(PixmapPtr pix)
{
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pix);

    return priv->type == TEGRA_EXA_PIXMAP_TYPE_POOL;
}

static unsigned long TegraEXAPixmapOffset(PixmapPtr pix)
{
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pix);
    unsigned long offset = 0;

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_POOL)
        offset = mem_pool_entry_offset(&priv->pool_entry);

    return offset;
}

static struct drm_tegra_bo * TegraEXAPixmapBO(PixmapPtr pix)
{
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pix);

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_POOL) {
        TegraPixmapPoolPtr pool = TEGRA_CONTAINER_OF(
                    priv->pool_entry.pool, TegraPixmapPool, pool);
        return pool->bo;
    }

    return priv->bo;
}

unsigned int TegraEXAPitch(unsigned int width, unsigned int height,
                           unsigned int bpp)
{
    unsigned int alignment = 64;

    /* GR3D texture sampler has specific alignment restrictions. */
    if (IS_POW2(width) && IS_POW2(height))
            alignment = 16;

    return TEGRA_PITCH_ALIGN(width, bpp, alignment);
}

static int TegraEXAMarkSync(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;
    union {
        struct tegra_fence *fence;
        int marker;
    } data;

    /* on 32bit ARM size of integer is equal to size of pointer */
    data.fence = TEGRA_STREAM_GET_LAST_FENCE(tegra->cmds);

    /*
     * EXA may take marker multiple times, but it waits only for the
     * lastly taken marker, so we release the previous marker-fence here.
     */
    TEGRA_FENCE_PUT(tegra->scratch.marker);
    tegra->scratch.marker = data.fence;

    return data.marker;
}

static void TegraEXAWaitMarker(ScreenPtr pScreen, int marker)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraEXAPtr tegra = TegraPTR(pScrn)->exa;
    union {
        struct tegra_fence *fence;
        int marker;
    } data;

    data.marker = marker;

    TegraEXAWaitFence(data.fence);
    TEGRA_FENCE_PUT(data.fence);

    /* if it was a lastly-taken marker, then we've just released it */
    if (data.fence == tegra->scratch.marker)
        tegra->scratch.marker = NULL;
}

static PROFILE_DEF(cpu_access);

static Bool TegraEXAPrepareCPUAccess(PixmapPtr pPix, int idx, void **ptr)
{
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPix);
    int err;

    FallbackMsg("pPix %p idx %d type %u %d:%d:%d %p\n",
                pPix, idx, priv->type,
                pPix->drawable.width,
                pPix->drawable.height,
                pPix->drawable.bitsPerPixel,
                pPix->devPrivate.ptr);

    TegraEXAThawPixmap(pPix, FALSE);

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_FALLBACK) {
        PROFILE_START(cpu_access);
        *ptr = priv->fallback;
        return TRUE;
    }

    /*
     * EXA doesn't sync for Upload/DownloadFromScreen, assuming that HW
     * will take care of the fencing.
     *
     * Wait for the HW operations to be completed.
     */
    switch (idx) {
    default:
    case EXA_PREPARE_DEST:
    case EXA_PREPARE_AUX_DEST:
        TEGRA_EXA_WAIT_AND_PUT_FENCE(priv->fence_read);

        /* fall through */
    case EXA_PREPARE_SRC:
    case EXA_PREPARE_MASK:
    case EXA_PREPARE_AUX_SRC:
    case EXA_PREPARE_AUX_MASK:
    case EXA_NUM_PREPARE_INDICES:
        TEGRA_EXA_WAIT_AND_PUT_FENCE(priv->fence_write);
    }

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_POOL) {
        *ptr = TegraEXAPoolMapEntry(&priv->pool_entry);
        PROFILE_START(cpu_access);
        return TRUE;
    }

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_BO) {
        PROFILE_DEF(mmap);
        PROFILE_START(mmap);

        err = drm_tegra_bo_map(priv->bo, ptr);
        if (err < 0) {
            ErrorMsg("failed to map buffer object: %d\n", err);
            return FALSE;
        }

        PROFILE_STOP(mmap);

        PROFILE_START(cpu_access);
        return TRUE;
    }

    return FALSE;
}

static Bool TegraEXAPrepareAccess(PixmapPtr pPix, int idx)
{
    return TegraEXAPrepareCPUAccess(pPix, idx, &pPix->devPrivate.ptr);
}

void TegraEXAFinishCPUAccess(PixmapPtr pPix, int idx)
{
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPix);
    int err;

    PROFILE_STOP(cpu_access);

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_BO) {
        err = drm_tegra_bo_unmap(priv->bo);
        if (err < 0)
            ErrorMsg("failed to unmap buffer object: %d\n", err);
    }

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_POOL)
        TegraEXAPoolUnmapEntry(&priv->pool_entry);

    TegraEXACoolPixmap(pPix, TRUE);

    FallbackMsg("pPix %p idx %d\n", pPix, idx);
}

static void TegraEXAFinishAccess(PixmapPtr pPix, int idx)
{
    TegraEXAFinishCPUAccess(pPix, idx);
}

static Bool TegraEXAPixmapIsOffscreen(PixmapPtr pPix)
{
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPix);

    return priv && priv->accel && priv->tegra_data;
}

static void TegraEXATrimHeap(TegraEXAPtr exa)
{
    /*
     * Default trimming threshold isn't good for us, that results in
     * a big amounts of wasted memory due to high fragmentation. Hence
     * manually enforce trimming of the heap when it makes sense.
     */
#ifdef __GLIBC__
    if (exa->release_count > TEGRA_MALLOC_TRIM_THRESHOLD) {
        exa->release_count = 0;
        malloc_trim(0);
    }
#endif
}

static void TegraEXAReleasePixmapData(TegraPtr tegra, TegraPixmapPtr priv)
{
    TegraEXAPtr exa = tegra->exa;
    Bool force_fencing = false;
    int drm_ver;

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_NONE) {
        if (priv->frozen) {
#ifdef HAVE_JPEG
            if (priv->compression_type == TEGRA_EXA_COMPRESSION_JPEG)
                tjFree(priv->compressed_data);
            else
#endif
                free(priv->compressed_data);

            priv->frozen = FALSE;
            exa->release_count++;
        }

        goto out_final;
    } else if (tegra->exa_erase_pixmaps) {
        /* clear released data for privacy protection */
        TegraEXAClearPixmapData(priv, TRUE);
    }

    if (priv->cold) {
        exa->cooling_size -= TegraPixmapSize(priv);
        xorg_list_del(&priv->fridge_entry);
        priv->cold = FALSE;
    }

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_FALLBACK) {
        free(priv->fallback);
        exa->release_count++;
        goto out_final;
    }

    /*
     * Pool allocation data is sprayed with 0x88 if canary-debugging is
     * enabled, see mem_pool_free(). In this case we need to enforce
     * the fence-waiting, otherwise there will be visible glitches if
     * pixmap is released before GPU rendering is finished.
     *
     * One example where problem is visible is a "magnus" application of
     * MATE DE, click on the magnus itself to see the corrupted image of
     * the checkerboard background that app draws.
     *
     * Secondly, we can't check the pool allocation HW usage status at
     * allocation time, so we always need to fence a such allocation.
     */
    force_fencing = (priv->type == TEGRA_EXA_PIXMAP_TYPE_POOL);

    drm_ver = drm_tegra_version(tegra->drm);

    /*
     * We have to await the fence to avoid BO re-use while job is in progress,
     * this will be resolved by BO reservation that right now isn't supported
     * by vanilla upstream kernel driver.
     */
    if (priv->fence_read) {
        if (force_fencing || drm_ver < GRATE_KERNEL_DRM_VERSION)
            TegraEXAWaitFence(priv->fence_read);

        TEGRA_FENCE_PUT(priv->fence_read);
        priv->fence_read = NULL;
    }

    if (priv->fence_write) {
        if (force_fencing || drm_ver < GRATE_KERNEL_DRM_VERSION)
            TegraEXAWaitFence(priv->fence_write);

        TEGRA_FENCE_PUT(priv->fence_write);
        priv->fence_write = NULL;
    }

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_POOL) {
        TegraEXAPoolFree(&priv->pool_entry);
        goto out_final;
    }

    if (priv->type == TEGRA_EXA_PIXMAP_TYPE_BO) {
        drm_tegra_bo_unref(priv->bo);
        goto out_final;
    }

out_final:
    priv->type = TEGRA_EXA_PIXMAP_TYPE_NONE;
    TegraEXATrimHeap(exa);
}

unsigned TegraEXAHeightHwAligned(unsigned int height, unsigned int bpp)
{
    /*
     * Some of GR2D units operate with 16x16 (bytes) blocks, other HW units
     * may too.
     */
    return TEGRA_ALIGN(height, 16 / (bpp >> 3));
}

static unsigned TegraEXAPixmapSizeAligned(unsigned pitch, unsigned height,
                                          unsigned bpp)
{
    unsigned int size;

    size = pitch * TegraEXAHeightHwAligned(height, bpp);

    return TEGRA_ALIGN(size, TEGRA_EXA_OFFSET_ALIGN);
}

static unsigned TegraPixmapSize(TegraPixmapPtr pixmap)
{
    PixmapPtr pPixmap = pixmap->pPixmap;

    if (pixmap->offscreen)
        return TegraEXAPixmapSizeAligned(pPixmap->devKind,
                                         pPixmap->drawable.height,
                                         pPixmap->drawable.bitsPerPixel);

    return pPixmap->devKind * pPixmap->drawable.height;
}

static Bool TegraEXAAccelerated(unsigned bpp)
{
    return bpp == 8 || bpp == 16 || bpp == 32;
}

static Bool TegraEXAAllocatePixmapData(TegraPtr tegra,
                                       TegraPixmapPtr pixmap,
                                       unsigned int width,
                                       unsigned int height,
                                       unsigned int bpp,
                                       int usage_hint)
{
    unsigned int pitch = TegraEXAPitch(width, height, bpp);
    unsigned int size = pitch * height;

    pixmap->tegra_data = TRUE;
    pixmap->accel = TegraEXAAccelerated(bpp);

    if (usage_hint == TEGRA_DRI_USAGE_HINT)
        pixmap->dri = TRUE;

    /* DRI allocation must be accelerateable, otherwise what's the point? */
    if (pixmap->dri && !pixmap->accel)
        return FALSE;

    /*
     * Optimize allocation for 1x1 drawable as we will simply always
     * avoid sampling from a such textures.
     */
    if ((!pixmap->dri && width == 1 && height == 1) || !pixmap->accel)
        return TegraEXAAllocateMem(pixmap, size);

    if (pixmap->accel) {
        pixmap->offscreen = 1;
        size = TegraEXAPixmapSizeAligned(pitch, height, bpp);
    }

    /*
     * Allocation is deferred to TegraEXAThawPixmap() invocation
     * because there is no point to allocate BO if pixmap won't
     * be ever used for accelerated drawing. A set usage_hint
     * usually means that we really want to allocate data right
     * now, this will also bypass data-zeroing that is performed
     * for deferred allocations and shouldn't be needed for the
     * internal use.
     */
    if (!usage_hint && tegra->exa_refrigerator)
        return TRUE;

    return (TegraEXAAllocateDRMFromPool(tegra, pixmap, size) ||
            TegraEXAAllocateDRM(tegra, pixmap, size) ||
            TegraEXAAllocateMem(pixmap, size));
}

static void *TegraEXACreatePixmap2(ScreenPtr pScreen, int width, int height,
                                   int depth, int usage_hint, int bitsPerPixel,
                                   int *new_fb_pitch)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraPtr tegra = TegraPTR(pScrn);
    TegraPixmapPtr pixmap;

    pixmap = calloc(1, sizeof(*pixmap));
    if (!pixmap)
        return NULL;

    switch (bitsPerPixel) {
    case 8:
        pixmap->picture_format = PICT_a8;
        break;

    default:
        break;
    }

    if (width > 0 && height > 0 && bitsPerPixel > 0) {
        *new_fb_pitch = TegraEXAPitch(width, height, bitsPerPixel);

        if (!TegraEXAAllocatePixmapData(tegra, pixmap, width, height,
                                        bitsPerPixel, usage_hint)) {
            free(pixmap);
            return NULL;
        }
    } else {
        *new_fb_pitch = 0;
    }

    DebugMsg("priv %p type %u %d:%d:%d stride %d usage_hint 0x%x (%c%c%c%c)\n",
             pixmap, pixmap->type, width, height, bitsPerPixel,
             *new_fb_pitch, usage_hint,
             usage_hint >> 24, usage_hint >> 16, usage_hint >> 8, usage_hint);

    return pixmap;
}

static void TegraEXADestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraPtr tegra = TegraPTR(pScrn);
    TegraPixmapPtr priv = driverPriv;

    DebugMsg("pPix %p priv %p type %u %d:%d:%d stride %d\n",
             priv->pPixmap, priv, priv->type,
             priv->pPixmap->drawable.width,
             priv->pPixmap->drawable.height,
             priv->pPixmap->drawable.bitsPerPixel,
             priv->pPixmap->devKind);

    TegraEXAReleasePixmapData(tegra, priv);
    free(priv);
}

static Bool TegraEXAModifyPixmapHeader(PixmapPtr pPixmap, int width,
                                       int height, int depth, int bitsPerPixel,
                                       int devKind, pointer pPixData)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    TegraPtr tegra = TegraPTR(pScrn);
    struct drm_tegra_bo *scanout;
    Bool ret;

    ret = miModifyPixmapHeader(pPixmap, width, height, depth, bitsPerPixel,
                               devKind, pPixData);
    if (!ret)
        return FALSE;

    if (pPixData) {
        TegraEXAReleasePixmapData(tegra, priv);

        if (pPixData == drmmode_map_front_bo(&tegra->drmmode)) {
            scanout = drmmode_get_front_bo(&tegra->drmmode);
            priv->type = TEGRA_EXA_PIXMAP_TYPE_BO;
            priv->bo = drm_tegra_bo_ref(scanout);
            priv->tegra_data = TRUE;
            priv->offscreen = TRUE;
            priv->scanout = TRUE;
            priv->accel = TRUE;
            goto success;
        }

        if (pPixData == drmmode_crtc_map_rotate_bo(pScrn, 0)) {
            scanout = drmmode_crtc_get_rotate_bo(pScrn, 0);
            priv->type = TEGRA_EXA_PIXMAP_TYPE_BO;
            priv->bo = drm_tegra_bo_ref(scanout);
            priv->scanout_rotated = TRUE;
            priv->tegra_data = TRUE;
            priv->offscreen = TRUE;
            priv->scanout = TRUE;
            priv->accel = TRUE;
            priv->crtc = 0;
            goto success;
        }

        if (pPixData == drmmode_crtc_map_rotate_bo(pScrn, 1)) {
            scanout = drmmode_crtc_get_rotate_bo(pScrn, 1);
            priv->type = TEGRA_EXA_PIXMAP_TYPE_BO;
            priv->bo = drm_tegra_bo_ref(scanout);
            priv->scanout_rotated = TRUE;
            priv->tegra_data = TRUE;
            priv->offscreen = TRUE;
            priv->scanout = TRUE;
            priv->accel = TRUE;
            priv->crtc = 1;
            goto success;
        }
    } else if (!priv->accel && priv->tegra_data) {
        /* this tells EXA that this pixmap is unacceleratable */
        pPixmap->devPrivate.ptr = priv->fallback;
    }

    priv->pPixmap = pPixmap;
    TegraEXACoolTegraPixmap(tegra, priv);

success:
    DebugMsg("pPix %p priv %p type %u %d:%d:%d stride %d %d:%d:%d:%p\n",
             pPixmap, priv, priv->type,
             pPixmap->drawable.width,
             pPixmap->drawable.height,
             pPixmap->drawable.bitsPerPixel,
             pPixmap->devKind,
             width, height, bitsPerPixel, pPixData);

    return TRUE;
}

static void
TegraSelectCopyFunc(char *dst, const char *src, int line_len,
                    Bool download, Bool src_cached, Bool dst_cached,
                    tegra_vfp_func *pvfp_func, bool *pvfp_threaded)
{
    tegra_vfp_func vfp_func;
    bool vfp_threaded;
    bool vfp_safe;

    vfp_safe = tegra_memcpy_vfp_copy_safe(dst, src, line_len);

    if (vfp_safe && download && !src_cached) {
        vfp_threaded = true;
        vfp_func     = tegra_memcpy_vfp_aligned;

    } if (vfp_safe && !src_cached && dst_cached) {
        vfp_threaded = true;
        vfp_func     = tegra_memcpy_vfp_aligned_dst_cached;

    } if (vfp_safe && src_cached && !dst_cached) {
        vfp_threaded = false;
        vfp_func     = tegra_memcpy_vfp_aligned_src_cached;

    } else if (download && !src_cached) {
        vfp_threaded = true;
        vfp_func     = tegra_memcpy_vfp_unaligned;

    } else {
        vfp_threaded = false;
        vfp_func     = NULL;
    }

    *pvfp_threaded = vfp_threaded;
    *pvfp_func     = vfp_func;
}

static Bool
TegraEXACopyScreen(const char *src, int src_pitch, int height,
                   Bool download, Bool src_cached, Bool dst_cached,
                   char *dst, int dst_pitch, int line_len)
{
    tegra_vfp_func vfp_func;
    bool vfp_threaded;

    PROFILE_DEF(screen_load);

    if (src_pitch == line_len && src_pitch == dst_pitch) {
        line_len *= height;
        height = 1;
    }

    PROFILE_SET_NAME(screen_load, download ? "download" : "upload");
    PROFILE_START(screen_load);

    while (height--) {
        TegraSelectCopyFunc(dst, src, line_len,
                            download, src_cached, dst_cached,
                            &vfp_func, &vfp_threaded);

        if (vfp_func) {
            if (vfp_threaded)
                tegra_memcpy_vfp_threaded(dst, src, line_len, vfp_func);
            else
                vfp_func(dst, src, line_len);
        } else {
            memcpy(dst, src, line_len);
        }

        src += src_pitch;
        dst += dst_pitch;
    }

    PROFILE_STOP(screen_load);

    return TRUE;
}

static Bool
TegraEXALoadScreen(PixmapPtr pix, int x, int y, int w, int h,
                   char *usr, int usr_pitch, Bool download)
{
    TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pix);
    int offset, pitch, line_len, cpp;
    Bool src_cached, dst_cached;
    int access_hint;
    char *pmap;
    Bool ret;

    cpp      = pix->drawable.bitsPerPixel >> 3;
    pitch    = exaGetPixmapPitch(pix);
    offset   = (y * pitch) + (x * cpp);
    line_len = w * cpp;

    if (!line_len || !priv->tegra_data) {
        FallbackMsg("unaccelerateable pixmap %d:%d, %dx%d %d:%d\n",
                    pix->drawable.width, pix->drawable.height, x, y, w, h);
        return FALSE;
    }

    access_hint = download ? EXA_PREPARE_SRC : EXA_PREPARE_DEST;
    ret = TegraEXAPrepareCPUAccess(pix, access_hint, (void**)&pmap);
    if (!ret)
        return FALSE;

    if (download) {
        if (priv->type == TEGRA_EXA_PIXMAP_TYPE_FALLBACK)
            src_cached = TRUE;
        else
            src_cached = FALSE;

        dst_cached = TRUE;
    } else {
        if (priv->type == TEGRA_EXA_PIXMAP_TYPE_FALLBACK)
            dst_cached = TRUE;
        else
            dst_cached = FALSE;

        src_cached = TRUE;
    }

    AccelMsg("%dx%d %d:%d\n", x, y, w, h);

    if (download)
        ret = TegraEXACopyScreen(pmap + offset, pitch, h,
                                 download, src_cached, dst_cached,
                                 usr, usr_pitch, line_len);
    else
        ret = TegraEXACopyScreen(usr, usr_pitch, h,
                                 download, src_cached, dst_cached,
                                 pmap + offset, pitch, line_len);

    TegraEXAFinishCPUAccess(pix, EXA_PREPARE_SRC);

    return ret;
}

static Bool
TegraEXADownloadFromScreen(PixmapPtr pSrc, int x, int y, int w, int h,
                           char *dst, int dst_pitch)
{
    return TegraEXALoadScreen(pSrc, x, y, w, h, dst, dst_pitch, TRUE);
}

static Bool
TegraEXAUploadToScreen(PixmapPtr pDst, int x, int y, int w, int h,
                       char *src, int src_pitch)
{
    return TegraEXALoadScreen(pDst, x, y, w, h, src, src_pitch, FALSE);
}

static PixmapPtr TegraEXAGetDrawablePixmap(DrawablePtr drawable)
{
    if (drawable->type == DRAWABLE_PIXMAP)
        return (PixmapPtr) drawable;

    return NULL;
}

static int TegraEXACreatePicture(PicturePtr pPicture)
{
    PixmapPtr pPixmap = TegraEXAGetDrawablePixmap(pPicture->pDrawable);
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPicture->pDrawable->pScreen);
    TegraEXAPtr exa = TegraPTR(pScrn)->exa;

    if (pPixmap) {
        TegraPixmapPtr priv = exaGetPixmapDriverPrivate(pPixmap);
        priv->picture_format = pPicture->format;
    }

    if (exa->CreatePicture)
        return exa->CreatePicture(pPicture);

    return Success;
}

static void TegraEXABlockHandler(BLOCKHANDLER_ARGS_DECL)
{
    SCREEN_PTR(arg);
    TegraPtr tegra = TegraPTR(xf86ScreenToScrn(pScreen));
    TegraEXAPtr exa = tegra->exa;
    struct timespec time;

    pScreen->BlockHandler = exa->BlockHandler;
    pScreen->BlockHandler(BLOCKHANDLER_ARGS);
    pScreen->BlockHandler = TegraEXABlockHandler;

    clock_gettime(CLOCK_MONOTONIC, &time);
    TegraEXAFreezePixmaps(tegra, time.tv_sec);

    drm_tegra_bo_cache_cleanup(tegra->drm, time.tv_sec);
}

static void TegraEXAWrapProc(ScreenPtr pScreen)
{
    PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraEXAPtr exa = TegraPTR(pScrn)->exa;

    if (ps) {
        exa->CreatePicture = ps->CreatePicture;
        ps->CreatePicture = TegraEXACreatePicture;
    }

    exa->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = TegraEXABlockHandler;
}

static void TegraEXAUnWrapProc(ScreenPtr pScreen)
{
    PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraEXAPtr exa = TegraPTR(pScrn)->exa;

    if (ps)
        ps->CreatePicture = exa->CreatePicture;

    pScreen->BlockHandler = exa->BlockHandler;
}

Bool TegraEXAScreenInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraPtr tegra = TegraPTR(pScrn);
    ExaDriverPtr exa;
    TegraEXAPtr priv;
    int drm_ver;
    int err;

    drm_ver = drm_tegra_version(tegra->drm);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Tegra DRM kernel version %d\n",
               drm_ver);

    if (!tegra->exa_enabled)
        return TRUE;

    exa = exaDriverAlloc();
    if (!exa) {
        ErrorMsg("EXA allocation failed\n");
        return FALSE;
    }

    priv = calloc(1, sizeof(*priv));
    if (!priv) {
        ErrorMsg("EXA allocation failed\n");
        goto free_exa;
    }

    tegra->exa = priv;

    err = drm_tegra_channel_open(&priv->gr2d, tegra->drm, DRM_TEGRA_GR2D);
    if (err < 0) {
        ErrorMsg("failed to open 2D channel: %d\n", err);
        goto free_priv;
    }

    err = drm_tegra_channel_open(&priv->gr3d, tegra->drm, DRM_TEGRA_GR3D);
    if (err < 0) {
        ErrorMsg("failed to open 3D channel: %d\n", err);
        goto close_gr2d;
    }

    err = tegra_stream_create(&priv->cmds, tegra);
    if (err < 0) {
        ErrorMsg("failed to create command stream: %d\n", err);
        goto close_gr3d;
    }

    err = TegraEXAInitMM(tegra, priv);
    if (err) {
        ErrorMsg("TegraEXAInitMM failed\n");
        goto destroy_stream;
    }

    exa->exa_major = EXA_VERSION_MAJOR;
    exa->exa_minor = EXA_VERSION_MINOR;
    exa->pixmapOffsetAlign = TEGRA_EXA_OFFSET_ALIGN;
    exa->pixmapPitchAlign = TegraEXAPitch(1, 1, 32);
    exa->flags = EXA_SUPPORTS_PREPARE_AUX |
                 EXA_OFFSCREEN_PIXMAPS |
                 EXA_HANDLES_PIXMAPS;

    exa->maxX = 8192;
    exa->maxY = 8192;

    exa->MarkSync = TegraEXAMarkSync;
    exa->WaitMarker = TegraEXAWaitMarker;

    exa->PrepareAccess = TegraEXAPrepareAccess;
    exa->FinishAccess = TegraEXAFinishAccess;
    exa->PixmapIsOffscreen = TegraEXAPixmapIsOffscreen;

    exa->CreatePixmap2 = TegraEXACreatePixmap2;
    exa->DestroyPixmap = TegraEXADestroyPixmap;
    exa->ModifyPixmapHeader = TegraEXAModifyPixmapHeader;

    exa->PrepareSolid = TegraEXAPrepareSolid;
    exa->Solid = TegraEXASolid;
    exa->DoneSolid = TegraEXADoneSolid;

    exa->PrepareCopy = TegraEXAPrepareCopy;
    exa->Copy = TegraEXACopy;
    exa->DoneCopy = TegraEXADoneCopy;

    exa->CheckComposite = TegraEXACheckComposite;
    exa->PrepareComposite = TegraEXAPrepareComposite;
    exa->Composite = TegraEXAComposite;
    exa->DoneComposite = TegraEXADoneComposite;

    exa->DownloadFromScreen = TegraEXADownloadFromScreen;
    exa->UploadToScreen = TegraEXAUploadToScreen;

    if (!exaDriverInit(pScreen, exa)) {
        ErrorMsg("EXA initialization failed\n");
        goto release_mm;
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "EXA initialized\n");

    priv->driver = exa;

    TegraEXAWrapProc(pScreen);

    TegraGR3DStateReset(&priv->gr3d_state);

    priv->scratch.drm = tegra->drm;

    if (drm_ver >= GRATE_KERNEL_DRM_VERSION) {
        priv->default_drm_bo_flags = DRM_TEGRA_GEM_CREATE_DONT_KMAP;
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "EXA using GEM DONT_KMAP\n");
    }
    if (drm_ver >= GRATE_KERNEL_DRM_VERSION + 1) {
        /*
         * Just print message without using sparse allocation for the large
         * pool because it will hog most of GART aperture.
         */
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "EXA using GEM CREATE_SPARSE\n");
    }

    return TRUE;

release_mm:
    TegraEXAReleaseMM(tegra, priv);
destroy_stream:
    tegra_stream_destroy(priv->cmds);
close_gr3d:
    drm_tegra_channel_close(priv->gr3d);
close_gr2d:
    drm_tegra_channel_close(priv->gr2d);
free_priv:
    free(priv);
free_exa:
    free(exa);

    tegra->exa = NULL;

    return FALSE;
}

void TegraEXAScreenExit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TegraPtr tegra = TegraPTR(pScrn);
    TegraEXAPtr priv = tegra->exa;

    if (priv) {
        exaDriverFini(pScreen);
        TegraGR3DStateReset(&priv->gr3d_state);
        TegraEXAUnWrapProc(pScreen);
        free(priv->driver);

        TegraEXAReleaseMM(tegra, priv);
        tegra_stream_destroy(priv->cmds);
        drm_tegra_channel_close(priv->gr2d);
        drm_tegra_channel_close(priv->gr3d);
        free(priv);

        tegra->exa = NULL;
    }
}

/* vim: set et sts=4 sw=4 ts=4: */
