/*
 * vhost-vdpa-blk device
 *
 * Copyright (C) 2020 Bytedance Inc. and/or its affiliates. All rights reserved.
 *
 * Author: Xie Yongji <xieyongji@bytedance.com>
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/vhost-vdpa-blk.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"

static const int user_feature_bits[] = {
    VIRTIO_BLK_F_SIZE_MAX,
    VIRTIO_BLK_F_SEG_MAX,
    VIRTIO_BLK_F_GEOMETRY,
    VIRTIO_BLK_F_BLK_SIZE,
    VIRTIO_BLK_F_TOPOLOGY,
    VIRTIO_BLK_F_MQ,
    VIRTIO_BLK_F_RO,
    VIRTIO_BLK_F_FLUSH,
    VIRTIO_BLK_F_CONFIG_WCE,
    VIRTIO_BLK_F_DISCARD,
    VIRTIO_BLK_F_WRITE_ZEROES,
    VIRTIO_F_IOMMU_PLATFORM,
    VIRTIO_F_VERSION_1,
    VIRTIO_RING_F_INDIRECT_DESC,
    VIRTIO_RING_F_EVENT_IDX,
    VIRTIO_F_NOTIFY_ON_EMPTY,
    VHOST_INVALID_FEATURE_BIT
};

static void vhost_vdpa_blk_update_config(VirtIODevice *vdev, uint8_t *config)
{
    VHostVdpaBlk *s = VHOST_VDPA_BLK(vdev);

    memcpy(config, &s->blkcfg, sizeof(struct virtio_blk_config));
}

static void vhost_vdpa_blk_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    VHostVdpaBlk *s = VHOST_VDPA_BLK(vdev);
    struct virtio_blk_config *blkcfg = (struct virtio_blk_config *)config;
    int ret;

    if (blkcfg->wce == s->blkcfg.wce) {
        return;
    }

    ret = vhost_dev_set_config(&s->dev, &blkcfg->wce,
                               offsetof(struct virtio_blk_config, wce),
                               sizeof(blkcfg->wce),
                               VHOST_SET_CONFIG_TYPE_MASTER);
    if (ret) {
        error_report("set device config space failed");
        return;
    }

    s->blkcfg.wce = blkcfg->wce;
}

static int vhost_vdpa_blk_start(VirtIODevice *vdev)
{
    VHostVdpaBlk *s = VHOST_VDPA_BLK(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int i, ret;

    if (!k->set_guest_notifiers) {
        error_report("binding does not support guest notifiers");
        return -ENOSYS;
    }

    ret = vhost_dev_enable_notifiers(&s->dev, vdev);
    if (ret < 0) {
        error_report("Error enabling host notifiers: %d", -ret);
        return ret;
    }

    ret = k->set_guest_notifiers(qbus->parent, s->dev.nvqs, true);
    if (ret < 0) {
        error_report("Error binding guest notifier: %d", -ret);
        goto err_host_notifiers;
    }

    s->dev.acked_features = vdev->guest_features;

    ret = vhost_dev_start(&s->dev, vdev);
    if (ret < 0) {
        error_report("Error starting vhost: %d", -ret);
        goto err_guest_notifiers;
    }
    s->started_vu = true;

    /* guest_notifier_mask/pending not used yet, so just unmask
     * everything here. virtio-pci will do the right thing by
     * enabling/disabling irqfd.
     */
    for (i = 0; i < s->dev.nvqs; i++) {
        vhost_virtqueue_mask(&s->dev, vdev, i, false);
    }

    return ret;

err_guest_notifiers:
    k->set_guest_notifiers(qbus->parent, s->dev.nvqs, false);
err_host_notifiers:
    vhost_dev_disable_notifiers(&s->dev, vdev);
    return ret;
}

static void vhost_vdpa_blk_stop(VirtIODevice *vdev)
{
    VHostVdpaBlk *s = VHOST_VDPA_BLK(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;

    if (!s->started_vu) {
        return;
    }
    s->started_vu = false;

    if (!k->set_guest_notifiers) {
        return;
    }

    vhost_dev_stop(&s->dev, vdev);

    ret = k->set_guest_notifiers(qbus->parent, s->dev.nvqs, false);
    if (ret < 0) {
        error_report("vhost guest notifier cleanup failed: %d", ret);
        return;
    }

    vhost_dev_disable_notifiers(&s->dev, vdev);
}

static void vhost_vdpa_blk_set_status(VirtIODevice *vdev, uint8_t status)
{
    VHostVdpaBlk *s = VHOST_VDPA_BLK(vdev);
    bool should_start = virtio_device_started(vdev, status);
    int ret;

    if (!vdev->vm_running) {
        should_start = false;
    }

    if (s->dev.started == should_start) {
        return;
    }

    if (should_start) {
        ret = vhost_vdpa_blk_start(vdev);
        if (ret < 0) {
            error_report("vhost-vdpa-blk: vhost start failed: %s",
                         strerror(-ret));
        }
    } else {
        vhost_vdpa_blk_stop(vdev);
    }

}

static uint64_t vhost_vdpa_blk_get_features(VirtIODevice *vdev,
                                            uint64_t features,
                                            Error **errp)
{
    VHostVdpaBlk *s = VHOST_VDPA_BLK(vdev);

    /* Turn on pre-defined features */
    virtio_add_feature(&features, VIRTIO_BLK_F_SEG_MAX);
    virtio_add_feature(&features, VIRTIO_BLK_F_GEOMETRY);
    virtio_add_feature(&features, VIRTIO_BLK_F_TOPOLOGY);
    virtio_add_feature(&features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_add_feature(&features, VIRTIO_BLK_F_FLUSH);
    virtio_add_feature(&features, VIRTIO_BLK_F_RO);
    virtio_add_feature(&features, VIRTIO_BLK_F_DISCARD);
    virtio_add_feature(&features, VIRTIO_BLK_F_WRITE_ZEROES);

    if (s->config_wce) {
        virtio_add_feature(&features, VIRTIO_BLK_F_CONFIG_WCE);
    }
    if (s->num_queues > 1) {
        virtio_add_feature(&features, VIRTIO_BLK_F_MQ);
    }

    return vhost_get_features(&s->dev, user_feature_bits, features);
}

static void vhost_vdpa_blk_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    error_report("vhost-vdpa-blk: vhost vdpa kick");
}

static void vhost_vdpa_blk_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostVdpaBlk *s = VHOST_VDPA_BLK(vdev);
    int i, ret;

    if (!s->num_queues || s->num_queues > VIRTIO_QUEUE_MAX) {
        error_setg(errp, "vhost-vdpa-blk: invalid number of IO queues");
        return;
    }

    if (!s->queue_size) {
        error_setg(errp, "vhost-vdpa-blk: queue size must be non-zero");
        return;
    }

    s->vdpa.device_fd = qemu_open_old(s->vdpa_dev, O_RDWR);
    if (s->vdpa.device_fd == -1) {
        error_setg(errp, "vhost-vdpa-blk: open vdpa dev failed: %s", strerror(errno));
        return;
    }
    s->vhost_vqs = g_new0(struct vhost_virtqueue, s->num_queues);
    s->dev.nvqs = s->num_queues;
    s->dev.vqs = s->vhost_vqs;
    s->dev.vq_index = 0;
    s->dev.backend_features = 0;

    ret = vhost_dev_init(&s->dev, &s->vdpa, VHOST_BACKEND_TYPE_VDPA, 0);
    if (ret < 0) {
        error_setg(errp, "vhost-vdpa-blk: vhost initialization failed: %s",
                   strerror(-ret));
        goto vdpa_err;
    }

    ret = vhost_dev_get_config(&s->dev, (uint8_t *)&s->blkcfg,
                               sizeof(struct virtio_blk_config));
    if (ret < 0) {
        error_setg(errp, "vhost-vdpa-blk: get block config failed");
        goto vdpa_err;
    }

    if (s->blkcfg.num_queues != s->num_queues) {
        s->blkcfg.num_queues = s->num_queues;
    }

    virtio_init(vdev, "virtio-blk", VIRTIO_ID_BLOCK,
                sizeof(struct virtio_blk_config));

    s->virtqs = g_new(VirtQueue *, s->num_queues);
    for (i = 0; i < s->num_queues; i++) {
        s->virtqs[i] = virtio_add_queue(vdev, s->queue_size,
                                        vhost_vdpa_blk_handle_output);
    }

    return;

vdpa_err:
    g_free(s->vhost_vqs);
    close(s->vdpa.device_fd);
}

static void vhost_vdpa_blk_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostVdpaBlk *s = VHOST_VDPA_BLK(dev);
    int i;

    virtio_set_status(vdev, 0);
    vhost_dev_cleanup(&s->dev);
    g_free(s->vhost_vqs);
    s->vhost_vqs = NULL;

    for (i = 0; i < s->num_queues; i++) {
        virtio_delete_queue(s->virtqs[i]);
    }
    g_free(s->virtqs);
    virtio_cleanup(vdev);
    close(s->vdpa.device_fd);
}

static void vhost_vdpa_blk_instance_init(Object *obj)
{
    VHostVdpaBlk *s = VHOST_VDPA_BLK(obj);

    device_add_bootindex_property(obj, &s->bootindex, "bootindex",
                                  "/disk@0,0", DEVICE(obj));
}

static Property vhost_vdpa_blk_properties[] = {
    DEFINE_PROP_STRING("vdpa-dev", VHostVdpaBlk, vdpa_dev),
    DEFINE_PROP_UINT16("num-queues", VHostVdpaBlk, num_queues, 1),
    DEFINE_PROP_UINT32("queue-size", VHostVdpaBlk, queue_size, 128),
    DEFINE_PROP_END_OF_LIST(),
};

static void vhost_vdpa_blk_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, vhost_vdpa_blk_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    vdc->realize = vhost_vdpa_blk_device_realize;
    vdc->unrealize = vhost_vdpa_blk_device_unrealize;
    vdc->get_config = vhost_vdpa_blk_update_config;
    vdc->set_config = vhost_vdpa_blk_set_config;
    vdc->get_features = vhost_vdpa_blk_get_features;
    vdc->set_status = vhost_vdpa_blk_set_status;
}

static const TypeInfo vhost_vdpa_blk_info = {
    .name = TYPE_VHOST_VDPA_BLK,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VHostVdpaBlk),
    .instance_init = vhost_vdpa_blk_instance_init,
    .class_init = vhost_vdpa_blk_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&vhost_vdpa_blk_info);
}

type_init(virtio_register_types)
