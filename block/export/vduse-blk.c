/*
 * Export QEMU block device via VDUSE
 *
 * Copyright (C) 2022 Bytedance Inc. and/or its affiliates. All rights reserved.
 *
 * Author:
 *   Xie Yongji <xieyongji@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include <sys/eventfd.h>

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/export.h"
#include "qemu/error-report.h"
#include "util/block-helpers.h"
#include "subprojects/libvduse/libvduse.h"
#include "virtio-blk-handler.h"

#include "standard-headers/linux/virtio_blk.h"

#define VDUSE_DEFAULT_NUM_QUEUE 1
#define VDUSE_DEFAULT_QUEUE_SIZE 256

typedef struct VduseBlkExport {
    BlockExport export;
    VirtioBlkHandler handler;
    VduseDev *dev;
    uint16_t num_queues;
    char *recon_file;
    unsigned int inflight;
    int fd;
} VduseBlkExport;

typedef struct VduseBlkReq {
    VduseVirtqElement elem;
    VduseVirtq *vq;
} VduseBlkReq;

static void vduse_blk_inflight_inc(VduseBlkExport *vblk_exp)
{
    vblk_exp->inflight++;
}

static void vduse_blk_inflight_dec(VduseBlkExport *vblk_exp)
{
    if (--vblk_exp->inflight == 0) {
        aio_wait_kick();
    }
}

static void vduse_blk_req_complete(VduseBlkReq *req, size_t in_len)
{
    vduse_queue_push(req->vq, &req->elem, in_len);
    vduse_queue_notify(req->vq);

    free(req);
}

struct virtio_blk_inhdr {
    unsigned char status;
};

static int vduse_blk_process_req(int fd, VduseDesc *in_iov, VduseDesc *out_iov,
                                  unsigned int in_num, unsigned int out_num)
{
    struct virtio_blk_inhdr in;
    struct virtio_blk_outhdr out;
    uint32_t type;
    ssize_t ret;
    int in_len, out_len;

    if (out_num < 1 || in_num < 1) {
        error_report("virtio-blk request missing headers");
        return -EINVAL;
    }

    if (unlikely(vduse_write_to_buf(out_iov, out_num, 0, &out,
                            sizeof(out)) != sizeof(out))) {
        error_report("virtio-blk request outhdr too short");
        return -EINVAL;
    }

    if (in_iov[in_num - 1].iov.iov_len < sizeof(struct virtio_blk_inhdr)) {
        error_report("virtio-blk request inhdr too short");
        return -EINVAL;
    }

    /* We always touch the last byte, so just see how big in_iov is. */
    in_len = vduse_iov_size(in_iov, in_num);
    out_len = vduse_iov_size(out_iov, out_num);

    type = le32_to_cpu(out.type);
    switch (type & ~VIRTIO_BLK_T_BARRIER) {
    case VIRTIO_BLK_T_IN:
    case VIRTIO_BLK_T_OUT: {
        int64_t offset;
        bool is_write = type & VIRTIO_BLK_T_OUT;
        int64_t sector_num = le64_to_cpu(out.sector);

        offset = sector_num << VIRTIO_BLK_SECTOR_BITS;

        if (is_write) {
            ret = vduse_splice_to_fd(out_iov, out_num, sizeof(out), fd, offset, out_len - sizeof(out));
            if (ret != out_len - sizeof(out)) {
	        error_report("invalid write size: %ld vs. %d", ret, out_len);
            }
            assert(ret == out_len - sizeof(out));
        } else {
            ret = vduse_splice_from_fd(in_iov, in_num, 0, fd, offset,
                                 in_len - sizeof(in));
            if (ret != in_len - sizeof(in)) {
	        error_report("invalid read size: %ld vs. %ld", ret, in_len - sizeof(in));
            }
            assert(ret == in_len - sizeof(in));
        }
        in.status = VIRTIO_BLK_S_OK;
        ret = vduse_read_from_buf(in_iov, in_num,
                                  in_len - sizeof(in),
                                  &in, sizeof(in));
        assert(ret == sizeof(in));
        break;
    }
    default:
        in.status = VIRTIO_BLK_S_UNSUPP;
        ret = vduse_read_from_buf(in_iov, in_num,
                                  in_len - sizeof(in),
                                  &in, sizeof(in));
        assert(ret == sizeof(in));
        break;
    }

    return in_len;
}


static void vduse_blk_virtio_process_req(VduseBlkReq *req)
{
    VduseVirtq *vq = req->vq;
    VduseDev *dev = vduse_queue_get_dev(vq);
    VduseBlkExport *vblk_exp = vduse_dev_get_priv(dev);
    VduseVirtqElement *elem = &req->elem;
    VduseDesc *in_iov = elem->in_sg;
    VduseDesc *out_iov = elem->out_sg;
    unsigned in_num = elem->in_num;
    unsigned out_num = elem->out_num;
    int in_len;

    in_len = vduse_blk_process_req(vblk_exp->fd, in_iov,
                                    out_iov, in_num, out_num);
    if (in_len < 0) {
        free(req);
        return;
    }

    vduse_blk_req_complete(req, in_len);
    vduse_blk_inflight_dec(vblk_exp);
}

static void vduse_blk_vq_handler(VduseDev *dev, VduseVirtq *vq)
{
    VduseBlkExport *vblk_exp = vduse_dev_get_priv(dev);

    while (1) {
        VduseBlkReq *req;

        req = vduse_queue_pop(vq, sizeof(VduseBlkReq));
        if (!req) {
            break;
        }
        req->vq = vq;

        vduse_blk_inflight_inc(vblk_exp);

        vduse_blk_virtio_process_req(req);
    }
}

static void on_vduse_vq_kick(void *opaque)
{
    VduseVirtq *vq = opaque;
    VduseDev *dev = vduse_queue_get_dev(vq);
    int fd = vduse_queue_get_fd(vq);
    eventfd_t kick_data;

    if (eventfd_read(fd, &kick_data) == -1) {
        error_report("failed to read data from eventfd");
        return;
    }

    vduse_blk_vq_handler(dev, vq);
}

static void vduse_blk_enable_queue(VduseDev *dev, VduseVirtq *vq)
{
    VduseBlkExport *vblk_exp = vduse_dev_get_priv(dev);

    aio_set_fd_handler(vblk_exp->export.ctx, vduse_queue_get_fd(vq),
                       true, on_vduse_vq_kick, NULL, NULL, NULL, vq);
    /* Make sure we don't miss any kick afer reconnecting */
    eventfd_write(vduse_queue_get_fd(vq), 1);
}

static void vduse_blk_disable_queue(VduseDev *dev, VduseVirtq *vq)
{
    VduseBlkExport *vblk_exp = vduse_dev_get_priv(dev);

    aio_set_fd_handler(vblk_exp->export.ctx, vduse_queue_get_fd(vq),
                       true, NULL, NULL, NULL, NULL, NULL);
}

static const VduseOps vduse_blk_ops = {
    .enable_queue = vduse_blk_enable_queue,
    .disable_queue = vduse_blk_disable_queue,
};

static void on_vduse_dev_kick(void *opaque)
{
    VduseDev *dev = opaque;

    vduse_dev_handler(dev);
}

static void vduse_blk_attach_ctx(VduseBlkExport *vblk_exp, AioContext *ctx)
{
    int i;

    aio_set_fd_handler(vblk_exp->export.ctx, vduse_dev_get_fd(vblk_exp->dev),
                       true, on_vduse_dev_kick, NULL, NULL, NULL,
                       vblk_exp->dev);

    for (i = 0; i < vblk_exp->num_queues; i++) {
        VduseVirtq *vq = vduse_dev_get_queue(vblk_exp->dev, i);
        int fd = vduse_queue_get_fd(vq);

        if (fd < 0) {
            continue;
        }
        aio_set_fd_handler(vblk_exp->export.ctx, fd, true,
                           on_vduse_vq_kick, NULL, NULL, NULL, vq);
    }
}

static void vduse_blk_detach_ctx(VduseBlkExport *vblk_exp)
{
    int i;

    for (i = 0; i < vblk_exp->num_queues; i++) {
        VduseVirtq *vq = vduse_dev_get_queue(vblk_exp->dev, i);
        int fd = vduse_queue_get_fd(vq);

        if (fd < 0) {
            continue;
        }
        aio_set_fd_handler(vblk_exp->export.ctx, fd,
                           true, NULL, NULL, NULL, NULL, NULL);
    }
    aio_set_fd_handler(vblk_exp->export.ctx, vduse_dev_get_fd(vblk_exp->dev),
                       true, NULL, NULL, NULL, NULL, NULL);

    AIO_WAIT_WHILE(vblk_exp->export.ctx, vblk_exp->inflight > 0);
}


static void blk_aio_attached(AioContext *ctx, void *opaque)
{
    VduseBlkExport *vblk_exp = opaque;

    vblk_exp->export.ctx = ctx;
    vduse_blk_attach_ctx(vblk_exp, ctx);
}

static void blk_aio_detach(void *opaque)
{
    VduseBlkExport *vblk_exp = opaque;

    vduse_blk_detach_ctx(vblk_exp);
    vblk_exp->export.ctx = NULL;
}

static void vduse_blk_resize(void *opaque)
{
    BlockExport *exp = opaque;
    VduseBlkExport *vblk_exp = container_of(exp, VduseBlkExport, export);
    struct virtio_blk_config config;

    config.capacity =
            cpu_to_le64(blk_getlength(exp->blk) >> VIRTIO_BLK_SECTOR_BITS);
    vduse_dev_update_config(vblk_exp->dev, sizeof(config.capacity),
                            offsetof(struct virtio_blk_config, capacity),
                            (char *)&config.capacity);
}

static const BlockDevOps vduse_block_ops = {
    .resize_cb = vduse_blk_resize,
};

static ssize_t vduse_blk_size(const char *filename)
{
    struct stat st;

    if (stat(filename, &st) < 0) {
        return -1;
    }

    return st.st_size;
}

static int vduse_blk_exp_create(BlockExport *exp, BlockExportOptions *opts,
                                Error **errp)
{
    VduseBlkExport *vblk_exp = container_of(exp, VduseBlkExport, export);
    BlockExportOptionsVduseBlk *vblk_opts = &opts->u.vduse_blk;
    uint64_t logical_block_size = VIRTIO_BLK_SECTOR_SIZE;
    uint16_t num_queues = VDUSE_DEFAULT_NUM_QUEUE;
    uint16_t queue_size = VDUSE_DEFAULT_QUEUE_SIZE;
    Error *local_err = NULL;
    struct virtio_blk_config config = { 0 };
    uint64_t features;
    int i, ret;

    if (vblk_opts->has_num_queues) {
        num_queues = vblk_opts->num_queues;
        if (num_queues == 0) {
            error_setg(errp, "num-queues must be greater than 0");
            return -EINVAL;
        }
    }

    if (vblk_opts->has_queue_size) {
        queue_size = vblk_opts->queue_size;
        if (queue_size <= 2 || !is_power_of_2(queue_size) ||
            queue_size > VIRTQUEUE_MAX_SIZE) {
            error_setg(errp, "queue-size is invalid");
            return -EINVAL;
        }
    }

    if (vblk_opts->has_logical_block_size) {
        logical_block_size = vblk_opts->logical_block_size;
        check_block_size(exp->id, "logical-block-size", logical_block_size,
                         &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return -EINVAL;
        }
    }

    vblk_exp->fd = open(vblk_opts->test_file, O_RDWR);
    if (vblk_exp->fd < 0) {
        error_setg(errp, "Failed to open test file %s, errno: %d",
                   vblk_opts->test_file, errno);
        return -EINVAL;
    }



    vblk_exp->num_queues = num_queues;
    vblk_exp->handler.blk = exp->blk;
    vblk_exp->handler.serial = g_strdup(vblk_opts->has_serial ?
                                        vblk_opts->serial : "");
    vblk_exp->handler.logical_block_size = logical_block_size;
    vblk_exp->handler.writable = opts->writable;

    config.capacity =
            cpu_to_le64(vduse_blk_size(vblk_opts->test_file) >> VIRTIO_BLK_SECTOR_BITS);
    config.seg_max = cpu_to_le32(queue_size - 2);
    config.min_io_size = cpu_to_le16(1);
    config.opt_io_size = cpu_to_le32(1);
    config.num_queues = cpu_to_le16(num_queues);
    config.blk_size = cpu_to_le32(logical_block_size);
    config.max_discard_sectors = cpu_to_le32(VIRTIO_BLK_MAX_DISCARD_SECTORS);
    config.max_discard_seg = cpu_to_le32(1);
    config.discard_sector_alignment =
        cpu_to_le32(logical_block_size >> VIRTIO_BLK_SECTOR_BITS);
    config.max_write_zeroes_sectors =
        cpu_to_le32(VIRTIO_BLK_MAX_WRITE_ZEROES_SECTORS);
    config.max_write_zeroes_seg = cpu_to_le32(1);

    features = vduse_get_virtio_features() |
               (1ULL << VIRTIO_BLK_F_SEG_MAX) |
               (1ULL << VIRTIO_BLK_F_TOPOLOGY) |
               (1ULL << VIRTIO_BLK_F_BLK_SIZE);
//               (1ULL << VIRTIO_BLK_F_FLUSH) |
//               (1ULL << VIRTIO_BLK_F_DISCARD) |
//               (1ULL << VIRTIO_BLK_F_WRITE_ZEROES);

    if (num_queues > 1) {
        features |= 1ULL << VIRTIO_BLK_F_MQ;
    }
    if (!opts->writable) {
        features |= 1ULL << VIRTIO_BLK_F_RO;
    }

    vblk_exp->dev = vduse_dev_create(vblk_opts->name, VIRTIO_ID_BLOCK, 0,
                                     features, num_queues,
                                     sizeof(struct virtio_blk_config),
                                     (char *)&config, &vduse_blk_ops,
                                     vblk_exp);
    if (!vblk_exp->dev) {
        error_setg(errp, "failed to create vduse device");
        ret = -ENOMEM;
        goto err_dev;
    }

    vblk_exp->recon_file = g_strdup_printf("%s/vduse-blk-%s",
                                           g_get_tmp_dir(), vblk_opts->name);
    if (vduse_set_reconnect_log_file(vblk_exp->dev, vblk_exp->recon_file)) {
        error_setg(errp, "failed to set reconnect log file");
        ret = -EINVAL;
        goto err;
    }

    for (i = 0; i < num_queues; i++) {
        vduse_dev_setup_queue(vblk_exp->dev, i, queue_size);
    }

    aio_set_fd_handler(exp->ctx, vduse_dev_get_fd(vblk_exp->dev), true,
                       on_vduse_dev_kick, NULL, NULL, NULL, vblk_exp->dev);

    blk_add_aio_context_notifier(exp->blk, blk_aio_attached, blk_aio_detach,
                                 vblk_exp);

    blk_set_dev_ops(exp->blk, &vduse_block_ops, exp);

    return 0;
err:
    vduse_dev_destroy(vblk_exp->dev);
    g_free(vblk_exp->recon_file);
err_dev:
    g_free(vblk_exp->handler.serial);
    return ret;
}

static void vduse_blk_exp_delete(BlockExport *exp)
{
    VduseBlkExport *vblk_exp = container_of(exp, VduseBlkExport, export);
    int ret;

    blk_remove_aio_context_notifier(exp->blk, blk_aio_attached, blk_aio_detach,
                                    vblk_exp);
    blk_set_dev_ops(exp->blk, NULL, NULL);
    ret = vduse_dev_destroy(vblk_exp->dev);
    if (ret != -EBUSY) {
        unlink(vblk_exp->recon_file);
    }
    g_free(vblk_exp->recon_file);
    g_free(vblk_exp->handler.serial);
}

static void vduse_blk_exp_request_shutdown(BlockExport *exp)
{
    VduseBlkExport *vblk_exp = container_of(exp, VduseBlkExport, export);

    aio_context_acquire(vblk_exp->export.ctx);
    vduse_blk_detach_ctx(vblk_exp);
    aio_context_acquire(vblk_exp->export.ctx);
}

const BlockExportDriver blk_exp_vduse_blk = {
    .type               = BLOCK_EXPORT_TYPE_VDUSE_BLK,
    .instance_size      = sizeof(VduseBlkExport),
    .create             = vduse_blk_exp_create,
    .delete             = vduse_blk_exp_delete,
    .request_shutdown   = vduse_blk_exp_request_shutdown,
};
