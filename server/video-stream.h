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

#ifndef VIDEO_STREAM_H_
#define VIDEO_STREAM_H_

#include <glib.h>
#include <common/region.h>

#include "utils.h"
#include "video-encoder.h"
#include "red-channel.h"
#include "dcc.h"

#include "push-visibility.h"

#define RED_STREAM_DETECTION_MAX_DELTA (NSEC_PER_SEC / 5)
#define RED_STREAM_CONTINUOUS_MAX_DELTA NSEC_PER_SEC
#define RED_STREAM_TIMEOUT NSEC_PER_SEC
#define RED_STREAM_FRAMES_START_CONDITION 20
#define RED_STREAM_GRADUAL_FRAMES_START_CONDITION 0.2
#define RED_STREAM_FRAMES_RESET_CONDITION 100
#define RED_STREAM_MIN_SIZE (96 * 96)
#define RED_STREAM_INPUT_FPS_TIMEOUT (NSEC_PER_SEC * 5)
#define RED_STREAM_CHANNEL_CAPACITY 0.8
/* the client's stream report frequency is the minimum of the 2 values below */
#define RED_STREAM_CLIENT_REPORT_WINDOW 5 // #frames
#define RED_STREAM_CLIENT_REPORT_TIMEOUT MSEC_PER_SEC
#define RED_STREAM_DEFAULT_HIGH_START_BIT_RATE (10 * 1024 * 1024) // 10Mbps
#define RED_STREAM_DEFAULT_LOW_START_BIT_RATE (2.5 * 1024 * 1024) // 2.5Mbps
#define MAX_FPS 30

struct VideoStream;

#ifdef STREAM_STATS
struct StreamStats {
    uint64_t num_drops_pipe;
    uint64_t num_drops_fps;
    uint64_t num_frames_sent;
    uint64_t num_input_frames;
    uint64_t size_sent;

    uint64_t start;
    uint64_t end;
};
#endif

struct VideoStreamAgent {
    QRegion vis_region; /* the part of the surface area that is currently occupied by video
                           fragments */
    QRegion clip;       /* the current video clipping. It can be different from vis_region:
                           for example, let c1 be the clip area at time t1, and c2
                           be the clip area at time t2, where t1 < t2. If c1 contains c2, and
                           at least part of c1/c2, hasn't been covered by a non-video images,
                           vis_region will contain c2 and also the part of c1/c2 that still
                           displays fragments of the video */

    VideoStream *stream;
    VideoEncoder *video_encoder;
    DisplayChannelClient *dcc;

    uint32_t report_id;
    uint32_t client_required_latency;
#ifdef STREAM_STATS
    StreamStats stats;
#endif
};

struct VideoStreamClipItem: public RedPipeItem {
    VideoStreamClipItem(VideoStreamAgent *agent);
    ~VideoStreamClipItem();

    VideoStreamAgent *stream_agent;
    int clip_type;
    red::glib_unique_ptr<SpiceClipRects> rects;
};

struct StreamCreateDestroyItem: public RedPipeItem {
    StreamCreateDestroyItem(VideoStreamAgent *agent, int type);
    ~StreamCreateDestroyItem();
    VideoStreamAgent *agent;
};

struct ItemTrace {
    red_time_t time;
    red_time_t first_frame_time;
    int frames_count;
    int gradual_frames_count;
    int last_gradual_frame;
    int width;
    int height;
    SpiceRect dest_area;
};

struct VideoStream {
    uint8_t refs;
    Drawable *current;
    red_time_t last_time;
    int width;
    int height;
    SpiceRect dest_area;
    int top_down;
    VideoStream *next;
    RingItem link;

    uint32_t num_input_frames;
    uint64_t input_fps_start_time;
    uint32_t input_fps;
};

void display_channel_init_video_streams(DisplayChannel *display);
void video_stream_stop(DisplayChannel *display, VideoStream *stream);
void video_stream_trace_update(DisplayChannel *display, Drawable *drawable);
void video_stream_maintenance(DisplayChannel *display, Drawable *candidate,
                              Drawable *prev);
void video_stream_timeout(DisplayChannel *display);
void video_stream_detach_and_stop(DisplayChannel *display);
void video_stream_trace_add_drawable(DisplayChannel *display, Drawable *item);
void video_stream_detach_behind(DisplayChannel *display, QRegion *region,
                                Drawable *drawable);
GArray *video_stream_parse_preferred_codecs(SpiceMsgcDisplayPreferredVideoCodecType *msg);

void video_stream_agent_stop(VideoStreamAgent *agent);

void video_stream_detach_drawable(VideoStream *stream);

#include "pop-visibility.h"

#endif /* VIDEO_STREAM_H_ */
