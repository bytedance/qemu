/*
 * vhost-user-rdma host device
 *
 * Copyright (C) 2022 Bytedance Inc. and/or its affiliates. All rights reserved.
 *
 * Authors:
 *  Wei Junji <weijunji@bytedance.com>
 *  Xie Yongji <xieyongji@bytedance.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef VHOST_USER_RDMA_H
#define VHOST_USER_RDMA_H

#include "standard-headers/linux/virtio_net.h"
#include "chardev/char-fe.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/vhost-user.h"
#include "qom/object.h"

#define TYPE_VHOST_USER_RDMA "vhost-user-rdma"
OBJECT_DECLARE_SIMPLE_TYPE(VHostUserRdma, VHOST_USER_RDMA)

struct VHostUserRdma {
    VirtIODevice parent_obj;
    CharBackend chardev;
    int32_t bootindex;
    struct virtio_net_config rdmacfg;
    struct vhost_dev dev;
    VhostUserState vhost_user;
    struct vhost_virtqueue *vhost_vqs;
    VirtQueue **virtqs;

    int num_queues;

    /* vhost_user_rdma_connect/vhost_user_rdma_disconnect */
    bool connected;
    /* vhost_user_rdma_start/vhost_user_rdma_stop */
    bool started_vu;
};

#endif
