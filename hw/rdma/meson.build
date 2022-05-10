specific_ss.add(when: 'CONFIG_VMW_PVRDMA', if_true: files(
  'rdma.c',
  'rdma_backend.c',
  'rdma_rm.c',
  'rdma_utils.c',
  'vmw/pvrdma_cmd.c',
  'vmw/pvrdma_dev_ring.c',
  'vmw/pvrdma_main.c',
  'vmw/pvrdma_qp_ops.c',
))

specific_ss.add(when: 'CONFIG_VHOST_USER_RDMA', if_true: files(
  'vhost-user-rdma.c',
))
