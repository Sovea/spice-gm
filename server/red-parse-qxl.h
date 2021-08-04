/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009,2010 Red Hat, Inc.

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

#ifndef RED_PARSE_QXL_H_
#define RED_PARSE_QXL_H_

#include <spice/qxl_dev.h>

#include "red-common.h"
#include "memslot.h"
#include "utils.hpp"

#include "push-visibility.h"

typedef struct RedDrawable {
    int refs;
    QXLInstance *qxl;
    QXLReleaseInfoExt release_info_ext;
    uint32_t surface_id;
    uint8_t effect;
    uint8_t type;
    uint8_t self_bitmap;
    SpiceRect self_bitmap_area;
    SpiceImage *self_bitmap_image;
    SpiceRect bbox;
    SpiceClip clip;
    uint32_t mm_time;
    int32_t surface_deps[3];
    SpiceRect surfaces_rects[3];
    union {
        SpiceFill fill;
        SpiceOpaque opaque;
        SpiceCopy copy;
        SpiceTransparent transparent;
        SpiceAlphaBlend alpha_blend;
        struct {
            SpicePoint src_pos;
        } copy_bits;
        SpiceBlend blend;
        SpiceRop3 rop3;
        SpiceStroke stroke;
        SpiceText text;
        SpiceBlackness blackness;
        SpiceInvers invers;
        SpiceWhiteness whiteness;
        SpiceComposite composite;
    } u;
} RedDrawable;

struct RedUpdateCmd final: public red::simple_ptr_counted<RedUpdateCmd> {
    ~RedUpdateCmd();
    QXLInstance *qxl;
    QXLReleaseInfoExt release_info_ext;
    SpiceRect area;
    uint32_t update_id;
    uint32_t surface_id;
};

struct RedMessage final: public red::simple_ptr_counted<RedMessage> {
    ~RedMessage();
    QXLInstance *qxl;
    QXLReleaseInfoExt release_info_ext;
    int len;
    uint8_t *data;
};

typedef struct RedSurfaceCreate {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    int32_t stride;
    uint8_t *data;
} RedSurfaceCreate;

struct RedSurfaceCmd final: public red::simple_ptr_counted<RedSurfaceCmd> {
    ~RedSurfaceCmd();
    QXLInstance *qxl;
    QXLReleaseInfoExt release_info_ext;
    uint32_t surface_id;
    uint8_t type;
    uint32_t flags;
    union {
        RedSurfaceCreate surface_create;
    } u;
};

struct RedCursorCmd final: public red::simple_ptr_counted<RedCursorCmd> {
    ~RedCursorCmd();
    QXLInstance *qxl;
    QXLReleaseInfoExt release_info_ext;
    uint8_t type;
    union {
        struct {
            SpicePoint16 position;
            uint8_t visible;
            SpiceCursor shape;
        } set;
        struct {
            uint16_t length;
            uint16_t frequency;
        } trail;
        SpicePoint16 position;
    } u;
};

void red_get_rect_ptr(SpiceRect *red, const QXLRect *qxl);

RedDrawable *red_drawable_new(QXLInstance *qxl, RedMemSlotInfo *slots,
                              int group_id, QXLPHYSICAL addr,
                              uint32_t flags);
RedDrawable *red_drawable_ref(RedDrawable *drawable);
void red_drawable_unref(RedDrawable *red_drawable);

red::shared_ptr<const RedUpdateCmd>
red_update_cmd_new(QXLInstance *qxl, RedMemSlotInfo *slots,
                   int group_id, QXLPHYSICAL addr);

red::shared_ptr<const RedMessage>
red_message_new(QXLInstance *qxl, RedMemSlotInfo *slots,
                int group_id, QXLPHYSICAL addr);

bool red_validate_surface(uint32_t width, uint32_t height,
                          int32_t stride, uint32_t format);

red::shared_ptr<const RedSurfaceCmd>
red_surface_cmd_new(QXLInstance *qxl_instance, RedMemSlotInfo *slots,
                    int group_id, QXLPHYSICAL addr);

red::shared_ptr<const RedCursorCmd>
red_cursor_cmd_new(QXLInstance *qxl, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr);

#include "pop-visibility.h"

#endif /* RED_PARSE_QXL_H_ */
