# MLNX_OFED_4.9_Ubuntu_22.04

This is a migrated version of Mellanox OFED 4.9-7.1.0.0 (https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/) for Ubuntu 22.04 with Linux kernel 5.15.0.

# Quick start

1. Ensure that you are running Ubuntu 22.04 with Linux kernel 5.15.0-xxx (**old or new kernel versions are not compatible** due to intrusive modifications of DKMS code).

```bash
lsb_release -sr # 22.04
uname -r # 5.15.0-xxx-generic
```

2. 

# Tested Systems

Testbed: 

* [Rowan](https://github.com/thustorage/rowan), using shared receive queue + multi-packet receive queue (features not in OFED 5+) for shared log.

* [Deft](https://github.com/thustorage/deft), a tree index on disaggregated memory.

**Not tested:**

* mlnx_nvme functionality.

Feedback is welcome if you have tested other features in your environment.


# Modification to Source Code

See `./mod_src` for details.