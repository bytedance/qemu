/*
 * Vhost user rdma PCI Bindings
 *
 * Copyright(C) 2021 Bytedance Inc. All rights reserved.
 *
 * Authors:
 *  Junji Wei <weijunji@bytedance.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "qemu/osdep.h"

#include "standard-headers/linux/virtio_net.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/vhost-user-rdma.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "virtio-pci.h"
#include "qom/object.h"

typedef struct VHostUserRdmaPCI VHostUserRdmaPCI;

#define TYPE_VHOST_USER_RDMA_PCI "vhost-user-rdma-pci-base"
DECLARE_INSTANCE_CHECKER(VHostUserRdmaPCI, VHOST_USER_RDMA_PCI,
                         TYPE_VHOST_USER_RDMA_PCI)

struct VHostUserRdmaPCI {
    VirtIOPCIProxy parent_obj;
    VHostUserRdma vdev;
};

static Property vhost_user_rdma_pci_properties[] = {
    DEFINE_PROP_UINT32("class", VirtIOPCIProxy, class_code, 0),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors,
                       DEV_NVECTORS_UNSPECIFIED),
    DEFINE_PROP_END_OF_LIST(),
};

static void vhost_user_rdma_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VHostUserRdmaPCI *dev = VHOST_USER_RDMA_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);

    if (vpci_dev->nvectors == DEV_NVECTORS_UNSPECIFIED) {
        vpci_dev->nvectors = dev->vdev.num_queues + 1;
    }

    // vhost-user not support pci legacy
    virtio_pci_force_virtio_1(vpci_dev);

    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void vhost_user_rdma_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    device_class_set_props(dc, vhost_user_rdma_pci_properties);
    k->realize = vhost_user_rdma_pci_realize;
    pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev_k->device_id = PCI_DEVICE_ID_VIRTIO_NET;
    pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
    pcidev_k->class_id = PCI_CLASS_NETWORK_ETHERNET;
}

static void vhost_user_rdma_pci_instance_init(Object *obj)
{
    VHostUserRdmaPCI *dev = VHOST_USER_RDMA_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VHOST_USER_RDMA);

    object_property_add_alias(obj, "bootindex", OBJECT(&dev->vdev),
                              "bootindex");
}

static const VirtioPCIDeviceTypeInfo vhost_user_rdma_pci_info = {
    .base_name               = TYPE_VHOST_USER_RDMA_PCI,
    .generic_name            = "vhost-user-rdma-pci",
    .transitional_name       = "vhost-user-rdma-pci-transitional",
    .non_transitional_name   = "vhost-user-rdma-pci-non-transitional",
    .instance_size  = sizeof(VHostUserRdmaPCI),
    .instance_init  = vhost_user_rdma_pci_instance_init,
    .class_init     = vhost_user_rdma_pci_class_init,
};

static void vhost_user_rdma_pci_register(void)
{
    virtio_pci_types_register(&vhost_user_rdma_pci_info);
}

type_init(vhost_user_rdma_pci_register)
