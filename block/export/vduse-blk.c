/*
 * VDUSE Block Device Export
 *
 * Copyright (C) 2020 Bytedance Inc. and/or its affiliates. All rights reserved.
 *
 * Author:
 *   Xie Yongji <xieyongji@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "sysemu/block-backend.h"
#include "block/export.h"
#include "qemu/error-report.h"
#include "libvduse.h"

#include "standard-headers/linux/virtio_ring.h"
#include "standard-headers/linux/virtio_blk.h"

typedef struct VduseBlkExport {
    BlockExport export;
    VduseDev dev;
    struct virtio_blk_config config;
    uint64_t features;
    char *name;
} VduseBlkExport;

struct virtio_blk_inhdr {
    unsigned char status;
};

typedef struct VduseBlkReq {
    VduseVirtqElement elem;
    int64_t sector_num;
    struct virtio_blk_inhdr *in;
    struct virtio_blk_outhdr *out;
    size_t in_len;
    QEMUIOVector qiov;
    VduseVirtq *vq;
} VduseBlkReq;

static void vduse_blk_req_complete(VduseBlkReq *req, unsigned char status)
{
    req->in->status = status;
    vduse_queue_push(req->vq, &req->elem, req->in_len);
    vduse_queue_notify(req->vq);

    g_free(req);
}

static void vduse_blk_rw_complete(void *opaque, int ret)
{
    VduseBlkReq *req = opaque;
    unsigned char status = (ret == 0) ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;

    vduse_blk_req_complete(req, status);
}

static int vduse_blk_vq_process(VduseBlkExport *exp, VduseVirtq *vq)
{
    VduseBlkReq *req;
    uint32_t type;
    unsigned in_num;
    unsigned out_num;
    BlockBackend *blk = exp->export.blk;

    req = vduse_queue_pop(vq, sizeof(VduseBlkReq));
    if (!req) {
        return -1;
    }
    req->vq = vq;

    if (req->elem.out_num < 1 || req->elem.in_num < 1) {
        error_report("virtio-blk request missing headers");
        goto err;
    }

    in_num = req->elem.in_num;
    out_num = req->elem.out_num;

    if (req->elem.out_sg[0].iov_len < sizeof(struct virtio_blk_outhdr)) {
        error_report("Invalid outhdr size");
        goto err;
    }
    req->out = (struct virtio_blk_outhdr *)req->elem.out_sg[0].iov_base;
    out_num--;

    if (req->elem.in_sg[in_num - 1].iov_len < sizeof(struct virtio_blk_inhdr)) {
        error_report("Invalid inhdr size");
        goto err;
    }
    req->in_len = iov_size(req->elem.in_sg, in_num);
    req->in = (struct virtio_blk_inhdr *)req->elem.in_sg[in_num - 1].iov_base;
    in_num--;

    type = le32toh(req->out->type);
    switch (type & ~VIRTIO_BLK_T_BARRIER) {
    case VIRTIO_BLK_T_IN:
    case VIRTIO_BLK_T_OUT: {
        bool is_write = type & VIRTIO_BLK_T_OUT;

        req->sector_num = le64toh(req->out->sector);
        if (is_write) {
            qemu_iovec_init_external(&req->qiov, &req->elem.out_sg[1], out_num);
            blk_aio_pwritev(blk, req->sector_num << BDRV_SECTOR_BITS,
                            &req->qiov, 0, vduse_blk_rw_complete, req);
        } else {
            qemu_iovec_init_external(&req->qiov, &req->elem.in_sg[0], in_num);
            blk_aio_preadv(blk, req->sector_num << BDRV_SECTOR_BITS,
                           &req->qiov, 0, vduse_blk_rw_complete, req);
        }
        break;
    }
    case VIRTIO_BLK_T_FLUSH:
        vduse_blk_req_complete(req, VIRTIO_BLK_S_OK);
        break;
    case VIRTIO_BLK_T_GET_ID: {
        size_t size = MIN(iov_size(&req->elem.in_sg[0], in_num),
                          VIRTIO_BLK_ID_BYTES);

        snprintf(req->elem.in_sg[0].iov_base, size, "vduse-blk:%s", exp->name);
        vduse_blk_req_complete(req, VIRTIO_BLK_S_OK);
        break;
    }
    default:
        vduse_blk_req_complete(req, VIRTIO_BLK_S_UNSUPP);
        break;
    }

    return 0;
err:
    g_free(req);
    return -1;
}

static void vduse_vq_handler(VduseDev *dev, VduseVirtq *vq)
{
    VduseBlkExport *exp = container_of(dev, VduseBlkExport, dev);

    while (1) {
        if (vduse_blk_vq_process(exp, vq)) {
            break;
        }
    }
}

static int vduse_blk_exp_create(BlockExport *exp, BlockExportOptions *opts,
                                Error **errp)
{
    VduseBlkExport *vblk_exp = container_of(exp, VduseBlkExport, export);
    VduseDev *dev = &vblk_exp->dev;
    BlockExportOptionsVduseBlk *vblk_opts = &opts->u.vduse_blk;
    BlockBackend *blk = exp->blk;
    int ret, i;

    vblk_exp->name = g_strdup(vblk_opts->name);
    vblk_exp->config.capacity = blk_getlength(blk) >> BDRV_SECTOR_BITS;
    vblk_exp->config.seg_max = 128 - 2;
    vblk_exp->config.size_max = 65536;
    vblk_exp->config.min_io_size = 1;
    vblk_exp->config.opt_io_size = 1;
    vblk_exp->config.num_queues = vblk_opts->num_queues;
    vblk_exp->config.blk_size = BDRV_SECTOR_SIZE;

    vblk_exp->features = (1ULL << VIRTIO_F_IOMMU_PLATFORM) |
                         (1ULL << VIRTIO_F_VERSION_1) |
                         (1ULL << VIRTIO_RING_F_EVENT_IDX) |
                         (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) |
                         (1ULL << VIRTIO_BLK_F_SIZE_MAX) |
                         (1ULL << VIRTIO_BLK_F_SEG_MAX) |
                         (1ULL << VIRTIO_BLK_F_TOPOLOGY) |
                         (1ULL << VIRTIO_BLK_F_BLK_SIZE);

    if (vblk_opts->num_queues > 1) {
        vblk_exp->features |= (1ULL << VIRTIO_BLK_F_MQ);
    }
    blk_set_allow_aio_context_change(blk, true);

    ret = vduse_dev_init(dev, vblk_opts->name, VIRTIO_ID_BLOCK, 0,
                         vblk_exp->features, vblk_opts->num_queues,
                         sizeof(struct virtio_blk_config),
                         (char *)&vblk_exp->config);
    if (ret) {
        error_setg(errp, "Failed to init vduse device");
        return ret;
    }
    for (i = 0; i < vblk_opts->num_queues; i++) {
        vduse_queue_init(&dev->vqs[i], vblk_opts->queue_size, vduse_vq_handler);
    }

    return 0;
}

static void vduse_blk_exp_delete(BlockExport *exp)
{
    VduseBlkExport *vblk_exp = container_of(exp, VduseBlkExport, export);

    vduse_dev_cleanup(&vblk_exp->dev);
    g_free(vblk_exp->name);
}

static void vduse_blk_exp_request_shutdown(BlockExport *exp)
{
}

const BlockExportDriver blk_exp_vduse_blk = {
    .type               = BLOCK_EXPORT_TYPE_VDUSE_BLK,
    .instance_size      = sizeof(VduseBlkExport),
    .create             = vduse_blk_exp_create,
    .delete             = vduse_blk_exp_delete,
    .request_shutdown   = vduse_blk_exp_request_shutdown,
};
