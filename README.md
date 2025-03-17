# MLNX_OFED_4.9_Ubuntu_22.04

This is a migrated version of Mellanox OFED 4.9-7.1.0.0 (https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/) for Ubuntu 22.04 with Linux kernel 5.15.0.

# Quick start

1. Ensure that you are running Ubuntu 22.04 with Linux kernel 5.15.0-xxx (**old or new kernel versions are not compatible** due to intrusive modifications of DKMS code).

```bash
lsb_release -sr # 22.04
uname -r # 5.15.0-xxx-generic
```

2. Install gcc-9 and g++-9, which are used for compiling.

```bash
apt install gcc-9 g++-9
```

3. Run `./mlnxofedinstall`.

```bash
cd MLNX_OFED_LINUX-4.9-7.1.0.0-ubuntu22.04-x86_64
./mlnxofedinstall
```

# Evaluated Systems

Testbed: Two machines with ConnectX-6 NICs, Ubuntu 22.04 with Linux kernel 5.15.0-134-generic.

* `ib_write_bw`.

* [Rowan](https://github.com/thustorage/rowan), which uses **shared receive queue + multi-packet receive queue** (features not in OFED 5+) for shared log.

* [Deft](https://github.com/thustorage/deft), a tree index on disaggregated memory.

**Not tested:**

* mlnx_nvme functionality.

* OpenMPI, UCX, and other high-level interfaces.

Feedback is welcome if you have tested other features in your environment.

# Modified Source Code

See `./mod_src` for details. It contains the modified OFED source code (in `MOD` folder).

Generally, I only modify code that fail to compile in Linux 5.15 (because of changed kernel interfaces).

* For other modules except for OFED kernel, the modifications are shown in commit `976455b`.

* For OFED kernel, I directly apply the backports and then make modifications, the modifications are in commit `f76378`.

* Misc: UCX fails to compile. But the package in OFED_LINUX could be installed.

# Supporting Other Kernel Versions

## Modify Code (Especially DKMS)

Typically, you should modify only the DKMS modules in the `MOD` folder. If other modules fail to compile, modify their source code by `tar -zxvf` and copy them to `MOD` folder.

* When debugging, you could directly compile by executing `make` (it uses both `makefile` and `Makefile`, CFLAGS are passed in `makefile`) in the source folder. For `ofed_kernel`, refer to `ofed_scripts/pre_build.sh` (`./configure --with-njobs=${nprocs}; make -j`).

* Or, use `dpkg -i` to install the compiled software one-by-one, see `make.log` (path shown in ERROR message) for details.

* When the compile could pass, use `tar -zcvf` to generate `.tgz`, replace the corresponding ones in `SOURCES`.

* Delete the corresponding .deb in `DEBS`, rebuild them using `./install.pl`.

Remember to modify `install.pl` to change the version of dependencies (e.g., `libssl3` vs `libssl1.1`).

## Install

When `install.pl` successes in installing all the packages:

* Replace the DEBS in `MLNX_OFED_LINUX` with corresponding ones in `mod_src/DEBS`

* Change `distro` to your distribution in `MLNX_OFED_LINUX`.

* Install the OFED normally using `./mlnxofedinstall`.