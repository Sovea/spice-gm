/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2015 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include <config.h>

#include <common/utils.h>
#include "dcc-private.h"
#include "display-channel.h"
#include "display-channel-private.h"
#include "red-client.h"
#include "main-channel-client.h"
#include <spice-server-enums.h>

#define DISPLAY_CLIENT_SHORT_TIMEOUT 15000000000ULL //nano
#define DISPLAY_FREE_LIST_DEFAULT_SIZE 128

static void dcc_init_stream_agents(DisplayChannelClient *dcc);

DisplayChannelClient::DisplayChannelClient(DisplayChannel *display,
                         RedClient *client, RedStream *stream,
                         RedChannelCapabilities *caps,
                         uint32_t id,
                         SpiceImageCompression image_compression,
                         spice_wan_compression_t jpeg_state,
                         spice_wan_compression_t zlib_glz_state):
    CommonGraphicsChannelClient(display, client, stream, caps, true),
    priv(new DisplayChannelClientPrivate)
{

    // XXX from display_channel_client_init, put somewhere else
    ring_init(&priv->palette_cache_lru);
    // todo: tune quality according to bandwidth
    priv->encoders.jpeg_quality = 85;

    priv->send_data.free_list.res = (SpiceResourceList*)
        g_malloc(sizeof(SpiceResourceList) +
                 DISPLAY_FREE_LIST_DEFAULT_SIZE * sizeof(SpiceResourceID));
    priv->send_data.free_list.res_size = DISPLAY_FREE_LIST_DEFAULT_SIZE;


    priv->image_compression = image_compression;
    priv->jpeg_state = jpeg_state;
    priv->zlib_glz_state = zlib_glz_state;


    priv->id = id;

    image_encoders_init(&priv->encoders, &DCC_TO_DC(this)->priv->encoder_shared_data);

    dcc_init_stream_agents(this);
}

DisplayChannelClient::~DisplayChannelClient()
{
    g_clear_pointer(&priv->preferred_video_codecs, g_array_unref);
    g_clear_pointer(&priv->client_preferred_video_codecs, g_array_unref);
}

RedSurfaceCreateItem::RedSurfaceCreateItem(uint32_t surface_id,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t format,
                                           uint32_t flags)
{
    surface_create.surface_id = surface_id;
    surface_create.width = width;
    surface_create.height = height;
    surface_create.flags = flags;
    surface_create.format = format;
}

bool dcc_drawable_is_in_pipe(DisplayChannelClient *dcc, Drawable *drawable)
{
    RedDrawablePipeItem *dpi;
    GList *l;

    for (l = drawable->pipes; l != nullptr; l = l->next) {
        dpi = (RedDrawablePipeItem *) l->data;
        if (dpi->dcc == dcc) {
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * Return: TRUE if wait_if_used == FALSE, or otherwise, if all of the pipe items that
 * are related to the surface have been cleared (or sent) from the pipe.
 */
bool dcc_clear_surface_drawables_from_pipe(DisplayChannelClient *dcc, int surface_id,
                                           int wait_if_used)
{
    DisplayChannel *display;
    RedSurface *surface;

    spice_return_val_if_fail(dcc != nullptr, TRUE);
    /* removing the newest drawables that their destination is surface_id and
       no other drawable depends on them */

    display = DCC_TO_DC(dcc);
    surface = &display->priv->surfaces[surface_id];

    auto &pipe = dcc->get_pipe();
    for (auto l = pipe.begin(); l != pipe.end(); ) {
        Drawable *drawable;
        RedDrawablePipeItem *dpi = nullptr;
        RedPipeItem *item = l->get();
        auto item_pos = l;

        ++l;
        if (item->type == RED_PIPE_ITEM_TYPE_DRAW) {
            dpi = static_cast<RedDrawablePipeItem*>(item);
            drawable = dpi->drawable;
        } else if (item->type == RED_PIPE_ITEM_TYPE_UPGRADE) {
            drawable = static_cast<RedUpgradeItem*>(item)->drawable;
        } else {
            continue;
        }

        if (drawable->surface == surface) {
            l = pipe.erase(item_pos);
            continue;
        }

        auto depend_found =
            std::find(std::begin(drawable->surface_deps), std::end(drawable->surface_deps),
                      surface) != std::end(drawable->surface_deps);
        if (depend_found) {
            spice_debug("surface %d dependent item found %p, %p", surface_id, drawable, item);
            if (!wait_if_used) {
                return TRUE;
            }
            return dcc->wait_pipe_item_sent(item_pos, COMMON_CLIENT_TIMEOUT);
        }
    }

    if (!wait_if_used) {
        return TRUE;
    }

    /*
     * in case that the pipe didn't contain any item that is dependent on the surface, but
     * there is one during sending. Use a shorter timeout, since it is just one item
     */
    return dcc->wait_outgoing_item(DISPLAY_CLIENT_SHORT_TIMEOUT);
}

void dcc_create_surface(DisplayChannelClient *dcc, int surface_id)
{
    DisplayChannel *display;
    RedSurface *surface;
    uint32_t flags;

    if (!dcc) {
        return;
    }

    display = DCC_TO_DC(dcc);
    flags = is_primary_surface_id(display, surface_id) ? SPICE_SURFACE_FLAGS_PRIMARY : 0;

    /* don't send redundant create surface commands to client */
    if (display->get_during_target_migrate() ||
        dcc->priv->surface_client_created[surface_id]) {
        return;
    }
    surface = &display->priv->surfaces[surface_id];
    auto create = red::make_shared<RedSurfaceCreateItem>(surface_id, surface->context.width,
                                                         surface->context.height,
                                                         surface->context.format, flags);
    dcc->priv->surface_client_created[surface_id] = TRUE;
    dcc->pipe_add(create);
}

// adding the pipe item after pos. If pos == NULL, adding to head.
void
dcc_add_surface_area_image(DisplayChannelClient *dcc, int surface_id,
                           SpiceRect *area, RedChannelClient::Pipe::iterator pipe_item_pos,
                           int can_lossy)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    RedSurface *surface = &display->priv->surfaces[surface_id];
    SpiceCanvas *canvas = surface->context.canvas;
    int stride;
    int width;
    int height;
    int bpp;
    int all_set;

    spice_assert(area);

    width = area->right - area->left;
    height = area->bottom - area->top;
    bpp = SPICE_SURFACE_FMT_DEPTH(surface->context.format) / 8;
    stride = width * bpp;

    red::shared_ptr<RedImageItem> item(new (height * stride) RedImageItem());

    item->surface_id = surface_id;
    item->image_format =
        spice_bitmap_from_surface_type(surface->context.format);
    item->image_flags = 0;
    item->pos.x = area->left;
    item->pos.y = area->top;
    item->width = width;
    item->height = height;
    item->stride = stride;
    item->top_down = surface->context.top_down;
    item->can_lossy = can_lossy;

    canvas->ops->read_bits(canvas, item->data, stride, area);

    /* For 32bit non-primary surfaces we need to keep any non-zero
       high bytes as the surface may be used as source to an alpha_blend */
    if (!is_primary_surface(display, surface) &&
        item->image_format == SPICE_BITMAP_FMT_32BIT &&
        rgb32_data_has_alpha(item->width, item->height, item->stride, item->data, &all_set)) {
        if (all_set) {
            item->image_flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
        } else {
            item->image_format = SPICE_BITMAP_FMT_RGBA;
        }
    }

    if (pipe_item_pos != dcc->get_pipe().end()) {
        dcc->pipe_add_after_pos(item, pipe_item_pos);
    } else {
        dcc->pipe_add(item);
    }
}

void dcc_push_surface_image(DisplayChannelClient *dcc, int surface_id)
{
    DisplayChannel *display;
    SpiceRect area;
    RedSurface *surface;

    if (!dcc) {
        return;
    }

    display = DCC_TO_DC(dcc);
    surface = &display->priv->surfaces[surface_id];
    if (!surface->context.canvas) {
        return;
    }
    area.top = area.left = 0;
    area.right = surface->context.width;
    area.bottom = surface->context.height;

    /* not allowing lossy compression because probably, especially if it is a primary surface,
       it combines both "picture-like" areas with areas that are more "artificial"*/
    dcc_add_surface_area_image(dcc, surface_id, &area, dcc->get_pipe().end(), FALSE);
}

static void add_drawable_surface_images(DisplayChannelClient *dcc, Drawable *drawable)
{
    DisplayChannel *display = DCC_TO_DC(dcc);

    for (const auto surface : drawable->surface_deps) {
        if (surface != nullptr) {
            const auto surface_id = surface->id;
            if (dcc->priv->surface_client_created[surface_id]) {
                continue;
            }
            dcc_create_surface(dcc, surface_id);
            display_channel_current_flush(display, surface_id);
            dcc_push_surface_image(dcc, surface_id);
        }
    }

    if (dcc->priv->surface_client_created[drawable->surface->id]) {
        return;
    }

    dcc_create_surface(dcc, drawable->surface->id);
    display_channel_current_flush(display, drawable->surface->id);
    dcc_push_surface_image(dcc, drawable->surface->id);
}

RedDrawablePipeItem::RedDrawablePipeItem(DisplayChannelClient *init_dcc, Drawable *init_drawable):
    drawable(init_drawable),
    dcc(init_dcc)
{
    drawable->pipes = g_list_prepend(drawable->pipes, this);
    drawable->refs++;
}

RedDrawablePipeItem::~RedDrawablePipeItem()
{
    drawable->pipes = g_list_remove(drawable->pipes, this);
    drawable_unref(drawable);
}

void dcc_prepend_drawable(DisplayChannelClient *dcc, Drawable *drawable)
{
    auto dpi = red::make_shared<RedDrawablePipeItem>(dcc, drawable);

    add_drawable_surface_images(dcc, drawable);
    dcc->pipe_add(dpi);
}

void dcc_append_drawable(DisplayChannelClient *dcc, Drawable *drawable)
{
    auto dpi = red::make_shared<RedDrawablePipeItem>(dcc, drawable);

    add_drawable_surface_images(dcc, drawable);
    dcc->pipe_add_tail(dpi);
}

void dcc_add_drawable_after(DisplayChannelClient *dcc, Drawable *drawable, RedPipeItem *pos)
{
    auto dpi = red::make_shared<RedDrawablePipeItem>(dcc, drawable);

    add_drawable_surface_images(dcc, drawable);
    dcc->pipe_add_after(dpi, pos);
}

static void dcc_init_stream_agents(DisplayChannelClient *dcc)
{
    int i;
    DisplayChannel *display = DCC_TO_DC(dcc);

    for (i = 0; i < NUM_STREAMS; i++) {
        VideoStreamAgent *agent = &dcc->priv->stream_agents[i];
        agent->stream = display_channel_get_nth_video_stream(display, i);
        region_init(&agent->vis_region);
        region_init(&agent->clip);
    }
}

DisplayChannelClient *dcc_new(DisplayChannel *display,
                              RedClient *client, RedStream *stream,
                              int mig_target,
                              RedChannelCapabilities *caps,
                              SpiceImageCompression image_compression,
                              spice_wan_compression_t jpeg_state,
                              spice_wan_compression_t zlib_glz_state)

{
    auto dcc =
        red::make_shared<DisplayChannelClient>(display, client, stream, caps,
                                               display->priv->qxl->id, image_compression,
                                               jpeg_state, zlib_glz_state);
    if (!dcc->init()) {
        return nullptr;
    }
    spice_debug("New display (client %p) dcc %p stream %p", client, dcc.get(), stream);
    display->set_during_target_migrate(mig_target);

    return dcc.get();
}

static void dcc_create_all_streams(DisplayChannelClient *dcc)
{
    Ring *ring = &DCC_TO_DC(dcc)->priv->streams;
    RingItem *item = ring;

    while ((item = ring_next(ring, item))) {
        VideoStream *stream = SPICE_CONTAINEROF(item, VideoStream, link);
        dcc_create_stream(dcc, stream);
    }
}

/* TODO: this function is evil^Wsynchronous, fix */
static bool display_channel_client_wait_for_init(DisplayChannelClient *dcc)
{
    dcc->priv->expect_init = TRUE;
    uint64_t end_time = spice_get_monotonic_time_ns() + COMMON_CLIENT_TIMEOUT;
    for (;;) {
        dcc->receive();
        if (!dcc->is_connected()) {
            break;
        }
        if (dcc->priv->pixmap_cache && dcc->priv->encoders.glz_dict) {
            dcc->priv->pixmap_cache_generation = dcc->priv->pixmap_cache->generation;
            /* TODO: move common.id? if it's used for a per client structure.. */
            spice_debug("creating encoder with id == %d", dcc->priv->id);
            if (!image_encoders_glz_create(&dcc->priv->encoders, dcc->priv->id)) {
                spice_critical("create global lz failed");
            }
            return TRUE;
        }
        if (spice_get_monotonic_time_ns() > end_time) {
            spice_warning("timeout");
            dcc->disconnect();
            break;
        }
        usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
    }
    return FALSE;
}

void dcc_start(DisplayChannelClient *dcc)
{
    DisplayChannel *display = DCC_TO_DC(dcc);

    dcc->push_set_ack();

    if (dcc->is_waiting_for_migrate_data())
        return;

    if (!display_channel_client_wait_for_init(dcc))
        return;

    red::shared_ptr<DisplayChannelClient> self(dcc);
    dcc->ack_zero_messages_window();
    if (display->priv->surfaces[0].context.canvas) {
        display_channel_current_flush(display, 0);
        dcc->pipe_add_type(RED_PIPE_ITEM_TYPE_INVAL_PALETTE_CACHE);
        dcc_create_surface(dcc, 0);
        dcc_push_surface_image(dcc, 0);
        dcc_push_monitors_config(dcc);
        dcc->pipe_add_empty_msg(SPICE_MSG_DISPLAY_MARK);
        dcc_create_all_streams(dcc);
    }

    if (red_stream_is_plain_unix(dcc->get_stream()) &&
        dcc->test_remote_cap(SPICE_DISPLAY_CAP_GL_SCANOUT)) {
        dcc->pipe_add(dcc_gl_scanout_item_new(dcc, nullptr, 0));
        dcc_push_monitors_config(dcc);
    }
}

static void dcc_destroy_stream_agents(DisplayChannelClient *dcc)
{
    for (auto& agent : dcc->priv->stream_agents) {
        region_destroy(&agent.vis_region);
        region_destroy(&agent.clip);
        if (agent.video_encoder) {
            agent.video_encoder->destroy(agent.video_encoder);
            agent.video_encoder = nullptr;
        }
    }
}

static void dcc_stop(DisplayChannelClient *dcc)
{
    DisplayChannel *dc = DCC_TO_DC(dcc);

    pixmap_cache_unref(dcc->priv->pixmap_cache);
    dcc->priv->pixmap_cache = nullptr;
    dcc_palette_cache_reset(dcc);
    g_free(dcc->priv->send_data.free_list.res);
    dcc_destroy_stream_agents(dcc);
    image_encoders_free(&dcc->priv->encoders);

    if (dcc->priv->gl_draw_ongoing) {
        display_channel_gl_draw_done(dc);
    }
}

void dcc_video_stream_agent_clip(DisplayChannelClient* dcc, VideoStreamAgent *agent)
{
    auto item = red::make_shared<VideoStreamClipItem>(agent);

    dcc->pipe_add(item);
}

RedMonitorsConfigItem::~RedMonitorsConfigItem()
{
    monitors_config_unref(monitors_config);
}

RedMonitorsConfigItem::RedMonitorsConfigItem(MonitorsConfig *init_monitors_config)
{
    monitors_config = monitors_config_ref(init_monitors_config);
}

void dcc_push_monitors_config(DisplayChannelClient *dcc)
{
    DisplayChannel *dc = DCC_TO_DC(dcc);
    MonitorsConfig *monitors_config = dc->priv->monitors_config;

    if (monitors_config == nullptr) {
        spice_warning("monitors_config is NULL");
        return;
    }

    if (!dcc->test_remote_cap(SPICE_DISPLAY_CAP_MONITORS_CONFIG)) {
        return;
    }

    auto mci = red::make_shared<RedMonitorsConfigItem>(monitors_config);
    dcc->pipe_add(mci);
}

RedSurfaceDestroyItem::RedSurfaceDestroyItem(uint32_t surface_id)
{
    surface_destroy.surface_id = surface_id;
}

RedPipeItemPtr dcc_gl_scanout_item_new(RedChannelClient *rcc, void *data, int num)
{
    /* FIXME: on !unix peer, start streaming with a video codec */
    if (!red_stream_is_plain_unix(rcc->get_stream()) ||
        !rcc->test_remote_cap(SPICE_DISPLAY_CAP_GL_SCANOUT)) {
        red_channel_warning(rcc->get_channel(),
                            "FIXME: client does not support GL scanout");
        rcc->disconnect();
        return RedPipeItemPtr();
    }

    return red::make_shared<RedGlScanoutUnixItem>();
}

XXX_CAST(RedChannelClient, DisplayChannelClient, DISPLAY_CHANNEL_CLIENT);

RedPipeItemPtr dcc_gl_draw_item_new(RedChannelClient *rcc, void *data, int num)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    auto draw = (const SpiceMsgDisplayGlDraw *) data;

    if (!red_stream_is_plain_unix(rcc->get_stream()) ||
        !rcc->test_remote_cap(SPICE_DISPLAY_CAP_GL_SCANOUT)) {
        red_channel_warning(rcc->get_channel(),
                            "FIXME: client does not support GL scanout");
        rcc->disconnect();
        return RedPipeItemPtr();
    }

    dcc->priv->gl_draw_ongoing = TRUE;
    auto item = red::make_shared<RedGlDrawItem>();
    item->draw = *draw;

    return item;
}

void dcc_destroy_surface(DisplayChannelClient *dcc, uint32_t surface_id)
{
    DisplayChannel *display;

    if (!dcc) {
        return;
    }

    display = DCC_TO_DC(dcc);

    if (display->get_during_target_migrate() ||
        !dcc->priv->surface_client_created[surface_id]) {
        return;
    }

    dcc->priv->surface_client_created[surface_id] = FALSE;
    auto destroy = red::make_shared<RedSurfaceDestroyItem>(surface_id);
    dcc->pipe_add(destroy);
}

#define MIN_DIMENSION_TO_QUIC 3
/**
 * quic doesn't handle:
 *       (1) palette
 */
static bool can_quic_compress(SpiceBitmap *bitmap)
{
    return !bitmap_fmt_is_plt(bitmap->format) &&
            bitmap->x >= MIN_DIMENSION_TO_QUIC && bitmap->y >= MIN_DIMENSION_TO_QUIC;
}
/**
 * lz/glz doesn't handle:
 *       (1) bitmaps with strides that are larger than the width of the image in bytes
 *       (2) unstable bitmaps
 */
static bool can_lz_compress(SpiceBitmap *bitmap)
{
    return !bitmap_has_extra_stride(bitmap) &&
           !(bitmap->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE);
}

#define MIN_SIZE_TO_COMPRESS 54
static SpiceImageCompression get_compression_for_bitmap(SpiceBitmap *bitmap,
                                                        SpiceImageCompression preferred_compression,
                                                        Drawable *drawable)
{
    if (bitmap->y * bitmap->stride < MIN_SIZE_TO_COMPRESS) { // TODO: change the size cond
        return SPICE_IMAGE_COMPRESSION_OFF;
    }
    if (preferred_compression == SPICE_IMAGE_COMPRESSION_OFF) {
        return SPICE_IMAGE_COMPRESSION_OFF;
    }
    if (preferred_compression == SPICE_IMAGE_COMPRESSION_QUIC) {
        if (can_quic_compress(bitmap)) {
            return SPICE_IMAGE_COMPRESSION_QUIC;
        }
        return SPICE_IMAGE_COMPRESSION_OFF;
    }

    if (preferred_compression == SPICE_IMAGE_COMPRESSION_AUTO_GLZ ||
        preferred_compression == SPICE_IMAGE_COMPRESSION_AUTO_LZ) {
        if (can_quic_compress(bitmap)) {
            if (drawable == nullptr ||
                drawable->copy_bitmap_graduality == BITMAP_GRADUAL_INVALID) {
                if (bitmap_fmt_has_graduality(bitmap->format) &&
                    bitmap_get_graduality_level(bitmap) == BITMAP_GRADUAL_HIGH) {
                    return SPICE_IMAGE_COMPRESSION_QUIC;
                }
            } else if (drawable->copy_bitmap_graduality == BITMAP_GRADUAL_HIGH) {
                return SPICE_IMAGE_COMPRESSION_QUIC;
            }
            if (!can_lz_compress(bitmap)) {
                return SPICE_IMAGE_COMPRESSION_QUIC;
            }
        }
        if (preferred_compression == SPICE_IMAGE_COMPRESSION_AUTO_LZ) {
            preferred_compression = SPICE_IMAGE_COMPRESSION_LZ;
        } else {
            preferred_compression = SPICE_IMAGE_COMPRESSION_GLZ;
        }
    }

    if (preferred_compression == SPICE_IMAGE_COMPRESSION_GLZ) {
        if (drawable == nullptr || !bitmap_fmt_has_graduality(bitmap->format)) {
            preferred_compression = SPICE_IMAGE_COMPRESSION_LZ;
        }
    }

    if (preferred_compression == SPICE_IMAGE_COMPRESSION_LZ4) {
        if (!bitmap_fmt_is_rgb(bitmap->format)) {
            preferred_compression = SPICE_IMAGE_COMPRESSION_LZ;
        }
    }

    if (preferred_compression == SPICE_IMAGE_COMPRESSION_LZ ||
        preferred_compression == SPICE_IMAGE_COMPRESSION_LZ4 ||
        preferred_compression == SPICE_IMAGE_COMPRESSION_GLZ) {
        if (can_lz_compress(bitmap)) {
            return preferred_compression;
        }
        return SPICE_IMAGE_COMPRESSION_OFF;
    }

    return SPICE_IMAGE_COMPRESSION_INVALID;
}

int dcc_compress_image(DisplayChannelClient *dcc,
                       SpiceImage *dest, SpiceBitmap *src, Drawable *drawable,
                       int can_lossy,
                       compress_send_data_t* o_comp_data)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    SpiceImageCompression image_compression;
    stat_start_time_t start_time;
    int success = FALSE;

    stat_start_time_init(&start_time, &display_channel->priv->encoder_shared_data.off_stat);

    image_compression = get_compression_for_bitmap(src, dcc->priv->image_compression, drawable);
    switch (image_compression) {
    case SPICE_IMAGE_COMPRESSION_OFF:
        break;
    case SPICE_IMAGE_COMPRESSION_QUIC:
        if (can_lossy && display_channel->priv->enable_jpeg &&
            (src->format != SPICE_BITMAP_FMT_RGBA || !bitmap_has_extra_stride(src))) {
            success = image_encoders_compress_jpeg(&dcc->priv->encoders, dest, src, o_comp_data);
            break;
        }
        success = image_encoders_compress_quic(&dcc->priv->encoders, dest, src, o_comp_data);
        break;
    case SPICE_IMAGE_COMPRESSION_GLZ:
        success = image_encoders_compress_glz(&dcc->priv->encoders, dest, src,
                                              drawable->red_drawable.get(), &drawable->glz_retention,
                                              o_comp_data,
                                              display_channel->priv->enable_zlib_glz_wrap);
        if (success) {
            break;
        }
        goto lz_compress;
#ifdef USE_LZ4
    case SPICE_IMAGE_COMPRESSION_LZ4:
        if (dcc->test_remote_cap(SPICE_DISPLAY_CAP_LZ4_COMPRESSION)) {
            success = image_encoders_compress_lz4(&dcc->priv->encoders, dest, src, o_comp_data);
            break;
        }
#endif
        /* fall through */
    case SPICE_IMAGE_COMPRESSION_LZ:
lz_compress:
        success = image_encoders_compress_lz(&dcc->priv->encoders, dest, src, o_comp_data);
        if (success && !bitmap_fmt_is_rgb(src->format)) {
            dcc_palette_cache_palette(dcc, dest->u.lz_plt.palette, &(dest->u.lz_plt.flags));
        }
        break;
    default:
        spice_error("invalid image compression type %u", image_compression);
    }

    if (!success) {
        uint64_t image_size = src->stride * (uint64_t)src->y;
        stat_compress_add(&display_channel->priv->encoder_shared_data.off_stat, start_time, image_size, image_size);
    }

    return success;
}

#define CLIENT_PALETTE_CACHE
#include "cache-item.tmpl.cpp"
#undef CLIENT_PALETTE_CACHE

void dcc_palette_cache_palette(DisplayChannelClient *dcc, SpicePalette *palette,
                               uint8_t *flags)
{
    if (palette == nullptr) {
        return;
    }
    if (palette->unique) {
        if (red_palette_cache_find(dcc, palette->unique)) {
            *flags |= SPICE_BITMAP_FLAGS_PAL_FROM_CACHE;
            return;
        }
        if (red_palette_cache_add(dcc, palette->unique, 1)) {
            *flags |= SPICE_BITMAP_FLAGS_PAL_CACHE_ME;
        }
    }
}

void dcc_palette_cache_reset(DisplayChannelClient *dcc)
{
    red_palette_cache_reset(dcc, CLIENT_PALETTE_CACHE_SIZE);
}

static void dcc_push_release(DisplayChannelClient *dcc, uint8_t type, uint64_t id,
                             uint64_t* sync_data)
{
    FreeList *free_list = &dcc->priv->send_data.free_list;
    int i;

    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        free_list->sync[i] = MAX(free_list->sync[i], sync_data[i]);
    }

    if (free_list->res->count == free_list->res_size) {
        free_list->res = (SpiceResourceList*) g_realloc(free_list->res,
                                   sizeof(*free_list->res) +
                                   free_list->res_size * sizeof(SpiceResourceID) * 2);
        free_list->res_size *= 2;
    }
    free_list->res->resources[free_list->res->count].type = type;
    free_list->res->resources[free_list->res->count++].id = id;
}

bool dcc_pixmap_cache_unlocked_add(DisplayChannelClient *dcc, uint64_t id,
                                   uint32_t size, int lossy)
{
    PixmapCache *cache = dcc->priv->pixmap_cache;
    NewCacheItem *item;
    uint64_t serial;
    int key;

    spice_assert(size > 0);

    item = g_new(NewCacheItem, 1);
    serial = dcc->get_message_serial();

    if (cache->generation != dcc->priv->pixmap_cache_generation) {
        if (!dcc->priv->pending_pixmaps_sync) {
            dcc->pipe_add_type(RED_PIPE_ITEM_TYPE_PIXMAP_SYNC);
            dcc->priv->pending_pixmaps_sync = TRUE;
        }
        g_free(item);
        return FALSE;
    }

    cache->available -= size;
    while (cache->available < 0) {
        NewCacheItem *tail;
        NewCacheItem **now;

        SPICE_VERIFY(SPICE_OFFSETOF(NewCacheItem, lru_link) == 0);
        if (!(tail = SPICE_CONTAINEROF(ring_get_tail(&cache->lru), NewCacheItem, lru_link)) ||
                                                     tail->sync[dcc->priv->id] == serial) {
            cache->available += size;
            g_free(item);
            return FALSE;
        }

        now = &cache->hash_table[BITS_CACHE_HASH_KEY(tail->id)];
        for (;;) {
            spice_assert(*now);
            if (*now == tail) {
                *now = tail->next;
                break;
            }
            now = &(*now)->next;
        }
        ring_remove(&tail->lru_link);
        cache->available += tail->size;
        cache->sync[dcc->priv->id] = serial;
        dcc_push_release(dcc, SPICE_RES_TYPE_PIXMAP, tail->id, tail->sync);
        g_free(tail);
    }
    item->next = cache->hash_table[(key = BITS_CACHE_HASH_KEY(id))];
    cache->hash_table[key] = item;
    ring_item_init(&item->lru_link);
    ring_add(&cache->lru, &item->lru_link);
    item->id = id;
    item->size = size;
    item->lossy = lossy;
    memset(item->sync, 0, sizeof(item->sync));
    item->sync[dcc->priv->id] = serial;
    cache->sync[dcc->priv->id] = serial;
    return TRUE;
}

static bool dcc_handle_init(DisplayChannelClient *dcc, SpiceMsgcDisplayInit *init)
{
    gboolean success;
    RedClient *client = dcc->get_client();

    spice_return_val_if_fail(dcc->priv->expect_init, FALSE);
    dcc->priv->expect_init = FALSE;

    spice_return_val_if_fail(!dcc->priv->pixmap_cache, FALSE);
    dcc->priv->pixmap_cache = pixmap_cache_get(client,
                                               init->pixmap_cache_id,
                                               init->pixmap_cache_size);
    spice_return_val_if_fail(dcc->priv->pixmap_cache, FALSE);

    success = image_encoders_get_glz_dictionary(&dcc->priv->encoders,
                                                client,
                                                init->glz_dictionary_id,
                                                init->glz_dictionary_window_size);
    spice_return_val_if_fail(success, FALSE);

    return TRUE;
}

static bool dcc_handle_stream_report(DisplayChannelClient *dcc,
                                     SpiceMsgcDisplayStreamReport *report)
{
    VideoStreamAgent *agent;

    if (report->stream_id >= NUM_STREAMS) {
        spice_warning("stream_report: invalid stream id %u",
                      report->stream_id);
        return FALSE;
    }

    agent = &dcc->priv->stream_agents[report->stream_id];
    if (!agent->video_encoder) {
        spice_debug("stream_report: no encoder for stream id %u. "
                   "The stream has probably been destroyed",
                   report->stream_id);
        return TRUE;
    }

    if (report->num_frames == 0 && report->num_drops == UINT32_MAX) {
        spice_warning("stream_report: the client does not support stream %u",
                      report->stream_id);
        /* Stop streaming the video so the client can see it */
        agent->video_encoder->destroy(agent->video_encoder);
        agent->video_encoder = nullptr;
        return TRUE;
    }

    if (report->unique_id != agent->report_id) {
        spice_warning("stream_report: unique id mismatch: local (%u) != msg (%u) "
                      "The old stream was probably replaced by a new one",
                      agent->report_id, report->unique_id);
        return TRUE;
    }

    agent->video_encoder->client_stream_report(agent->video_encoder,
                                               report->num_frames,
                                               report->num_drops,
                                               report->start_frame_mm_time,
                                               report->end_frame_mm_time,
                                               report->last_frame_delay,
                                               report->audio_delay);
    return TRUE;
}

static bool dcc_handle_preferred_compression(DisplayChannelClient *dcc,
                                             SpiceMsgcDisplayPreferredCompression *pc)
{
    switch (pc->image_compression) {
    case SPICE_IMAGE_COMPRESSION_AUTO_LZ:
    case SPICE_IMAGE_COMPRESSION_AUTO_GLZ:
    case SPICE_IMAGE_COMPRESSION_QUIC:
#ifdef USE_LZ4
    case SPICE_IMAGE_COMPRESSION_LZ4:
#endif
    case SPICE_IMAGE_COMPRESSION_LZ:
    case SPICE_IMAGE_COMPRESSION_GLZ:
    case SPICE_IMAGE_COMPRESSION_OFF:
        dcc->priv->image_compression = (SpiceImageCompression) pc->image_compression;
        break;
    default:
        spice_warning("preferred-compression: unsupported image compression setting");
    }
    g_debug("Setting preferred compression to %s",
            spice_image_compression_t_get_nick(dcc->priv->image_compression));
    return TRUE;
}

/* TODO: Client preference should only be considered when host has video-codecs
 * with the same priority value. At the moment, the video-codec GArray will be
 * sorted following only the client's preference (@user_data)
 *
 * example:
 * host encoding preference: gstreamer:mjpeg;gstreamer:vp8;gstreamer:h264
 * client decoding preference: h264, vp9, mjpeg
 * result: gstreamer:h264;gstreamer:mjpeg;gstreamer:vp8
 */
static gint sort_video_codecs_by_client_preference(gconstpointer a_pointer,
                                                   gconstpointer b_pointer,
                                                   gpointer user_data)
{
    auto a = (const RedVideoCodec *) a_pointer;
    auto b = (const RedVideoCodec *) b_pointer;
    auto client_pref = (GArray *) user_data;

    return (g_array_index(client_pref, gint, a->type) -
            g_array_index(client_pref, gint, b->type));
}

static void dcc_update_preferred_video_codecs(DisplayChannelClient *dcc)
{
    GArray *video_codecs, *server_codecs;
    char *codecs_str;

    server_codecs = display_channel_get_video_codecs(DCC_TO_DC(dcc));
    spice_return_if_fail(server_codecs != nullptr);

    /* Copy current host preference */
    video_codecs = g_array_sized_new(FALSE, FALSE, sizeof(RedVideoCodec), server_codecs->len);
    g_array_append_vals(video_codecs, server_codecs->data, server_codecs->len);

    /* Sort the copy of current host preference based on client's preference */
    g_array_sort_with_data(video_codecs, sort_video_codecs_by_client_preference,
                           dcc->priv->client_preferred_video_codecs);
    g_clear_pointer(&dcc->priv->preferred_video_codecs, g_array_unref);
    dcc->priv->preferred_video_codecs = video_codecs;

    codecs_str = video_codecs_to_string(video_codecs, " ");
    spice_debug("Preferred video-codecs: %s", codecs_str);
    g_free(codecs_str);
}

void dcc_video_codecs_update(DisplayChannelClient *dcc)
{
    /* Only worry about video-codecs update if client has sent
     * SPICE_MSGC_DISPLAY_PREFERRED_VIDEO_CODEC_TYPE */
    if (dcc->priv->client_preferred_video_codecs == nullptr) {
        return;
    }

    /* New host preference */
    dcc_update_preferred_video_codecs(dcc);
}

static int dcc_handle_preferred_video_codec_type(DisplayChannelClient *dcc,
                                                 SpiceMsgcDisplayPreferredVideoCodecType *msg)
{
    g_return_val_if_fail(msg->num_of_codecs > 0, TRUE);

    g_clear_pointer(&dcc->priv->client_preferred_video_codecs, g_array_unref);
    dcc->priv->client_preferred_video_codecs = video_stream_parse_preferred_codecs(msg);

    /* New client preference */
    dcc_update_preferred_video_codecs(dcc);
    video_stream_detach_and_stop(DCC_TO_DC(dcc));

    return TRUE;
}

GArray *dcc_get_preferred_video_codecs_for_encoding(DisplayChannelClient *dcc)
{
    if (dcc->priv->preferred_video_codecs != nullptr) {
        return dcc->priv->preferred_video_codecs;
    }
    return display_channel_get_video_codecs(DCC_TO_DC(dcc));
}

static bool dcc_handle_gl_draw_done(DisplayChannelClient *dcc)
{
    DisplayChannel *display = DCC_TO_DC(dcc);

    if (G_UNLIKELY(!dcc->priv->gl_draw_ongoing)) {
        g_warning("unexpected DRAW_DONE received\n");
        /* close client connection */
        return FALSE;
    }

    dcc->priv->gl_draw_ongoing = FALSE;
    display_channel_gl_draw_done(display);

    return TRUE;
}

bool DisplayChannelClient::handle_message(uint16_t type, uint32_t size, void *msg)
{
    switch (type) {
    case SPICE_MSGC_DISPLAY_INIT:
        return dcc_handle_init(this, (SpiceMsgcDisplayInit *)msg);
    case SPICE_MSGC_DISPLAY_STREAM_REPORT:
        return dcc_handle_stream_report(this, (SpiceMsgcDisplayStreamReport *)msg);
    case SPICE_MSGC_DISPLAY_PREFERRED_COMPRESSION:
        return dcc_handle_preferred_compression(this,
            (SpiceMsgcDisplayPreferredCompression *)msg);
    case SPICE_MSGC_DISPLAY_GL_DRAW_DONE:
        return dcc_handle_gl_draw_done(this);
    case SPICE_MSGC_DISPLAY_PREFERRED_VIDEO_CODEC_TYPE:
        return dcc_handle_preferred_video_codec_type(this,
            (SpiceMsgcDisplayPreferredVideoCodecType *)msg);
    default:
        return RedChannelClient::handle_message(type, size, msg);
    }
}

static int dcc_handle_migrate_glz_dictionary(DisplayChannelClient *dcc,
                                             SpiceMigrateDataDisplay *migrate)
{
    GlzEncDictRestoreData glz_dict_data = migrate->glz_dict_data;
    return image_encoders_restore_glz_dictionary(&dcc->priv->encoders,
                                                 dcc->get_client(),
                                                 migrate->glz_dict_id,
                                                 &glz_dict_data);
}

static bool restore_surface(DisplayChannelClient *dcc, uint32_t surface_id)
{
    /* we don't process commands till we receive the migration data, thus,
     * we should have not sent any surface to the client. */
    if (dcc->priv->surface_client_created[surface_id]) {
        spice_warning("surface %u is already marked as client_created", surface_id);
        return FALSE;
    }
    dcc->priv->surface_client_created[surface_id] = TRUE;
    return TRUE;
}

static bool restore_surfaces_lossless(DisplayChannelClient *dcc,
                                      MigrateDisplaySurfacesAtClientLossless *mig_surfaces)
{
    uint32_t i;

    spice_debug("trace");
    for (i = 0; i < mig_surfaces->num_surfaces; i++) {
        uint32_t surface_id = mig_surfaces->surfaces[i].id;

        if (!restore_surface(dcc, surface_id))
            return FALSE;
    }
    return TRUE;
}

static bool restore_surfaces_lossy(DisplayChannelClient *dcc,
                                   MigrateDisplaySurfacesAtClientLossy *mig_surfaces)
{
    uint32_t i;

    spice_debug("trace");
    for (i = 0; i < mig_surfaces->num_surfaces; i++) {
        uint32_t surface_id = mig_surfaces->surfaces[i].id;
        SpiceMigrateDataRect *mig_lossy_rect;
        SpiceRect lossy_rect;

        if (!restore_surface(dcc, surface_id))
            return FALSE;

        mig_lossy_rect = &mig_surfaces->surfaces[i].lossy_rect;
        lossy_rect.left = mig_lossy_rect->left;
        lossy_rect.top = mig_lossy_rect->top;
        lossy_rect.right = mig_lossy_rect->right;
        lossy_rect.bottom = mig_lossy_rect->bottom;
        region_init(&dcc->priv->surface_client_lossy_region[surface_id]);
        region_add(&dcc->priv->surface_client_lossy_region[surface_id], &lossy_rect);
    }
    return TRUE;
}

bool DisplayChannelClient::handle_migrate_data(uint32_t size, void *message)
{
    DisplayChannelClient *dcc = this;
    DisplayChannel *display = DCC_TO_DC(dcc);
    int surfaces_restored = FALSE;
    auto header = (SpiceMigrateDataHeader *)message;
    auto migrate_data = (SpiceMigrateDataDisplay *)(header + 1);
    uint8_t *surfaces;
    int i;

    spice_return_val_if_fail(
        size >= (sizeof(*migrate_data) + sizeof(SpiceMigrateDataHeader)), FALSE);
    spice_return_val_if_fail(
         migration_protocol_validate_header(header,
             SPICE_MIGRATE_DATA_DISPLAY_MAGIC, SPICE_MIGRATE_DATA_DISPLAY_VERSION), FALSE);

    /* size is set to -1 in order to keep the cache frozen until the original
     * channel client that froze the cache on the src size receives the migrate
     * data and unfreezes the cache by setting its size > 0 and by triggering
     * pixmap_cache_reset */
    dcc->priv->pixmap_cache = pixmap_cache_get(dcc->get_client(),
                                               migrate_data->pixmap_cache_id, -1);
    spice_return_val_if_fail(dcc->priv->pixmap_cache, FALSE);

    pthread_mutex_lock(&dcc->priv->pixmap_cache->lock);
    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        dcc->priv->pixmap_cache->sync[i] = MAX(dcc->priv->pixmap_cache->sync[i],
                                               migrate_data->pixmap_cache_clients[i]);
    }
    pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);

    if (migrate_data->pixmap_cache_freezer) {
        /* activating the cache. The cache will start to be active after
         * pixmap_cache_reset is called, when handling RED_PIPE_ITEM_TYPE_PIXMAP_RESET */
        dcc->priv->pixmap_cache->size = migrate_data->pixmap_cache_size;
        dcc->pipe_add_type(RED_PIPE_ITEM_TYPE_PIXMAP_RESET);
    }

    if (dcc_handle_migrate_glz_dictionary(dcc, migrate_data)) {
        image_encoders_glz_create(&dcc->priv->encoders, dcc->priv->id);
    } else {
        spice_critical("restoring global lz dictionary failed");
    }

    dcc->is_low_bandwidth = migrate_data->low_bandwidth_setting;

    if (migrate_data->low_bandwidth_setting) {
        dcc->ack_set_client_window(WIDE_CLIENT_ACK_WINDOW);
        if (dcc->priv->jpeg_state == SPICE_WAN_COMPRESSION_AUTO) {
            display->priv->enable_jpeg = TRUE;
        }
        if (dcc->priv->zlib_glz_state == SPICE_WAN_COMPRESSION_AUTO) {
            display->priv->enable_zlib_glz_wrap = TRUE;
        }
    }

    surfaces = (uint8_t *)message + migrate_data->surfaces_at_client_ptr;
    surfaces_restored = display->priv->enable_jpeg ?
        restore_surfaces_lossy(dcc, (MigrateDisplaySurfacesAtClientLossy *)surfaces) :
        restore_surfaces_lossless(dcc, (MigrateDisplaySurfacesAtClientLossless*)surfaces);

    spice_return_val_if_fail(surfaces_restored, FALSE);

    dcc->pipe_add_type(RED_PIPE_ITEM_TYPE_INVAL_PALETTE_CACHE);
    /* enable sending messages */
    dcc->ack_zero_messages_window();
    return TRUE;
}

VideoStreamAgent* dcc_get_video_stream_agent(DisplayChannelClient *dcc, int stream_id)
{
    return &dcc->priv->stream_agents[stream_id];
}

ImageEncoders* dcc_get_encoders(DisplayChannelClient *dcc)
{
    return &dcc->priv->encoders;
}

spice_wan_compression_t dcc_get_jpeg_state(DisplayChannelClient *dcc)
{
    return dcc->priv->jpeg_state;
}

spice_wan_compression_t dcc_get_zlib_glz_state(DisplayChannelClient *dcc)
{
    return dcc->priv->zlib_glz_state;
}

uint32_t dcc_get_max_stream_latency(DisplayChannelClient *dcc)
{
    return dcc->priv->streams_max_latency;
}

void dcc_set_max_stream_latency(DisplayChannelClient *dcc, uint32_t latency)
{
    dcc->priv->streams_max_latency = latency;
}

uint64_t dcc_get_max_stream_bit_rate(DisplayChannelClient *dcc)
{
    return dcc->priv->streams_max_bit_rate;
}

void dcc_set_max_stream_bit_rate(DisplayChannelClient *dcc, uint64_t rate)
{
    dcc->priv->streams_max_bit_rate = rate;
}

bool DisplayChannelClient::config_socket()
{
    RedClient *client = get_client();
    MainChannelClient *mcc = client->get_main();

    is_low_bandwidth = mcc->is_low_bandwidth();

    return CommonGraphicsChannelClient::config_socket();
}

void DisplayChannelClient::on_disconnect()
{
    DisplayChannel *display;

    spice_debug("trace");

    display = DCC_TO_DC(this);

    dcc_stop(this); // TODO: start/stop -> connect/disconnect?
    display_channel_compress_stats_print(display);

    // this was the last channel client
    spice_debug("#draw=%d, #glz_draw=%d",
                display->priv->drawable_count,
                display->priv->encoder_shared_data.glz_drawable_count);
}

gboolean dcc_is_low_bandwidth(DisplayChannelClient *dcc)
{
    return dcc->is_low_bandwidth;
}
