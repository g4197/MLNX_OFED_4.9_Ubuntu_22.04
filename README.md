# MLNX_OFED 4.9 for Ubuntu 22.04 (Kernel 5.15)

This repository provides a migrated version of Mellanox OFED 4.9-7.1.0.0 (originally available at https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/) adapted for Ubuntu 22.04 with the Linux 5.15.0 kernel series.

# Quick start

0. **Clone this repository.**

1. Ensure your system is running Ubuntu 22.04 with a Linux kernel from the 5.15.0-xxx series.  **Other kernel versions (older or newer) are incompatible** due to significant modifications to the DKMS (Dynamic Kernel Module Support) code.  Verify with the following commands:

```bash
lsb_release -sr  # Expected output: 22.04
uname -r        # Expected output: 5.15.0-xxx-generic
```

2. Install GCC-9 and G++-9, which are required for compilation:

```bash
apt install gcc-9 g++-9
```

3. Navigate to the driver directory and run the `mlnxofedinstall` script:

```bash
cd MLNX_OFED_LINUX-4.9-7.1.0.0-ubuntu22.04-x86_64
./mlnxofedinstall
```

# Tested Systems and Features

The following configurations and applications have been successfully tested:

* **Testbed:** Two machines equipped with ConnectX-6 NICs, running Ubuntu 22.04 with Linux kernel 5.15.0-134-generic.

* **`ib_write_bw`:**  The InfiniBand write bandwidth test utility.

* **[Rowan](https://github.com/thustorage/rowan):** A shared log implementation utilizing **multi-packet receive queues** (features not present in OFED 5+).

* **[Deft](https://github.com/thustorage/deft):** A tree index designed for disaggregated memory systems.

**Untested Features:**

* `mlnx_nvme` functionality.

* High-level communication libraries and frameworks such as OpenMPI, UCX, etc.

We encourage users to provide feedback if they have tested other features in their environments.

# Source Code Modifications

Detailed information regarding the source code modifications can be found in the `./mod_src` directory. 
This directory contains the modified OFED source code within the `MOD` folder.

The modifications primarily address compilation failures encountered with the Linux 5.15 kernel due to changes in kernel interfaces.

* **Non-Kernel Modules:** Modifications to modules outside of the OFED kernel are detailed in commit `976455b`.

* **OFED Kernel Modules:**  For the OFED kernel, backports were applied first, followed by necessary modifications. These changes are detailed in commit `f76378`.

* **Note:** UCX compilation failed.  However, the pre-built package within `MLNX_OFED_LINUX` can still be installed.

# Adapting to Other Kernel Versions

## Code Modifications (Primarily DKMS)

To support other kernel versions, you will likely need to modify the DKMS modules located in the `MOD` folder.
If other modules fail to compile, extract their source code using `tar -zxvf` and place them in the `MOD` folder for modification.

* When debugging, you could directly compile by executing `make` (it uses both `makefile` and `Makefile`, CFLAGS are passed in `makefile`) in the source folder. For `ofed_kernel`, refer to `ofed_scripts/pre_build.sh` (`./configure --with-njobs=${nprocs}; make -j`).

* Alternatively, use `dpkg -i` to install compiled packages individually. Refer to the `make.log` file (the path is indicated in error messages) for error details.

* Once compilation is successful, use `tar -zcvf` to create `.tgz` archives. Replace the corresponding archives in the `SOURCES` directory.

* Delete the corresponding `.deb` files in `DEBS`, rebuild and reinstall them using `./install.pl`.

Remember to update the dependency versions in `install.pl` as needed (e.g., `libssl3` versus `libssl1.1`).

## Install

After successfully building all packages using `install.pl`:

* Replace the `.deb` files in the `MLNX_OFED_LINUX` directory with the newly built ones located in `mod_src/DEBS`.

* Modify the `distro` variable in the `MLNX_OFED_LINUX` directory to reflect your target distribution.

* Proceed with the standard OFED installation using `./mlnxofedinstall`.