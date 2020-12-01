/*
 * vhost-vdpa-blk device
 *
 * Copyright (C) 2020 Bytedance Inc. and/or its affiliates. All rights reserved.
 *
 * Author: Xie Yongji <xieyongji@bytedance.com>
 *
 */

#ifndef VHOST_VDPA_BLK_H
#define VHOST_VDPA_BLK_H

#include "standard-headers/linux/virtio_blk.h"
#include "hw/block/block.h"
#include "chardev/char-fe.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/vhost-vdpa.h"
#include "qom/object.h"

#define TYPE_VHOST_VDPA_BLK "vhost-vdpa-blk"
OBJECT_DECLARE_SIMPLE_TYPE(VHostVdpaBlk, VHOST_VDPA_BLK)

struct VHostVdpaBlk {
    VirtIODevice parent_obj;
    int32_t bootindex;
    struct virtio_blk_config blkcfg;
    uint16_t num_queues;
    uint32_t queue_size;
    uint32_t config_wce;
    char *vdpa_dev;
    struct vhost_dev dev;
    struct vhost_vdpa vdpa;
    struct vhost_virtqueue *vhost_vqs;
    VirtQueue **virtqs;
    bool started_vu;
};

#endif
