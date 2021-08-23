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

#ifndef DCC_PRIVATE_H_
#define DCC_PRIVATE_H_

#include "cache-item.h"
#include "dcc.h"
#include "image-encoders.h"
#include "video-stream.h"
#include "red-channel-client.h"

#include "push-visibility.h"

struct DisplayChannelClientPrivate
{
    SPICE_CXX_GLIB_ALLOCATOR

    uint32_t id = 0;
    SpiceImageCompression image_compression;
    spice_wan_compression_t jpeg_state;
    spice_wan_compression_t zlib_glz_state;

    ImageEncoders encoders;

    int expect_init = 0;

    PixmapCache *pixmap_cache = nullptr;
    uint32_t pixmap_cache_generation = 0;
    int pending_pixmaps_sync = 0;

    std::array<RedCacheItem *, PALETTE_CACHE_HASH_SIZE> palette_cache;
    Ring palette_cache_lru = { nullptr, nullptr };
    long palette_cache_available = CLIENT_PALETTE_CACHE_SIZE;

    struct {
        FreeList free_list;
        std::array<uint64_t, MAX_DRAWABLE_PIXMAP_CACHE_ITEMS> pixmap_cache_items;
        int num_pixmap_cache_items;
    } send_data;

    /* Host preferred video-codec order sorted with client preferred */
    GArray *preferred_video_codecs;
    /* Array with SPICE_VIDEO_CODEC_TYPE_ENUM_END elements, with the client
     * preference order (index) as value */
    GArray *client_preferred_video_codecs;

    std::array<bool, NUM_SURFACES> surface_client_created;
    std::array<QRegion, NUM_SURFACES> surface_client_lossy_region;

    std::array<VideoStreamAgent, NUM_STREAMS> stream_agents;
    uint32_t streams_max_latency;
    uint64_t streams_max_bit_rate;
    bool gl_draw_ongoing;
};

#include "pop-visibility.h"

#endif /* DCC_PRIVATE_H_ */
