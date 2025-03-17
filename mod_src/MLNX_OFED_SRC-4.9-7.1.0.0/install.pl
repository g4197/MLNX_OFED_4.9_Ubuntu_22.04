#!/usr/bin/perl
#
# Copyright (c) 2013 Mellanox Technologies. All rights reserved.
#
# This Software is licensed under one of the following licenses:
#
# 1) under the terms of the "Common Public License 1.0" a copy of which is
#    available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/cpl.php.
#
# 2) under the terms of the "The BSD License" a copy of which is
#    available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/bsd-license.php.
#
# 3) under the terms of the "GNU General Public License (GPL) Version 2" a
#    copy of which is available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/gpl-license.php.
#
# Licensee has the right to choose one of the above licenses.
#
# Redistributions of source code must retain the above copyright
# notice and one of the license notices.
#
# Redistributions in binary form must reproduce both the above copyright
# notice, one of the license notices in the documentation
# and/or other materials provided with the distribution.

use strict;
use File::Basename;
use File::Path;
use File::Find;
use File::Copy;
use Cwd;
use Term::ANSIColor qw(:constants);

my $PREREQUISIT = "172";
my $MST_START_FAIL = "173";
my $NO_HARDWARE = "171";
my $SUCCESS = "0";
my $DEVICE_INI_MISSING = "2";
my $ERROR = "1";
my $EINVAL = "22";
my $ENOSPC = "28";
my $NONOFEDRPMS = "174";

$ENV{"LANG"} = "C";

$| = 1;

my $WDIR    = dirname(`readlink -f $0`);
chdir $WDIR;
require("$WDIR/common.pl");
my $CWD     = getcwd;
my $DPKG = "/usr/bin/dpkg";
my $DPKG_QUERY = "/usr/bin/dpkg-query";
my $DPKG_BUILDPACKAGE = "/usr/bin/dpkg-buildpackage";
my $MODINFO = "/sbin/modinfo";
my $DPKG_FLAGS = "--force-confmiss";
my $DPKG_DEB = "/usr/bin/dpkg-deb";
my $BUILD_ENV = '';
my $enable_mlnx_tune = 0;
my $check_linux_deps = 1;

my $ifconf = "/etc/network/interfaces";
my $ib_udev_rules = "/etc/udev/rules.d/90-ib.rules";
my $config_net_given = 0;
my $config_net = "";
my %ifcfg = ();
my $umad_dev_rw = 0;
my $umad_dev_na = 0;
my $config_given = 0;
my $conf_dir = $CWD;
my $config = $conf_dir . '/ofed.conf';
chomp $config;
my $install_option = 'all';
if (-e "$CWD/.def_option" ) {
	$install_option = `cat $CWD/.def_option 2>/dev/null`;
	chomp $install_option;
}
my $force_all = 0;
my $user_space_only = 0;
my $with_vma = 0;
my $with_libdisni = 0;
my $print_available = 0;
my $force = 0;
my %disabled_packages;
my %force_enable_packages;
my %packages_deps = ();
my %modules_deps = ();
my $with_memtrack = 0;
my $with_cx4lx_opt = 0;
my $with_dkms = 1;
my $with_kmod_debug_symbols = 0;
my $force_dkms = 0;
my $build_only = 0;
my $uninstall = 1;
my $use_upstream_libs = 0;
my $with_ovs_dpdk = 0;

my $kernel_elfutils_devel = 'libelf-dev';

# list of scripts to run for each package
my %package_pre_build_script = ();
my %package_post_build_script = ();
my %package_pre_install_script = ();
my %package_post_install_script = ();

$ENV{"DEBIAN_FRONTEND"} = "noninteractive";

my $fca_prefix = '/opt/mellanox/fca';

my $CMD = "$0 " . join(' ', @ARGV);
my $enable_opensm = 0;

my $LOCK_EXCLUSIVE = 2;
my $UNLOCK         = 8;

my $PACKAGE     = 'OFED';
my $TMPDIR = "/tmp";

my $quiet = 0;
my $verbose = 1;
my $verbose2 = 0;
my $verbose3 = 0;
my %selected_for_uninstall;
my @dependant_packages_to_uninstall = ();
my %non_ofed_for_uninstall = ();

my $builddir = "/var/tmp";

my %main_packages = ();
my @selected_packages = ();
my @selected_modules_by_user = ();
my @selected_kernel_modules = ();
my $kernel_configure_options = '';
# list of the packages that will be installed (selected by user)
my @selected_by_user = ();
my @selected_to_install = ();

my $distro = "";
my $arch = `uname -m`;
chomp $arch;
my $kernel = `uname -r`;
chomp $kernel;
my $kernel_sources = "/lib/modules/$kernel/build";
chomp $kernel_sources;
my $kernel_given = 0;
my $kernel_source_given = 0;
my $cross_compiling = 0;
my $check_deps_only = 0;
my $with_mlx5_ipsec = 1;

if ($ENV{"ARCH"} ne "" and $ENV{"ARCH"} ne "$arch") {
	print "Detected cross compiling (local: $arch, target: $ENV{ARCH})\n\n";
	$arch = $ENV{"ARCH"};
	$arch =~ s/arm64/aarch64/g;
	$DPKG_BUILDPACKAGE = "$DPKG_BUILDPACKAGE -a$ENV{ARCH}";
	$cross_compiling = 1;
}

my $is_bf = `lspci -s 00:00.0 2> /dev/null | grep -wq "PCI bridge: Mellanox Technologies" && echo 1 || echo 0`;
chomp $is_bf;

my $with_bluefield = 0;
if  ($is_bf) {
	$with_bluefield = 1;
}

#
# parse options
#
while ( $#ARGV >= 0 ) {
	my $cmd_flag = shift(@ARGV);

	if ( $cmd_flag eq "--all" ) {
		$install_option = 'all';
		$force_all = 1;
	} elsif ( $cmd_flag eq "--bluefield" ) {
		# Do not override other install options to enable bluefield packages as an extension
		$install_option = 'bluefield' if (not $install_option or ($install_option eq 'all' and not $force_all));
		$with_bluefield = 1;
	} elsif ( $cmd_flag eq "--hpc" ) {
		$install_option = 'hpc';
	} elsif ( $cmd_flag eq "--basic" ) {
		$install_option = 'basic';
	} elsif ( $cmd_flag eq "--msm" ) {
		$install_option = 'msm';
		$enable_opensm = 1;
	} elsif ( $cmd_flag eq "--with-vma" and not ($install_option eq 'eth-only')) {
		$with_vma = 1;
	} elsif ( $cmd_flag eq "--vma" ) {
		$install_option = 'vma';
		$with_vma = 1;
	} elsif ( $cmd_flag eq "--vma-eth" ) {
		$install_option = 'vmaeth';
		$with_vma = 1;
	} elsif ( $cmd_flag eq "--vma-vpi" ) {
		$install_option = 'vmavpi';
		$with_vma = 1;
	} elsif ( $cmd_flag eq "--with-libdisni" ) {
		$with_libdisni = 1;
	} elsif ( $cmd_flag eq "--guest" ) {
		$install_option = 'guest';
	} elsif ( $cmd_flag eq "--hypervisor" ) {
		$install_option = 'hypervisor';
	} elsif ( $cmd_flag eq "--kernel-only" ) {
		$install_option = 'kernel-only';
	} elsif ( $cmd_flag eq "--user-space-only" ) {
		$user_space_only = 1;
	} elsif ( $cmd_flag eq "--eth-only" ) {
		$install_option = 'eth-only';
	} elsif ( $cmd_flag eq "--dpdk" ) {
		$install_option = 'dpdk';
	} elsif ( $cmd_flag eq "--ovs-dpdk" ) {
		$with_ovs_dpdk = 1;
	} elsif ( $cmd_flag eq "--upstream-libs" ) {
		$use_upstream_libs = 1;
	} elsif ( $cmd_flag eq "--mlnx-libs" ) {
		$use_upstream_libs = 0;
	} elsif ( $cmd_flag eq "--umad-dev-rw" ) {
		$umad_dev_rw = 1;
	} elsif ( $cmd_flag eq "--umad-dev-na" ) {
		$umad_dev_na = 1;
	} elsif ( $cmd_flag eq "--enable-opensm" ) {
		$enable_opensm = 1;
	} elsif ( $cmd_flag eq "-q" ) {
		$quiet = 1;
		$verbose = 0;
		$verbose2 = 0;
		$verbose3 = 0;
	} elsif ( $cmd_flag eq "-v" ) {
		$verbose = 1;
	} elsif ( $cmd_flag eq "-vv" ) {
		$verbose = 1;
		$verbose2 = 1;
	} elsif ( $cmd_flag eq "-vvv" ) {
		$verbose = 1;
		$verbose2 = 1;
		$verbose3 = 1;
	} elsif ( $cmd_flag eq "--force" ) {
		$force = 1;
	} elsif ( $cmd_flag eq "-n" or $cmd_flag eq "--net" ) {
		$config_net = shift(@ARGV);
		$config_net_given = 1;
	} elsif ( $cmd_flag eq "--with-memtrack" ) {
		$with_memtrack = 1;
	} elsif ( $cmd_flag eq "--with-cx4lx-optimizations" ) {
		$with_cx4lx_opt = 1;
	} elsif ( $cmd_flag eq "--without-cx4lx-optimizations" ) {
		$with_cx4lx_opt = 0;
	} elsif ($cmd_flag eq "--conf-dir") {
		$conf_dir = shift(@ARGV);
		mkpath([$conf_dir]) unless -d "$conf_dir";
		if (not $config_given) {
			$config = $conf_dir . '/ofed.conf';
		}
	} elsif ( $cmd_flag eq "-c" or $cmd_flag eq "--config" ) {
		$config = shift(@ARGV);
		$config_given = 1;
	} elsif ( $cmd_flag eq "-p" or $cmd_flag eq "--print-available" ) {
		$print_available = 1;
	} elsif ( $cmd_flag eq "--builddir" ) {
		$builddir = shift(@ARGV);
		$builddir = clean_path($builddir);
	} elsif ( $cmd_flag eq "--tmpdir" ) {
		$TMPDIR = shift(@ARGV);
		$TMPDIR = clean_path($TMPDIR);
	} elsif ( $cmd_flag eq "--enable-mlnx_tune" ) {
		$enable_mlnx_tune = 1;
	} elsif ( $cmd_flag eq "--without-mlx5-ipsec") {
                $with_mlx5_ipsec = 0;
        } elsif ( $cmd_flag eq "--without-depcheck" ) {
		$check_linux_deps = 0;
	} elsif ( $cmd_flag eq "--check-deps-only" ) {
		$check_deps_only = 1;
	} elsif ( $cmd_flag eq "--without-dkms" ) {
		$with_dkms = 0;
		$force_dkms = 0;
	} elsif ( $cmd_flag eq "--with-debug-symbols" ) {
		$with_kmod_debug_symbols = 1;
	} elsif ( $cmd_flag eq "--without-debug-symbols" ) {
		$with_kmod_debug_symbols = 0;
	} elsif ( $cmd_flag eq "--force-dkms" ) {
		$with_dkms = 1;
		$force_dkms = 1;
	} elsif ( $cmd_flag eq "-k" or $cmd_flag eq "--kernel" ) {
		$kernel = shift(@ARGV);
		$kernel_given = 1;
	} elsif ( $cmd_flag eq "-b" or $cmd_flag eq "--build-only" ) {
		$build_only = 1;
		$uninstall = 0;
	} elsif ( $cmd_flag eq "-s" or $cmd_flag eq "--kernel-sources" ) {
		$kernel_sources = shift(@ARGV);
		$kernel_source_given = 1;
	} elsif ( $cmd_flag =~ /--without|--disable/ ) {
		my $pckg = $cmd_flag;
		$pckg =~ s/--without-|--disable-//;
		$disabled_packages{$pckg} = 1;
	} elsif ( $cmd_flag =~ /--with-|--enable-/ ) {
		my $pckg = $cmd_flag;
		$pckg =~ s/--with-|--enable-//;
		$force_enable_packages{$pckg} = 1;
	} elsif ( $cmd_flag eq "--distro" ) {
		$distro = shift(@ARGV);
	} elsif ( $cmd_flag =~ /--pre-build-/) {
		my $pckg = $cmd_flag;
		$pckg =~ s/--pre-build-//;
		my $script = shift(@ARGV);
		$package_pre_build_script{$pckg} = $script;
	} elsif ( $cmd_flag =~ /--post-build-/) {
		my $pckg = $cmd_flag;
		$pckg =~ s/--post-build-//;
		my $script = shift(@ARGV);
		$package_post_build_script{$pckg} = $script;
	} elsif ( $cmd_flag =~ /--pre-install-/) {
		my $pckg = $cmd_flag;
		$pckg =~ s/--pre-install-//;
		my $script = shift(@ARGV);
		$package_pre_install_script{$pckg} = $script;
	} elsif ( $cmd_flag =~ /--post-install-/) {
		my $pckg = $cmd_flag;
		$pckg =~ s/--post-install-//;
		my $script = shift(@ARGV);
		$package_post_install_script{$pckg} = $script;
	} elsif ( $cmd_flag eq "--package-install-options" ) {
		my $install_opt = shift(@ARGV);
		$install_opt =~ s/,/ /g;
		$DPKG_FLAGS .= " $install_opt";
	} elsif ( $cmd_flag eq "--help" or $cmd_flag eq "-h" ) {
		usage();
		exit 0;
	} else {
		print RED "\nUnsupported installation option: '$cmd_flag'", RESET "\n";
		print "To see list of supported options, run: $0 --help\n";
		exit $EINVAL;
	}
}

if ($build_only and not (($install_option eq 'kernel-only') or ($install_option eq 'eth-only'))) {
    print RED "\nError: The '--build-only' option is supported only when '--kernel-only' option is given!", RESET "\n";
    print "To see list of supported options, run: $0 --help\n";
    exit 1;
}

if ((not $build_only) and (not $print_available)) {
    check_root_user();
}

if ($with_ovs_dpdk and not $use_upstream_libs) {
	print RED "\nError: The '--ovs-dpdk' option is supported only when '--upstream-libs' option is given!", RESET "\n";
	exit 1;

}

# packages to remove
my @remove_debs = qw(ar_mgr ar-mgr cc_mgr cc-mgr compat-dapl1 compat-dapl-dev dapl1 dapl1-utils dapl2-utils dapl-dev dump_pr dump-pr ibacm ibacm-dev ibsim ibsim-utils ibutils ibutils2 ibverbs-utils infiniband-diags libdapl2 libdapl-dev libibcm libibcm1 libibcm-dev libibdm1 libibdm-dev libibmad libibmad1 libibmad5 libibmad-dev libibmad-devel libibmad-static libibnetdisc5 libibnetdisc-dev libibnetdisc5-dbg libibumad libibumad1 libibumad-dev libibumad-devel libibumad-static libibverbs libibverbs1 libibverbs1-dbg libibverbs-dev libipathverbs1 libipathverbs1-dbg libipathverbs-dev libmlx4 libmlx4-1 libmlx4-1-dbg libmlx4-dev libmlx5 libmlx5-1 libmlx5-1-dbg libmlx5-dev librxe-1 librxe-dev librxe-1-dbg libopensm libopensm2 libopensm2-dev libopensm-dev libopensm-devel librdmacm librdmacm1 librdmacm1-dbg librdmacm-dev libsdp1 libsdp-dev libumad2sim0 mlnx-ofed-kernel-dkms mlnx-ofed-kernel-modules mlnx-ofed-kernel-utils ofed-docs ofed-scripts opensm opensm-libs opensm-doc perftest rdmacm-utils rds-tools sdpnetstat srptools mft kernel-mft-dkms mft-compat mft-devel mft-devmon mft-devmondb mft-int mft-oem mft-tests mstflint mxm ucx fca openmpi openshmem mpitests knem knem-dkms ummunotify ummunotify-dkms libvma libvma-utils libvma-dev libvma-dbg sockperf srptools iser-dkms isert-dkms srp-dkms libmthca-dev libmthca1 libmthca1-dbg ibdump mlnx-ethtool mlnx-iproute2 mlnx-fw-updater knem-modules iser-modules isert-modules srp-modules ummunotify-modules kernel-mft-modules libosmvendor libosmvendor4 libosmcomp libosmcomp3 mlnx-en mlnx-en-utils mlnx-en-dkms mlnx-en-modules mlnx-sdp-dkms mlnx-sdp-modules mlnx-rds-dkms mlnx-rds-modules mlnx-nfsrdma-dkms mlnx-nfsrdma-modules mlnx-nvme-dkms mlnx-nvme-modules mlnx-rdma-rxe-dkms mlnx-rdma-rxe-modules ibverbs-providers libibumad3 libibumad3-dbg rdma-core rshim-modules rshim-dkms python3-pyverbs mlnx-tools nvme-snap);

my @immune_debs_list = map {"qemu-system-$_"} qw(arm misc mips ppc s390x sparc x86 x86-microvm x86-xen);
my %immune_debs = map { $_ => 1 } @immune_debs_list;

# required packages (will be always installed)
my @required_debs = qw(autotools-dev autoconf automake m4 debhelper chrpath swig graphviz dpatch libltdl-dev build-essential);

if ($kernel_given and not $kernel_source_given) {
    if (-d "/lib/modules/$kernel/build") {
        $kernel_sources = "/lib/modules/$kernel/build";
    }
    else {
        print RED "Provide path to the kernel sources for $kernel kernel.", RESET "\n";
        exit 1;
    }
}

#
# logging
#
my $ofedlogs = "$TMPDIR/$PACKAGE.$$.logs";
mkpath([$ofedlogs]);
my $glog = "$ofedlogs/general.log";
rmtree $glog;
open(GLOG, ">$glog") or die "Can't open $glog: $!\n";
close(GLOG);

sub print_and_log
{
	my $msg = shift @_;
	my $verb = shift @_;

	open(GLOG, ">>$glog") or die "Can't open $glog: $!\n";
	print GLOG "$msg";
	close(GLOG);

	if ($verb) {
		print "$msg";
	}
}

sub print_and_log_colored
{
	my $msg = shift @_;
	my $verb = shift @_;
	my $color = shift @_;

	open(GLOG, ">>$glog") or die "Can't open $glog: $!\n";
	print GLOG "$msg\n";
	close(GLOG);

	if ($verb) {
		if ($color eq "RED") {
			print RED "$msg", RESET "\n";
		} elsif ($color eq "YELLOW") {
			print YELLOW "$msg", RESET "\n";
		} elsif ($color eq "GREEN") {
			print GREEN "$msg", RESET "\n";
		} else {
			print "$msg\n";
		}
	}
}

print_and_log("Install command: $CMD\n", 0);

# disable DKMS if given kernel was not installed from deb package
if (not $force_dkms and $with_dkms and -d "$kernel_sources/scripts") {
	my $src_path = `readlink -f $kernel_sources/scripts 2>/dev/null`;
	chomp $src_path;
	if ($src_path eq "") {
		$src_path = "$kernel_sources/scripts";
	}
	system("$DPKG -S $src_path >/dev/null 2>&1");
	my $res = $? >> 8;
	my $sig = $? & 127;
	if ($sig or $res) {
		print_and_log("DKMS is not supported for kernels which were not installed as DEB.\n", $verbose2);
		$with_dkms = 0;
	}
}

#
# set kernel packages names
#
my $mlnX_ofed_kernel = "mlnx-ofed-kernel-dkms";
my $kernel_mft = "kernel-mft-dkms";
my $knem = "knem-dkms";
my $iser = "iser-dkms";
my $isert = "isert-dkms";
my $srp = "srp-dkms";
my $ummunotify = "ummunotify-dkms";
my $mlnx_en = "mlnx-en-dkms";
my $mlnx_sdp = "mlnx-sdp-dkms";
my $mlnx_rds = "mlnx-rds-dkms";
my $mlnx_nfsrdma = "mlnx-nfsrdma-dkms";
my $mlnx_nvme = "mlnx-nvme-dkms";
my $mlnx_rdma_rxe = "mlnx-rdma-rxe-dkms";
my $rshim = "rshim-dkms";

if (not $with_dkms) {
	$mlnX_ofed_kernel = "mlnx-ofed-kernel-modules";
	$kernel_mft = "kernel-mft-modules";
	$knem = "knem-modules";
	$iser = "iser-modules";
	$isert = "isert-modules";
	$srp = "srp-modules";
	$ummunotify = "ummunotify-modules";
	$mlnx_en = "mlnx-en-modules";
	$mlnx_sdp = "mlnx-sdp-modules";
	$mlnx_rds = "mlnx-rds-modules";
	$mlnx_nfsrdma = "mlnx-nfsrdma-modules";
	$mlnx_nvme = "mlnx-nvme-modules";
	$mlnx_rdma_rxe = "mlnx-rdma-rxe-modules";
	$rshim = "rshim-modules";
}

my $kernel_escaped = $kernel;
$kernel_escaped =~ s/\+/\\\+/g;

# upstream vs. legacy user-space
my $dep_on_rdma_core = "";
my $dep_ibverbs_providers = "";
my $COMMON_DIR = "COMMON";
my $MLNX_LIBS_DIR = "MLNX_LIBS";
my $UPSTREAM_LIBS_DIR = "UPSTREAM_LIBS";

my $infiniband_diags_parent = "infiniband-diags";
my @infiniband_diags_deps = ("libibumad", "libopensm", "libibmad");

my $libibverbs_parent = "libibverbs";

my $libmlx4_1 = "libmlx4-1";
my $libmlx4_dev = "libmlx4-dev";
my $libmlx4_1_dbg = "libmlx4-1-dbg";

my $libmlx5_1 = "libmlx5-1";
my $libmlx5_dev = "libmlx5-dev";
my $libmlx5_1_dbg = "libmlx5-1-dbg";

my $librxe_1 = "librxe-1";
my $librxe_dev = "librxe-dev";
my $librxe_1_dbg = "librxe-1-dbg";

my $ibacm_parent = "ibacm";
my $ibacm_dev = "ibacm-dev";

my $libibcm_parent = "libibcm";
my $libibcm1 = "libibcm1";
my $libibcm_dev = "libibcm-dev";

my $libibmad = "libibmad";
my $libibmad_parent = "libibmad";
my $libibmad_dev = "libibmad-devel";
my $libibmad_static = "libibmad-static";
my $libibmad_dbg = "libibmad5-dbg";

my $libibumad_parent = "libibumad";
my $libibumad = "libibumad";
my $libibumad_devel = "libibumad-devel";
my $libibumad_static = "libibumad-static";

my $librdmacm_parent = "librdmacm";
my $librdmacm_utils = "librdmacm-utils";

my $srptools_parent = "srptools";

if ($use_upstream_libs) {
	$dep_on_rdma_core = "rdma-core";
	$dep_ibverbs_providers = "ibverbs-providers";

	$libibverbs_parent = "rdma-core";

	$libmlx4_1 = "ibverbs-providers";
	$libmlx4_dev = "ibibverbs-dev";
	$libmlx4_1_dbg = "libibverbs1-dbg";

	$libmlx5_1 = "ibverbs-providers";
	$libmlx5_dev = "ibibverbs-dev";
	$libmlx5_1_dbg = "libibverbs1-dbg";

	$librxe_1 = "ibverbs-providers";
	$librxe_dev = "ibibverbs-dev";
	$librxe_1_dbg = "libibverbs1-dbg";

	$ibacm_parent = "rdma-core";
	$ibacm_dev = "ibacm";

	$libibmad = "libibmad5";
	$libibmad_parent = "rdma-core";
	$libibmad_dev = "libibmad-dev";
	$libibmad_static = "";

	$libibumad_parent = "rdma-core";
	$libibumad = "libibumad3";
	$libibumad_devel = "libibumad-dev";
	$libibumad_static = "";

	$librdmacm_parent = "rdma-core";
	$librdmacm_utils = "rdmacm-utils";

	$srptools_parent = "rdma-core";

	$infiniband_diags_parent = "rdma-core";
	@infiniband_diags_deps = ("$libibumad", "libibnetdisc5", "$libibmad");

	$libibcm1 = "";
	$libibcm_dev = "";
}

my @mlnx_dpdk_packages = (
				"mlnx-dpdk",
				"mlnx-dpdk-dbgsym",
				"mlnx-dpdk-dev",
				"mlnx-dpdk-dev-dbgsym",
				"mlnx-dpdk-doc"
			);

my @openvswitch_packages = (
				"libopenvswitch",
				"libopenvswitch-dev",
				"openvswitch-common",
				"openvswitch-datapath-dkms",
				"openvswitch-datapath-source",
				"openvswitch-dbg",
				"openvswitch-ipsec",
				"openvswitch-pki",
				"openvswitch-switch",
				"openvswitch-test",
				"openvswitch-testcontroller",
				"openvswitch-vtep",
				"ovn-central",
				"ovn-common",
				"ovn-controller-vtep",
				"ovn-docker",
				"ovn-host",
				"python-openvswitch"
	);

push (@remove_debs, @mlnx_dpdk_packages);
if (($with_ovs_dpdk or $with_bluefield) and $arch =~ /x86_64|aarch64/) {
	push (@remove_debs, @openvswitch_packages);
}

# custom packages
my @all_packages = (
				"ofed-scripts",
				"mlnx-ofed-kernel-utils", "$mlnX_ofed_kernel",
				"$rshim",
				"$iser",
				"$isert",
				"$srp",
				"$mlnx_sdp",
				"$mlnx_rds",
				"$mlnx_nfsrdma",
				"$mlnx_nvme",
				"$mlnx_rdma_rxe",
				"libibverbs1", "ibverbs-utils", "libibverbs-dev", "libibverbs1-dbg",
				"$libmlx4_1", "$libmlx4_dev", "$libmlx4_1_dbg",
				"$libmlx5_1", "$libmlx5_dev", "$libmlx5_1_dbg",
				"$librxe_1", "$librxe_dev", "$librxe_1_dbg",
				"$libibumad", "$libibumad_static", "$libibumad_devel",
				"ibacm", "$ibacm_dev",
				"librdmacm1", "$librdmacm_utils", "librdmacm-dev",
				"mstflint",
				"ibdump",
				"$libibmad", "$libibmad_static", "$libibmad_dev",
				"opensm", "libopensm", "opensm-doc", "libopensm-devel",
				"infiniband-diags", "infiniband-diags-compat",
				"mft", "$kernel_mft",
				"libibcm1", "libibcm-dev",
				"ibacm", "$ibacm_dev",
				"perftest",
				"ibutils2",
				"libibdm1", "ibutils",
				"cc-mgr",
				"ar-mgr",
				"dump-pr",
				"ibsim", "ibsim-doc",
				"mxm",
				"ucx",
				"sharp",
				"fca",
				"hcoll",
				"openmpi",
				"openshmem",
				"mpitests",
				"knem", "$knem",
				"ummunotify", "$ummunotify",
				"rds-tools",
				"libdapl2", "dapl2-utils", "libdapl-dev",
				"libvma", "libvma-utils", "libvma-dev",
				"sockperf",
				"srptools",
				"mlnx-ethtool",
				"mlnx-iproute2",
				"libsdp1", "libsdp-dev",
				"sdpnetstat",
				"libdisni-java-jni"
);

my @basic_packages = (
				"ofed-scripts",
				"mlnx-ofed-kernel-utils", "$mlnX_ofed_kernel",
				"$rshim",
				"$iser",
				"$isert",
				"$srp",
				"$mlnx_sdp",
				"$mlnx_rds",
				"$mlnx_nfsrdma",
				"$mlnx_nvme",
				"$mlnx_rdma_rxe",
				"libibverbs1", "ibverbs-utils", "libibverbs-dev", "libibverbs1-dbg",
				"$libmlx4_1", "$libmlx4_dev", "$libmlx4_1_dbg",
				"$libmlx5_1", "$libmlx5_dev", "$libmlx5_1_dbg",
				"$librxe_1", "$librxe_dev", "$librxe_1_dbg",
				"$libibumad", "$libibumad_static", "$libibumad_devel",
				"ibacm", "$ibacm_dev",
				"librdmacm1", "$librdmacm_utils", "librdmacm-dev",
				"mstflint",
				"ibdump",
				"$libibmad", "$libibmad_static", "$libibmad_dev",
				"opensm", "libopensm", "opensm-doc", "libopensm-devel",
				"infiniband-diags", "infiniband-diags-compat",
				"mft", "$kernel_mft",
				"srptools",
				"ibdump",
				"mlnx-ethtool",
				"mlnx-iproute2",
);

my @bluefield_packages = (
				"ofed-scripts",
				"mlnx-ofed-kernel-utils", "$mlnX_ofed_kernel",
				"$rshim",
				"$iser",
				"$isert",
				"$srp",
				"$mlnx_nvme",
				"$mlnx_rdma_rxe",
				"libibverbs1", "ibverbs-utils", "libibverbs-dev",
				"$libmlx5_1", "$libmlx5_dev",
				"$librxe_1", "$librxe_dev",
				"$libibumad", "$libibumad_static", "$libibumad_devel",
				"ibacm", "$ibacm_dev",
				"perftest",
				"ibutils2",
				"ibutils",
				"librdmacm1", "$librdmacm_utils", "librdmacm-dev",
				"mstflint",
				"ibdump",
				"$libibmad", "$libibmad_static", "$libibmad_dev",
				"opensm", "libopensm", "opensm-doc", "libopensm-devel",
				"infiniband-diags", "infiniband-diags-compat",
				"mft", "$kernel_mft",
				"srptools",
				"ibdump",
				"mlnx-ethtool",
				"mlnx-iproute2",
				"nvme-snap",
);

my @hpc_packages = (
				@basic_packages,
				"libibcm1", "libibcm-dev",
				"ibacm", "$ibacm_dev",
				"perftest",
				"ibutils2",
				"ibutils", "libibdm1",
				"cc-mgr",
				"ar-mgr",
				"dump-pr",
				"ibsim", "ibsim-doc",
				"mxm",
				"ucx",
				"sharp",
				"fca",
				"hcoll",
				"openmpi",
				"openshmem",
				"mpitests",
				"knem", "$knem",
				"ummunotify", "$ummunotify",
				"rds-tools",
				"libdapl2", "dapl2-utils", "libdapl-dev",
);

my @vma_packages = (
				@basic_packages,
				"libibcm1", "libibcm-dev",
				"perftest",
				"ibutils2",
				"libibdm1", "ibutils",
				"cc-mgr",
				"ar-mgr",
				"dump-pr",
				"ibsim", "ibsim-doc",
				"libvma", "libvma-utils", "libvma-dev",
				"sockperf",
);

my @vmavpi_packages = (
				@basic_packages,
				"libibcm1", "libibcm-dev",
				"perftest",
				"ibutils2",
				"libibdm1", "ibutils",
				"cc-mgr",
				"ar-mgr",
				"dump-pr",
				"ibsim", "ibsim-doc",
				"libvma", "libvma-utils", "libvma-dev",
				"sockperf",
);

my @vmaeth_packages = (
				"ofed-scripts",
				"mlnx-ofed-kernel-utils", "$mlnX_ofed_kernel",
				"$iser",
				"$isert",
				"$srp",
				"$mlnx_sdp",
				"$mlnx_rds",
				"$mlnx_nfsrdma",
				"$mlnx_nvme",
				"$mlnx_rdma_rxe",
				"libibverbs1", "ibverbs-utils", "libibverbs-dev", "libibverbs1-dbg",
				"$libmlx4_1", "$libmlx4_dev", "$libmlx4_1_dbg",
				"$libmlx5_1", "$libmlx5_dev", "$libmlx5_1_dbg",
				"$librxe_1", "$librxe_dev", "$librxe_1_dbg",
				"$libibumad", "$libibumad_static", "$libibumad_devel",
				"ibacm", "$ibacm_dev",
				"librdmacm1", "$librdmacm_utils", "librdmacm-dev",
				"mstflint",
				"ibdump",
				"mft", "$kernel_mft",
				"libvma", "libvma-utils", "libvma-dev",
				"sockperf",
				"mlnx-ethtool",
				"mlnx-iproute2",
);

my @guest_packages = (
				@basic_packages,
				"ibacm", "$ibacm_dev",
				"perftest",
				"libdapl2", "dapl2-utils", "libdapl-dev",
				"rds-tools",
				"mxm",
				"ucx",
				"sharp",
				"fca",
				"hcoll",
				"openmpi",
				"openshmem",
				"mpitests",
				"knem", "$knem",
				"ummunotify", "$ummunotify",
);

my @hypervisor_packages = (
				@basic_packages,
				"ibacm", "$ibacm_dev",
				"perftest",
				"libdapl2", "dapl2-utils", "libdapl-dev",
				"rds-tools",
				"fca",
				"ibutils2",
				"libibdm1", "ibutils",
);

my @eth_packages = (
				"$mlnx_en",
				"mlnx-en-utils",
				"mstflint",
);

my @dpdk_packages = (
				"ofed-scripts", "mstflint",
				"mlnx-ofed-kernel-utils", "$mlnX_ofed_kernel", "$rshim",
				"libibverbs1", "ibverbs-utils", "libibverbs-dev",
				"$libmlx4_1", "$libmlx4_dev",
				"$libmlx5_1", "$libmlx5_dev",
				"librdmacm1", "$librdmacm_utils", "librdmacm-dev", "ibacm",
);

##
my %kernel_packages = ("$mlnX_ofed_kernel" => {'ko' => ["mlx5_ib", "mlx5_core"]},
			"$knem" => {'ko' => ["knem"]},
			"$kernel_mft" => {'ko' => ["mst_pci", "mst_pciconf"]},
			"$ummunotify" => {'ko' => ["ummunotify"]},
			"$iser" => {'ko' => ["ib_iser"]},
			"$isert" => {'ko' => ["ib_isert"]},
			"$srp" => {'ko' => ["ib_srp"]},
			"$mlnx_sdp" => {'ko' => ["ib_sdp"]},
			"$mlnx_rds" => {'ko' => ["rds"]},
			"$mlnx_nfsrdma" => {'ko' => ["rpcrdma"]},
			"$mlnx_nvme" => {'ko' => ["nvme_rdma"]},
			"$mlnx_rdma_rxe" => {'ko' => ["rdma_rxe"]},
			"$rshim" => {'ko' => ["rshim", "rshim_usb", "rshim_pcie", "rshim_net"]},
			);

## set OS, arch
# don't auto-detect distro if it's provided by the user.
if ($distro eq "") {
	print_and_log("Distro was not provided, trying to auto-detect the current distro...\n", $verbose2);
	my $dist_os  = os_release('ID');
	if (not $dist_os) {
		print_and_log("/etc/os-release is missing or invalid\n");
	}
	my $dist_ver = os_release('VERSION_ID');
	$distro = "$dist_os$dist_ver";
	print_and_log("Auto-detected $distro distro.\n", $verbose2);
} else {
	$distro = lc($distro);
	print_and_log("Using provided distro: $distro\n", $verbose2);
}

##

if ($distro !~ m/debian | ubuntu/x) {
	print_and_log_colored("Current operation system is not supported ($distro)!", 1, "RED");
	exit 1;
}

my $DEBS  = "$CWD/DEBS/$distro/$arch";
chomp $DEBS;
mkpath(["$DEBS/$COMMON_DIR"]);
if ($use_upstream_libs) {
	mkpath(["$DEBS/$UPSTREAM_LIBS_DIR"]);
} else {
	mkpath(["$DEBS/$MLNX_LIBS_DIR"]);
}

# OS specific package names
my $module_tools = "kmod";
my $libssl = "libssl1.0.0";
my $libssl_devel = "libssl-dev";
my @rdmacore_python = qw/cython3 python3-dev/;
my @libsystemd_dev = qw/libsystemd-dev/;
if ($distro =~ /ubuntu1[0-7] | debian[5-8]/x) {
	@rdmacore_python = qw/python/; # older systems have no pyverbs
} else {
	push @dpdk_packages, "python3-pyverbs";
}
if ($distro =~ /ubuntu1[0-4] | debian[5-6]/x) {
	# Technically: libnih, practically: optional dependency:
	@libsystemd_dev = ();
}
if ( $distro =~ /debian6|ubuntu1[0-2]/) {
	$module_tools = "module-init-tools";
}
if ( $distro =~ /debian6\.0/) {
	$libssl = "libssl0.9.8";
}
if ($distro =~ /debian9/) {
	$libssl = "libssl1.0.2";
	$libssl_devel = "libssl1.0-dev";
} elsif ($distro =~ /ubuntu(19|2) | debian1[0-9]/x) {
	$libssl = "libssl1.1";
}

my $python2 = "python2";
if ($distro =~ /ubuntu18.04/) {
	$python2 = "python";
}


my $libunbound = "libunbound8";
if ( $distro =~ /debian[8-9] | ubuntu1[4-9]/x) {
	$libunbound = "";
}

# This is still a build dependency of openvswitch. However it
# has been a dummy package since 2013 and not really needed.
# Ubuntu 20.04 dropped it altogether. Sadly we have to build
# nodeps there.
my $python_conch = "python-twisted-conch";
if ($distro =~ /ubuntu20/) {
	$python_conch = "";
}

my $libudev = "libudev1";
my $libudev_devel = "libudev-dev";

my $dh_systemd = "debhelper";
if ($distro =~ /ubuntu1[0-7] | debian[4-8]/x) {
	$dh_systemd = "dh-systemd";
}

my $openjdk = "openjdk-11-jdk";
if ($distro =~ /ubuntu14 | debian8/x) {
	$openjdk = "openjdk-7-jdk";
} elsif ($distro =~ /ubuntu1[5-8] | debian9/x) {
	$openjdk = "openjdk-8-jdk";
}

my $libgfortran = "libgfortran4";
if ($distro =~ /ubuntu1[4-7] | debian([89])/x) {
	$libgfortran = "libgfortran3";
}

if ($with_ovs_dpdk and ($arch !~ /x86_64|aarch64/ or $distro !~ /ubuntu18.04 | ubuntu20.04/x)) {
	print YELLOW "\nWARNING: The '--ovs-dpdk' option is supported only on Ubuntu 18.04 or 20.04, x86_64 and aarch64. Disabling...", RESET "\n";
	$with_ovs_dpdk = 0;
}
###############

# define kernel modules
my @basic_kernel_modules = ("core", "mlxfw", "mthca", "mlx4", "mlx4_en", "mlx4_vnic", "mlx4_fc", "mlx5", "ipoib", "mlx5_fpga_tools", "mdev");
my @ulp_modules = ("sdp", "srp", "srpt", "rds", "iser", "e_ipoib", "nfsrdma", 'isert');
my @kernel_modules = (@basic_kernel_modules, @ulp_modules);
my @bluefield_kernel_modules = ("core", "mlxfw", "mlx5", "ipoib", "mlx5_fpga_tools", "mdev", "srp", "iser", "isert");
my @hpc_kernel_modules = (@basic_kernel_modules);
my @vma_kernel_modules = (@basic_kernel_modules);
my @dpdk_kernel_modules = (@basic_kernel_modules);
my @hypervisor_kernel_modules = ("core","mlxfw","mlx4","mlx4_en","mlx4_vnic","mlx5","ipoib","srp","iser", 'isert', "mlx5_fpga_tools", "mdev");
my @guest_kernel_modules = ("core","mlxfw","mlx4","mlx5","mlx4_en","ipoib","srp","iser", 'isert', "mlx5_fpga_tools", "mdev");
my @eth_kernel_modules = ("core", "mlxfw", "mlx4", "mlx4_en", "mlx5", "mdev");

# which modules are required for the standalone module rpms
my %standalone_kernel_modules_info = (
		"$iser" => ["core", "ipoib"],
		"$isert" => ["core", "ipoib"],
		"$srp" => ["core", "ipoib"],
		"$mlnx_sdp" => ["core"],
		"$mlnx_rds" => ["core"],
		"$mlnx_nfsrdma" => ["core"],
		"$mlnx_nvme" => ["core"],
		"$mlnx_rdma_rxe" => ["core"],
		"$rshim" => [""],
);

my %kernel_modules_info = (
			'core' =>
			{ name => "core", available => 1, selected => 0,
			included_in_rpm => 0, requires => [], },
			'mlxfw' =>
			{ name => "mlxfw", available => 1, selected => 0,
			included_in_rpm => 0, requires => [], },
			'mthca' =>
			{ name => "mthca", available => 0, selected => 0,
			included_in_rpm => 0, requires => ["core"], },
			'mlx4' =>
			{ name => "mlx4", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["core"], },
			'mlx5' =>
			{ name => "mlx5", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["core"], },
			'mlx4_en' =>
			{ name => "mlx4_en", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["core","mlx4"], },
			'mlx4_vnic' =>
			{ name => "mlx4_vnic", available => 0, selected => 0,
			included_in_rpm => 0, requires => ["core","mlx4"], },
			'mlx4_fc' =>
			{ name => "mlx4_fc", available => 0, selected => 0,
			included_in_rpm => 0, requires => ["core","mlx4_en"], },
			'ipoib' =>
			{ name => "ipoib", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["core"], },
			'mlx5_fpga_tools' =>
			{ name => "mlx5_fpga_tools", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["core"], },
			'sdp' =>
			{ name => "sdp", available => 0, selected => 0,
			included_in_rpm => 0, requires => ["core", "ipoib"], },
			'srp' =>
			{ name => "srp", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["core", "ipoib"], },
			'srpt' =>
			{ name => "srpt", available => 0, selected => 0,
			included_in_rpm => 0, requires => ["core"], },
			'rds' =>
			{ name => "rds", available => 0, selected => 0,
			included_in_rpm => 0, requires => ["core", "ipoib"], },
			'e_ipoib' =>
			{ name => "e_ipoib", available => 0, selected => 0,
			included_in_rpm => 0, requires => ["core", "ipoib"], },
			'iser' =>
			{ name => "iser", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["core", "ipoib"], ofa_req_inst => [] },
			'isert' =>
			{ name => "isert", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["core", "ipoib"], ofa_req_inst => [] },
			'nfsrdma' =>
			{ name => "nfsrdma", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["core", "ipoib"], },
			'mdev' =>
			{ name => "mdev", available => 1, selected => 0,
			included_in_rpm => 0, requires => ["mlx5"], },
);

# define packages
my %packages_info = (
			'ar-mgr' =>
				{ name => "ar-mgr", parent => "ar-mgr",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libstdc++6"],
				dist_req_inst => ["libstdc++6"],
				ofa_req_build => ["libopensm", "libopensm-devel", "ibutils2"],
				ofa_req_inst => ["opensm", "ibutils2"],
				configure_options => '' },
			'cc-mgr' =>
				{ name => "cc-mgr", parent => "cc-mgr",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libstdc++6"],
				dist_req_inst => ["libstdc++6"],
				ofa_req_build => ["libopensm", "libopensm-devel", "ibutils2"],
				ofa_req_inst => ["opensm", "ibutils2"],
				configure_options => '' },
			'dump-pr' =>
				{ name => "dump-pr", parent => "dump-pr",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libstdc++6"],
				dist_req_inst => ["libstdc++6"],
				ofa_req_build => ["libopensm", "libopensm-devel"],
				ofa_req_inst => ["opensm"],
				configure_options => '' },
			'fca' =>
				{ name => "fca", parent => "fca",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["gcc", "libstdc++6", "libcurl4-gnutls-dev"],
				dist_req_inst => ["curl"],
				ofa_req_build => ["librdmacm-dev", "libibverbs-dev", "$libibmad_dev", "$libibumad_devel", "libopensm-devel", "infiniband-diags-compat"],
				ofa_req_inst => ["librdmacm1", "infiniband-diags-compat", "mlnx-ofed-kernel-utils"],
				configure_options => '' },
			'ibacm-dev' =>
				{ name => "ibacm-dev", parent => "$ibacm_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "$libibumad_devel", "$mlnX_ofed_kernel"],
				ofa_req_inst => ["ibacm", "$dep_on_rdma_core"],
				configure_options => '' },
			'ibacm' =>
				{ name => "ibacm", parent => "$ibacm_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "$libibumad_devel", "$mlnX_ofed_kernel"],
				ofa_req_inst => ["libibverbs1", "$libibumad", "$dep_on_rdma_core"],
				configure_options => '' },
			'ibsim-doc' =>
				{ name => "ibsim-doc", parent => "ibsim",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["$libibmad_dev", "$libibumad_devel"],
				ofa_req_inst => ["$libibmad", "$libibumad"],
				configure_options => '' },
			'ibsim' =>
				{ name => "ibsim", parent => "ibsim",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["$libibmad_dev", "$libibumad_devel"],
				ofa_req_inst => ["$libibmad", "$libibumad"],
				configure_options => '' },
			'ibutils2' =>
				{ name => "ibutils2", parent => "ibutils2",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["tcl-dev", "tk-dev", "libstdc++6"],
				dist_req_inst => ["tcl", "tk", "libstdc++6"],
				ofa_req_build => ["$libibumad_devel"],
				ofa_req_inst => ["$libibumad"],
				configure_options => '' },
			'ibutils' =>
				{ name => "ibutils", parent => "ibutils",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["tcl-dev", "tk-dev", "libstdc++6"],
				dist_req_inst => ["tcl", "tk", "libstdc++6"],
				ofa_req_build => ["libopensm", "libopensm-devel", "$libibumad_devel", "libibverbs-dev"],
				ofa_req_inst => ["libibdm1", "$libibumad", "libopensm"],
				configure_options => '' },
			'libibdm1' =>
				{ name => "libibdm1", parent => "ibutils",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["tk", "tk-dev", "libstdc++6"],
				dist_req_inst => ["tk", "libstdc++6"],
				ofa_req_build => ["libopensm", "libopensm-devel", "$libibumad_devel", "libibverbs-dev"],
				ofa_req_inst => ["$libibumad", "libopensm"],
				configure_options => '' },
			'infiniband-diags' =>
				{ name => "infiniband-diags", parent => "$infiniband_diags_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libglib2.0-dev"],
				dist_req_inst => ["libglib2.0-0"],
				ofa_req_build => ["libopensm", "libopensm-devel", "$libibumad_devel", "$libibmad_dev"],
				ofa_req_inst => [@infiniband_diags_deps],
				configure_options => '' },
			'infiniband-diags-compat' =>
				{ name => "infiniband-diags-compat", parent => "$infiniband_diags_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libglib2.0-dev"],
				dist_req_inst => [],
				ofa_req_build => ["libopensm", "libopensm-devel", "$libibumad_devel", "$libibmad_dev"],
				ofa_req_inst => ["infiniband-diags", "$libibumad", "libopensm", "$libibmad"],
				configure_options => '' },
			'infiniband-diags-guest' =>
				{ name => "infiniband-diags-guest", parent => "$infiniband_diags_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => ["libglib2.0-dev"],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'kernel-mft-dkms' =>
				{ name => "kernel-mft-dkms", parent => "kernel-mft",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'kernel-mft' =>
				{ name => "kernel-mft", parent => "kernel-mft",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'kernel-mft-modules' =>
				{ name => "kernel-mft-modules", parent => "kernel-mft",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'knem-dkms' =>
				{ name => "knem-dkms", parent => "knem",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["build-essential", "debhelper", "pkg-config", "bzip2", "dh-autoreconf", "dkms"],
				dist_req_inst => ["dkms", "gcc", "make", "pkg-config"],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'knem-modules' =>
				{ name => "knem-modules", parent => "knem",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["build-essential", "debhelper", "pkg-config", "bzip2", "dh-autoreconf"],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'knem' =>
				{ name => "knem", parent => "knem",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["build-essential", "debhelper", "pkg-config", "bzip2", "dh-autoreconf"],
				dist_req_inst => ["pkg-config"],
				ofa_req_build => [],
				ofa_req_inst => [$knem],
				configure_options => '' },
			'dapl' =>
				{ name => "dapl", parent => "dapl",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["librdmacm-dev", "libibverbs-dev", "fca"],
				ofa_req_inst => ["libibverbs1", "librdmacm1", "fca"],
				soft_req => ["fca"],
				configure_options => '' },
			'dapl2-utils' =>
				{ name => "dapl2-utils", parent => "dapl",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["librdmacm-dev", "libibverbs-dev", "fca"],
				ofa_req_inst => ["libibverbs1", "fca", "librdmacm1"],
				soft_req => ["fca"],
				configure_options => '' },
			'libdapl-dev' =>
				{ name => "libdapl-dev", parent => "dapl",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["librdmacm-dev", "libibverbs-dev", "fca"],
				ofa_req_inst => ["libdapl2", "fca", "librdmacm1"],
				soft_req => ["fca"],
				configure_options => '' },
			'libdapl2' =>
				{ name => "libdapl2", parent => "dapl",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["librdmacm-dev", "libibverbs-dev", "fca"],
				ofa_req_inst => ["libibverbs1", "fca", "librdmacm1"],
				soft_req => ["fca"],
				configure_options => '' },
			'libibcm' =>
				{ name => "libibcm", parent => "$libibcm_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libibcm1", "$dep_on_rdma_core"],
				configure_options => '' },
			'libibcm-dev' =>
				{ name => "libibcm-dev", parent => "$libibcm_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libibcm1", "$dep_on_rdma_core"],
				configure_options => '' },
			'libibcm1' =>
				{ name => "libibcm1", parent => "$libibcm_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libibverbs1", "$dep_on_rdma_core"],
				configure_options => '' },
			'libibmad-dev' =>
				{ name => "libibmad-dev", parent => "$libibmad_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["$libibumad_devel"],
				ofa_req_inst => ["$libibmad"],
				configure_options => '' },
			'libibmad-devel' =>
				{ name => "libibmad-devel", parent => "$libibmad_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["$libibumad_devel"],
				ofa_req_inst => ["$libibmad"],
				configure_options => '' },
			'libibmad-dbg' =>
				{ name => "$libibmad_dbg", parent => "$libibmad_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["$libibumad_devel"],
				ofa_req_inst => ["$libibmad"],
				configure_options => '' },
			'libibmad-static' =>
				{ name => "libibmad-static", parent => "libibmad",
				selselected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["$libibumad_devel"],
				ofa_req_inst => ["$libibmad"],
				configure_options => '' },
			'libibmad' =>
				{ name => "$libibmad", parent => "$libibmad_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libtool"],
				dist_req_inst => [],
				ofa_req_build => ["$libibumad_devel"],
				ofa_req_inst => ["$libibumad"],
				configure_options => '' },
			'libibmad5' =>
				{ name => "$libibmad", parent => "$libibmad_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libtool"],
				dist_req_inst => [],
				ofa_req_build => ["$libibumad_devel"],
				ofa_req_inst => ["$libibumad"],
				configure_options => '' },
			'libibnetdisc5' =>
				{ name => "libibnetdisc5", parent => "$libibverbs_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => ["$libibmad", "$libibumad"],
				configure_options => '' },
			'libibnetdisc-dev' =>
				{ name => "libibnetdisc-dev", parent => "$libibverbs_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => ["libibnetdisc5"],
				configure_options => '' },
			'libibnetdisc5-dbg' =>
				{ name => "libibnetdisc5-dbg", parent => "$libibverbs_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => ["libibnetdisc5"],
				configure_options => '' },
			'libibumad-devel' =>
				{ name => "$libibumad_devel", parent => "$libibumad_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libtool"],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => ["$libibumad"],
				configure_options => '' },
			'libibumad-static' =>
				{ name => "libibumad-static", parent => "$libibumad_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libtool"],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => ["$libibumad", "$dep_on_rdma_core"],
				configure_options => '' },
			'libibumad' =>
				{ name => "$libibumad", parent => "$libibumad_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libtool"],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => ["$dep_on_rdma_core"],
				configure_options => '' },
			'libibverbs' =>
				{ name => "libibverbs", parent => "$libibverbs_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => ["libglib2.0-dev"],
				dist_req_inst => ["libglib2.0-0"],
				ofa_req_build => [],
				ofa_req_inst => ["libibverbs1", "$dep_on_rdma_core"],
				configure_options => '' },
			'ibverbs-utils' =>
				{ name => "ibverbs-utils", parent => "$libibverbs_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => ["libibverbs1"],
				configure_options => '' },
			'libibverbs-dev' =>
				{ name => "libibverbs-dev", parent => "$libibverbs_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => ["libibverbs1", "$dep_ibverbs_providers"],
				configure_options => '' },
			'libibverbs1-dbg' =>
				{ name => "libibverbs1-dbg", parent => "$libibverbs_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => ["libibverbs1", "$dep_on_rdma_core"],
				configure_options => '' },
			'libibverbs1' =>
				{ name => "libibverbs1", parent => "$libibverbs_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libnl-3-dev", "libnl-route-3-dev", "pkg-config"],
				dist_req_inst => ["libnl-3-200", "libnl-route-3-200", "adduser"],
				ofa_req_build => [],
				ofa_req_inst => ["$dep_on_rdma_core"],
				configure_options => '' },
			'libmlx4' =>
				{ name => "libmlx4", parent => "libmlx4",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => [],
				configure_options => '' },
			'libmlx4-1' =>
				{ name => "libmlx4-1", parent => "libmlx4",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libibverbs1"],
				configure_options => '' },
			'libmlx4-dev' =>
				{ name => "libmlx4-dev", parent => "libmlx4",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libmlx4-1"],
				configure_options => '' },
			'libmlx4-1-dbg' =>
				{ name => "libmlx4-1-dbg", parent => "libmlx4",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libmlx4-1"],
				configure_options => '' },
			'libmlx5' =>
				{ name => "libmlx5", parent => "libmlx5",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => ["libnuma-dev"],
				dist_req_inst => ["libnuma1"],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => [],
				configure_options => '' },
			'libmlx5-1-dbg' =>
				{ name => "libmlx5-1-dbg", parent => "libmlx5",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libnuma-dev"],
				dist_req_inst => ["libnuma1"],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libmlx5-1"],
				configure_options => '' },
			'libmlx5-1' =>
				{ name => "libmlx5-1", parent => "libmlx5",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libnuma-dev"],
				dist_req_inst => ["libnuma1"],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libibverbs1"],
				configure_options => '' },
			'libmlx5-dev' =>
				{ name => "libmlx5-dev", parent => "libmlx5",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libnuma-dev"],
				dist_req_inst => ["libnuma1"],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libmlx5-1"],
				configure_options => '' },

			'librxe' =>
				{ name => "librxe", parent => "librxe",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "$mlnX_ofed_kernel"],
				ofa_req_inst => [],
				configure_options => '' },
			'librxe-1' =>
				{ name => "librxe-1", parent => "librxe",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "$mlnX_ofed_kernel"],
				ofa_req_inst => ["libibverbs1"],
				configure_options => '' },
			'librxe-dev' =>
				{ name => "librxe-dev", parent => "librxe",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "$mlnX_ofed_kernel"],
				ofa_req_inst => ["librxe-1"],
				configure_options => '' },
			'librxe-1-dbg' =>
				{ name => "librxe-1-dbg", parent => "librxe",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "$mlnX_ofed_kernel"],
				ofa_req_inst => ["librxe-1"],
				configure_options => '' },

			'librdmacm' =>
				{ name => "librdmacm", parent => "$librdmacm_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["$dep_on_rdma_core"],
				configure_options => '' },
			'librdmacm-dev' =>
				{ name => "librdmacm-dev", parent => "$librdmacm_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["librdmacm1", "$dep_on_rdma_core"],
				configure_options => '' },
			'librdmacm-utils' =>
				{ name => "librdmacm-utils", parent => "$librdmacm_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["librdmacm1", "$dep_on_rdma_core"],
				configure_options => '' },
			'librdmacm1' =>
				{ name => "librdmacm1", parent => "$librdmacm_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev"],
				ofa_req_inst => ["libibverbs1", "$dep_on_rdma_core"],
				configure_options => '' },
			'libvma' =>
				{ name => "libvma", parent => "libvma",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libnl-3-dev"],
				dist_req_inst => ["libnl-3-200"],
				ofa_req_build => ["librdmacm1", "librdmacm-dev", "libibverbs1", "libibverbs-dev"],
				ofa_req_inst => ["librdmacm1", "libibverbs1"],
				configure_options => '' },
			'libvma-utils' =>
				{ name => "libvma-utils", parent => "libvma",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["librdmacm1", "librdmacm-dev", "libibverbs1", "libibverbs-dev"],
				ofa_req_inst => ["librdmacm1", "libibverbs1", "libvma"],
				configure_options => '' },
			'libvma-dev' =>
				{ name => "libvma-dev", parent => "libvma",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["librdmacm1", "librdmacm-dev", "libibverbs1", "libibverbs-dev"],
				ofa_req_inst => ["librdmacm1", "libibverbs1", "libvma"],
				configure_options => '' },

			'sockperf' =>
				{ name => "sockperf", parent => "sockperf",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["doxygen"],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },

			'mft' =>
				{ name => "mft", parent => "mft",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'mlnx-ofed-kernel' =>
				{ name => "mlnx-ofed-kernel", parent => "mlnx-ofed-kernel",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["dkms", "quilt", "make", "gcc"],
				dist_req_inst => ["dkms", "quilt", "make", "gcc", "coreutils", "pciutils", "grep", "perl", "procps", "$module_tools", "lsof"],
				ofa_req_build => ["$mlnX_ofed_kernel", "mlnx-ofed-kernel-utils"],
				ofa_req_inst => ["$mlnX_ofed_kernel", "mlnx-ofed-kernel-utils"],
				soft_req => ["ofed-scripts"],
				configure_options => '' },
			'mlnx-ofed-kernel-dkms' =>
				{ name => "mlnx-ofed-kernel-dkms", parent => "mlnx-ofed-kernel",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "quilt", "make", "gcc"],
				dist_req_inst => ["dkms", "quilt", "make", "gcc", "coreutils", "pciutils", "grep", "perl", "procps", "$module_tools", "lsof"],
				ofa_req_build => [],
				ofa_req_inst => ["ofed-scripts", "mlnx-ofed-kernel-utils"],
				soft_req => ["ofed-scripts"],
				configure_options => '' },
			'mlnx-ofed-kernel-modules' =>
				{ name => "mlnx-ofed-kernel-modules", parent => "mlnx-ofed-kernel",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["quilt", "make", "gcc"],
				dist_req_inst => ["quilt", "make", "gcc", "coreutils", "pciutils", "grep", "perl", "procps", "$module_tools", "lsof"],
				ofa_req_build => [],
				ofa_req_inst => ["ofed-scripts", "mlnx-ofed-kernel-utils"],
				soft_req => ["ofed-scripts"],
				configure_options => '' },
			'mlnx-ofed-kernel-utils' =>
				{ name => "mlnx-ofed-kernel-utils", parent => "mlnx-ofed-kernel",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["quilt", "make", "gcc"],
				dist_req_inst => ["ethtool", "coreutils", "pciutils", "grep", "perl", "procps", "$module_tools", "lsof"],
				ofa_req_build => [],
				ofa_req_inst => ["ofed-scripts"],
				soft_req => ["ofed-scripts"],
				configure_options => '' },

			# eth only
			'mlnx-en' =>
				{ name => "mlnx-en", parent => "mlnx-en",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["dkms", "quilt", "make", "gcc"],
				dist_req_inst => ["dkms", "quilt", "make", "gcc", "coreutils", "pciutils", "grep", "perl", "procps", "$module_tools", "lsof"],
				ofa_req_build => ["$mlnx_en", "mlnx-en-utils"],
				ofa_req_inst => ["$mlnx_en", "mlnx-en-utils"],
				soft_req => ["ofed-scripts"],
				configure_options => '' },
			'mlnx-en-dkms' =>
				{ name => "mlnx-en-dkms", parent => "mlnx-en",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "quilt", "make", "gcc"],
				dist_req_inst => ["dkms", "quilt", "make", "gcc", "coreutils", "pciutils", "grep", "perl", "procps", "$module_tools", "lsof"],
				ofa_req_build => [],
				ofa_req_inst => ["mlnx-en-utils"],
				soft_req => ["ofed-scripts"],
				configure_options => '' },
			'mlnx-en-modules' =>
				{ name => "mlnx-en-modules", parent => "mlnx-en",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["quilt", "make", "gcc"],
				dist_req_inst => ["quilt", "make", "gcc", "coreutils", "pciutils", "grep", "perl", "procps", "$module_tools", "lsof"],
				ofa_req_build => [],
				ofa_req_inst => ["mlnx-en-utils"],
				soft_req => ["ofed-scripts"],
				configure_options => '' },
			'mlnx-en-utils' =>
				{ name => "mlnx-en-utils", parent => "mlnx-en",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["quilt", "make", "gcc"],
				dist_req_inst => ["quilt", "make", "gcc", "coreutils", "pciutils", "grep", "perl", "procps", "$module_tools", "lsof"],
				ofa_req_build => [],
				ofa_req_inst => ['ofed-scripts'],
				soft_req => ["ofed-scripts"],
				configure_options => '' },

			'mpitests' =>
				{ name => "mpitests", parent => "mpitests",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["$libgfortran"],
				dist_req_inst => ["$libgfortran", "gfortran"],
				ofa_req_build => ["openmpi", "$libibumad_devel", "librdmacm-dev", "$libibmad_dev"],
				ofa_req_inst => ["openmpi", "$libibumad", "librdmacm1", "$libibmad"],
				configure_options => '' },
			'mstflint' =>
				{ name => "mstflint", parent => "mstflint",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["$libssl_devel"],
				dist_req_inst => ["$libssl"],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'mxm' =>
				{ name => "mxm", parent => "mxm",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libstdc++6", "pkg-config"],
				dist_req_inst => ["libstdc++6", "pkg-config"],
				ofa_req_build => ["libibverbs-dev","librdmacm-dev","$libibmad_dev","$libibumad_devel","knem"],
				ofa_req_inst => ["$libibumad", "libibverbs1", "knem"],
				soft_req => ["knem"],
				configure_options => '' },
			'ucx' =>
				{ name => "ucx", parent => "ucx",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libstdc++6", "pkg-config"],
				dist_req_inst => ["libstdc++6", "pkg-config"],
				soft_req => ["libibcm-dev", "libibcm1"],
				ofa_req_build => ["libibverbs-dev", "$libibcm_dev"],
				ofa_req_inst => ["libibverbs1", "$libibcm1"],
				configure_options => '' },

			'ofed-scripts' =>
				{ name => "ofed-scripts", parent => "ofed-scripts",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'openmpi' =>
				{ name => "openmpi", parent => "openmpi",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libstdc++6", "$libgfortran"],
				dist_req_inst => ["libstdc++6", "$libgfortran", "gfortran"],
				ofa_req_build => ["libibverbs-dev", "librdmacm-dev", "fca", "hcoll", "mxm", "ucx", "knem", "$libibmad_dev", "sharp"],
				ofa_req_inst => ["libibverbs1", "fca", "hcoll", "mxm", "ucx", "knem", "$libibmad", "sharp"],
				soft_req => ["fca", "hcoll", "mxm", "knem", "sharp"],
				configure_options => '' },
			'openshmem' =>
				{ name => "openshmem", parent => "openshmem",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["fca", "libopensm-devel", "knem", "mxm", "$libibmad_dev", "librdmacm-dev"],
				ofa_req_inst => ["fca", "libopensm", "knem", "mxm", "$libibmad", "librdmacm1", "libopensm"],
				configure_options => '' },
			'opensm' =>
				{ name => "opensm", parent => "opensm",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["bison", "flex"],
				dist_req_inst => ["bison", "flex"],
				ofa_req_build => ["libopensm", $libibumad_devel],
				ofa_req_inst => ["libopensm", "$libibumad"],
				configure_options => '' },
			'opensm-doc' =>
				{ name => "opensm-doc", parent => "opensm",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["bison", "flex"],
				dist_req_inst => ["bison", "flex"],
				ofa_req_build => ["opensm"],
				ofa_req_inst => ["opensm"],
				configure_options => '' },
			'libopensm-devel' =>
				{ name => "libopensm-devel", parent => "opensm",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["bison", "flex"],
				dist_req_inst => ["bison", "flex"],
				ofa_req_build => [],
				ofa_req_inst => ["opensm", "libopensm"],
				configure_options => '' },
			'libopensm' =>
				{ name => "libopensm", parent => "opensm",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["bison", "flex"],
				dist_req_inst => ["bison", "flex"],
				ofa_req_build => ["$libibumad_devel"],
				ofa_req_inst => ["$libibumad"],
				configure_options => '' },
			'perftest' =>
				{ name => "perftest", parent => "perftest",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "librdmacm-dev", "$libibumad_devel"],
				ofa_req_inst => ["libibverbs1", "librdmacm1", "$libibumad", $dep_ibverbs_providers],
				configure_options => '' },
			'rds-tools' =>
				{ name => "rds-tools", parent => "rds-tools",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'ummunotify-dkms' =>
				{ name => "ummunotify-dkms", parent => "ummunotify",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'ummunotify-modules' =>
				{ name => "ummunotify-modules", parent => "ummunotify",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'ummunotify' =>
				{ name => "ummunotify", parent => "ummunotify",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "kernel",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'srptools' =>
				{ name => "srptools", parent => "$srptools_parent",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["librdmacm-dev", "libibverbs-dev", "$libibumad_devel"],
				ofa_req_inst => ["librdmacm1", "$libibumad", "libibverbs1", "$dep_on_rdma_core"],
				configure_options => '' },

			'iser' =>
				{ name => "iser", parent => "iser",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'iser-dkms' =>
				{ name => "iser-dkms", parent => "iser",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },
			'iser-modules' =>
				{ name => "iser-modules", parent => "iser",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },

			'isert' =>
				{ name => "isert", parent => "isert",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'isert-dkms' =>
				{ name => "isert-dkms", parent => "isert",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },
			'isert-modules' =>
				{ name => "isert-modules", parent => "isert",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },

			'srp' =>
				{ name => "srp", parent => "srp",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'srp-dkms' =>
				{ name => "srp-dkms", parent => "srp",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },
			'srp-modules' =>
				{ name => "srp-modules", parent => "srp",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },

			'mlnx-sdp' =>
				{ name => "mlnx-sdp", parent => "mlnx-sdp",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'mlnx-sdp-dkms' =>
				{ name => "mlnx-sdp-dkms", parent => "mlnx-sdp",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },
			'mlnx-sdp-modules' =>
				{ name => "mlnx-sdp-modules", parent => "mlnx-sdp",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },

			'mlnx-rds' =>
				{ name => "mlnx-rds", parent => "mlnx-rds",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'mlnx-rds-dkms' =>
				{ name => "mlnx-rds-dkms", parent => "mlnx-rds",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },
			'mlnx-rds-modules' =>
				{ name => "mlnx-rds-modules", parent => "mlnx-rds",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },

			'mlnx-nfsrdma' =>
				{ name => "mlnx-nfsrdma", parent => "mlnx-nfsrdma",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'mlnx-nfsrdma-dkms' =>
				{ name => "mlnx-nfsrdma-dkms", parent => "mlnx-nfsrdma",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },
			'mlnx-nfsrdma-modules' =>
				{ name => "mlnx-nfsrdma-modules", parent => "mlnx-nfsrdma",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },

			'mlnx-nvme' =>
				{ name => "mlnx-nvme", parent => "mlnx-nvme",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'mlnx-nvme-dkms' =>
				{ name => "mlnx-nvme-dkms", parent => "mlnx-nvme",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },
			'mlnx-nvme-modules' =>
				{ name => "mlnx-nvme-modules", parent => "mlnx-nvme",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },

			'mlnx-rdma-rxe' =>
				{ name => "mlnx-rdma-rxe", parent => "mlnx-rdma-rxe",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'mlnx-rdma-rxe-dkms' =>
				{ name => "mlnx-rdma-rxe-dkms", parent => "mlnx-rdma-rxe",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },
			'mlnx-rdma-rxe-modules' =>
				{ name => "mlnx-rdma-rxe-modules", parent => "mlnx-rdma-rxe",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts","$mlnX_ofed_kernel","mlnx-ofed-kernel-utils"], configure_options => '' },

			'rshim' =>
				{ name => "rshim", parent => "rshim",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 0, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'rshim-dkms' =>
				{ name => "rshim-dkms", parent => "rshim",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["dkms", "gcc", "make"],
				dist_req_inst => ["dkms", "gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },
			'rshim-modules' =>
				{ name => "rshim-modules", parent => "rshim",
				selected => 0, installed => 0, deb_exist => 0, deb_exist32 => 0,
				available => 1, mode => "kernel",
				dist_req_build => ["gcc", "make"],
				dist_req_inst => ["gcc", "make"],
				ofa_req_build => [],
				ofa_req_inst => [], configure_options => '' },

			'nvme-snap' =>
				{ name => "nvme-snap", parent=> "nvme-snap",
				selected => 0, installed => 0, rpm_exist => 0, rpm_exist32 => 0,
				available => 0, mode => "user",
				dist_req_inst => [],
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "librdmacm-dev"],
				ofa_req_inst => ["libibverbs1", "librdmacm1"], configure_options => '' },

			'ibdump' =>
				{ name => "ibdump", parent => "ibdump",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => ["libstdc++6"],
				dist_req_inst => ["libstdc++6"],
				ofa_req_build => ["libibverbs-dev", "mstflint"],
				ofa_req_inst => ["libibverbs1", "mstflint"],
				configure_options => '' },

			'mlnx-ethtool' =>
				{ name => "mlnx-ethtool", parent => "mlnx-ethtool",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },

			'mlnx-iproute2' =>
				{ name => "mlnx-iproute2", parent => "mlnx-iproute2",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libelf-dev", "libselinux1-dev", "libdb-dev", "libmnl-dev"],
				dist_req_inst => ["libelf1", "libselinux1", "libmnl0"],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },

			'libsdp' =>
				{ name => "libsdp", parent => "libsdp",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'libsdp1' =>
				{ name => "libsdp1", parent => "libsdp",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => ["logrotate"],
				dist_req_inst => ["logrotate"],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },
			'libsdp-dev' =>
				{ name => "libsdp-dev", parent => "libsdp",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => ["logrotate"],
				dist_req_inst => ["logrotate"],
				ofa_req_build => [],
				ofa_req_inst => ["libsdp1"],
				configure_options => '' },

			'sdpnetstat' =>
				{ name => "sdpnetstat", parent => "sdpnetstat",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },

			# this package is listed here only for uninstall and --without.. flag support
			'mlnx-fw-updater' =>
				{ name => "mlnx-fw-updater", parent => "mlnx-fw-updater",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => [],
				ofa_req_inst => [],
				configure_options => '' },

			'hcoll' =>
				{ name => "hcoll", parent => "hcoll",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["gcc", "libstdc++6", "$libssl_devel"],
				dist_req_inst => ["$libssl"],
				ofa_req_build => ["libibverbs-dev", "librdmacm-dev", "$libibmad_dev", "$libibumad_devel", "sharp", "$libibcm_dev"],
				ofa_req_inst => ["libibverbs1", "librdmacm1", "$libibmad", "sharp", "$libibcm1"],
				configure_options => '',
				soft_req => ["sharp"] },
			'sharp' =>
				{ name => "sharp", parent => "sharp",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["gcc", "libstdc++6"],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "$libibumad_devel", "librdmacm-dev", "$libibmad_dev", "ucx"],
				ofa_req_inst => ["libibverbs1", "$libibumad", "librdmacm1", "$libibmad", "ucx"],
				soft_req => ["ucx"],
				configure_options => '' },

			'rdma-core' =>
				{ name => "rdma-core", parent => "rdma-core",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [
					"cmake",
					@rdmacore_python,
					$dh_systemd,
					"dh-python",
					"libnl-3-dev", "libnl-route-3-dev",
					@libsystemd_dev,
					"libudev-dev",
					"pandoc",
					"pkg-config",
					"python3-docutils",
					"valgrind"
				],
				dist_req_inst => ["lsb-base", "libnl-3-200", "udev", "$libudev"],
				ofa_req_build => ["$mlnX_ofed_kernel"],
				ofa_req_inst => ["ofed-scripts"],
				configure_options => '' },
			'ibverbs-providers' =>
				{ name => "ibverbs-providers", parent => "rdma-core",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libnuma-dev"],
				dist_req_inst => ["libnuma1"],
				ofa_req_build => ["rdma-core"],
				ofa_req_inst => ["rdma-core"],
				configure_options => '' },
			'libibumad3' =>
				{ name => "libibumad3", parent => "rdma-core",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libtool"],
				dist_req_inst => [],
				ofa_req_build => ["rdma-core"],
				ofa_req_inst => ["rdma-core"],
				configure_options => '' },
			'libibumad-dev' =>
				{ name => "libibumad-dev", parent => "rdma-core",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libtool"],
				dist_req_inst => [],
				ofa_req_build => ["rdma-core"],
				ofa_req_inst => ["$libibumad", "rdma-core"],
				configure_options => '' },
			'rdmacm-utils' =>
				{ name => "rdmacm-utils", parent => "rdma-core",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => [],
				ofa_req_build => ["rdma-core"],
				ofa_req_inst => ["librdmacm1", "rdma-core"],
				configure_options => '' },
			'python3-pyverbs' =>
				{ name => "python3-pyverbs", parent => "rdma-core",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => [],
				dist_req_inst => ["python3"],
				ofa_req_build => ["rdma-core"],
				ofa_req_inst => ["rdma-core"],
				configure_options => '' },
			'libdisni-java-jni' =>
				{ name => "libdisni-java-jni", parent => "libdisni",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["$openjdk"],
				dist_req_inst => [],
				ofa_req_build => ["librdmacm1", "librdmacm-dev", "libibverbs1", "libibverbs-dev"],
				ofa_req_inst => ["librdmacm1", "libibverbs1"],
				configure_options => '' },
			'mlnx-dpdk' =>
				{ name => "mlnx-dpdk", parent => "mlnx-dpdk",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libcap-dev", "libpcap-dev", "libnuma-dev", "python3", "python3-sphinx", "doxygen", "inkscape", "python3-sphinx-rtd-theme", "texlive-fonts-recommended", "texlive-latex-extra", "hwdata", "graphviz", "dh-python", "rsync"],
				dist_req_inst => ["hwdata", "pciutils", "libnuma1", "libpcap0.8"],
				ofa_req_build => ["libibverbs-dev", "librdmacm-dev"],
				ofa_req_inst => ["libibverbs1", "librdmacm1"],
				configure_options => '' },
			'mlnx-dpdk-dev' =>
				{ name => "mlnx-dpdk-dev", parent => "mlnx-dpdk",
				selected => 0, installed => 0, deb_exist => 0,
				available => 1, mode => "user",
				dist_req_build => ["libcap-dev", "libpcap-dev", "libnuma-dev", "python3", "python3-sphinx"],
				dist_req_inst => [],
				ofa_req_build => ["libibverbs-dev", "librdmacm-dev"],
				ofa_req_inst => ["libibverbs1", "librdmacm1", "mlnx-dpdk"],
				configure_options => '' },
			'mlnx-dpdk-doc' =>
				{ name => "mlnx-dpdk-doc", parent => "mlnx-dpdk",
				selected => 0, installed => 0, deb_exist => 0,
				available => 0, mode => "user",
				dist_req_build => ["libcap-dev", "libpcap-dev", "libnuma-dev", "python3", "python3-sphinx"],
				dist_req_inst => ["libjs-jquery", "libjs-underscore", "python3"],
				ofa_req_build => ["libibverbs-dev", "librdmacm-dev"],
				ofa_req_inst => ["libibverbs1", "librdmacm1"],
				configure_options => '' },
);

for my $ovsp (@openvswitch_packages) {
	$packages_info{$ovsp}{'name'} = $ovsp;
	$packages_info{$ovsp}{'parent'} = "openvswitch";
	$packages_info{$ovsp}{'selected'} = 0;
	$packages_info{$ovsp}{'installed'} = 0;
	$packages_info{$ovsp}{'deb_exist'} = 0;
	$packages_info{$ovsp}{'available'} = 0;
	$packages_info{$ovsp}{'mode'} = 'user';
	$packages_info{$ovsp}{'dist_req_build'} = ["openssl", "$python_conch", "python-zopeinterface", "libunbound-dev", "python-six", "python-all", "libssl-dev"];
	$packages_info{$ovsp}{'dist_req_inst'} = [];
	$packages_info{$ovsp}{'ofa_req_build'} = [];
	$packages_info{$ovsp}{'ofa_req_inst'} = [];
	$packages_info{$ovsp}{'configure_options'} = '';
}

$packages_info{"libopenvswitch"}{'available'} = 1;
$packages_info{"openvswitch-common"}{'available'} = 1;
$packages_info{"openvswitch-switch"}{'available'} = 1;
$packages_info{"libopenvswitch"}{'ofa_req_build'} = ["mlnx-dpdk-dev"];
$packages_info{"libopenvswitch"}{'ofa_req_inst'} = ["mlnx-dpdk"];
$packages_info{"libopenvswitch"}{'dist_req_inst'} = ["openssl", "$libssl", "$libunbound"];
$packages_info{"openvswitch-common"}{'ofa_req_inst'} = ["libopenvswitch"];
$packages_info{"openvswitch-common"}{'dist_req_inst'} = ["$python2", "python-six"];
$packages_info{"openvswitch-switch"}{'ofa_req_inst'} = ["openvswitch-common"];
$packages_info{"openvswitch-switch"}{'dist_req_inst'} = ["uuid-runtime", "python-argparse", "procps", "netbase"];

if ($rdmacore_python[0] eq "python") {
	$packages_info{"python3-pyverbs"}{"available"} = 0;
}

if ($use_upstream_libs) {
	$packages_info{"libibverbs-dev"}{'dist_req_inst'} = ['libnl-3-dev', 'libnl-route-3-dev'];
	$packages_info{"infiniband-diags"}{'dist_req_build'} = [''];
	$packages_info{"infiniband-diags"}{'dist_req_inst'} = [''];
}

###############

sub getch
{
	my $c;
	system("stty -echo raw");
	$c=getc(STDIN);
	system("stty echo -raw");
	# Exit on Ctrl+c or Esc
	if ($c eq "\cC" or $c eq "\e") {
		print "\n";
		exit 1;
	}
	print "$c\n";
	return $c;
}

sub is_installed_deb
{
	my $name = shift @_;

	my $installed_deb = `$DPKG_QUERY -l $name 2> /dev/null | awk '/^[rhi][iU]/{print \$2}'`;

	return ($installed_deb) ? 1 : 0;
}

sub get_all_matching_installed_debs
{
	my $name = shift @_;

	my $installed_debs = `dpkg-query -l "*$name*" 2> /dev/null | awk '/^[rhi][iU]/{print \$2}' | sed -e 's/:.*//g'`;
	return (split "\n", $installed_debs);
}

my %check_uninstall = ();
my %purge_no_deps = ();

# Removes a potential ':<archname>' suffix. e.g.
# 'package:amd64' -> 'package'
sub strip_package_arch($) {
	my $package = shift;
	$package =~ s/:.*//;
	return $package;
}

sub is_immuned($) {
	my $package = shift;
	$package = strip_package_arch($package);
	return exists $purge_no_deps{$package};
}

sub set_immuned($) {
	my $package = shift;
	$package = strip_package_arch($package);
	$purge_no_deps{$package} = 1;
}

sub mark_for_uninstall
{
	my $package = shift @_;

	return if ($package =~ /^xen|ovsvf-config|opensmtpd/);
	return if (is_immuned($package));

	if (not $selected_for_uninstall{$package}) {
		if (is_installed_deb $package) {
			print_and_log("$package will be removed.\n", $verbose2);
			push (@dependant_packages_to_uninstall, "$package");
			$selected_for_uninstall{$package} = 1;
			if (not (exists $packages_info{$package} or $package =~ /mlnx-ofed-/)) {
				$non_ofed_for_uninstall{$package} = 1;
			}
		}
	}
}

sub get_requires
{
	my $package = shift @_;

	chomp $package;

	if ($check_uninstall{$package}) {
		return; # already checked here
	}
	$check_uninstall{$package} = 1;

	if ($package eq "rdma") {
		# don't remove packages that needs rdma package
		return;
	}

	my @what_requires = `/usr/bin/dpkg --purge --dry-run $package 2>&1 | grep "depends on" 2> /dev/null`;

	for my $pack_req (@what_requires) {
		chomp $pack_req;
		$pack_req =~ s/\s*(.+) depends.*/$1/g;
		if (exists $immune_debs{$pack_req}) {
			print_and_log("get_requires: $package is required by $pack_req, but $pack_req won't be removed.\n", $verbose);
			set_immuned($package);
			$check_uninstall{$pack_req} = 1;
			return;
		}
		print_and_log("get_requires: $package is required by $pack_req\n", $verbose2);
		get_requires($pack_req);
		mark_for_uninstall($pack_req);
	}
}

sub is_configured_deb
{
	my $name = shift @_;

	my $installed_deb = `$DPKG_QUERY -l $name 2> /dev/null | awk '/^rc/{print \$2}'`;
	return ($installed_deb) ? 1 : 0;
}

sub ex
{
	my $cmd = shift @_;
	my $sig;
	my $res;

	print_and_log("Running: $cmd\n", $verbose2);
	system("$cmd >> $glog 2>&1");
	$res = $? >> 8;
	$sig = $? & 127;
	if ($sig or $res) {
		print_and_log_colored("Failed command: $cmd", 1, "RED");
		exit 1;
	}
}

sub ex_deb_build
{
	my $name = shift @_;
	my $cmd = shift @_;
	my $sig;
	my $res;

	print_and_log("Running $cmd\n", $verbose);
	system("echo $cmd > $ofedlogs/$name.debbuild.log 2>&1");
	system("$cmd >> $ofedlogs/$name.debbuild.log 2>&1");
	$res = $? >> 8;
	$sig = $? & 127;
	if ($sig or $res) {
		print_and_log_colored("Failed to build $name DEB", 1, "RED");
		addSetupInfo ("$ofedlogs/$name.debbuild.log");
		print_and_log_colored("See $ofedlogs/$name.debbuild.log", 1, "RED");
		exit 1;
	}
}

sub ex_deb_install
{
	my $name = shift @_;
	my $cmd = shift @_;
	my $sig;
	my $res;

	return 0 if ($build_only);

	if (exists $package_pre_install_script{$name}) {
		print_and_log("Running $name pre install script: $package_pre_install_script{$name}\n", $verbose);
		ex1("$package_pre_install_script{$name}");
	}

	print_and_log("Running $cmd\n", $verbose);
	system("echo $cmd > $ofedlogs/$name.debinstall.log 2>&1");
	system("$cmd >> $ofedlogs/$name.debinstall.log 2>&1");
	$res = $? >> 8;
	$sig = $? & 127;
	if ($sig or $res) {
		print_and_log_colored("Failed to install $name DEB", 1, "RED");
		addSetupInfo ("$ofedlogs/$name.debinstall.log");
		print_and_log_colored("See $ofedlogs/$name.debinstall.log", 1, "RED");
		exit 1;
	}

	if (exists $package_post_install_script{$name}) {
		print_and_log("Running $name post install script: $package_post_install_script{$name}\n", $verbose);
		ex1("$package_post_install_script{$name}");
	}
}

sub check_linux_dependencies
{
	my $kernel_dev_missing = 0;
	my %missing_packages = ();

	if (! $check_linux_deps) {
		return 0;
	}

	print_and_log("Checking SW Requirements...\n", (not $quiet));
	foreach my $req_name (@required_debs) {
		my $is_installed_flag = is_installed_deb($req_name);
		if (not $is_installed_flag) {
			print_and_log_colored("$req_name deb is required", $verbose2, "RED");
			$missing_packages{"$req_name"} = 1;
		}
	}

	foreach my $package (@selected_packages) {
		my $pname = $packages_info{$package}{'parent'};
		for my $ver (keys %{$main_packages{$pname}}) {
			if ($package =~ /kernel|knem|ummunotify|mlnx-en/) {
				# kernel sources are required to build mlnx-ofed-kernel
				# require only if with_dkms=1 or (with_dkms=0 and deb is not built)
				if ( not -d "$kernel_sources/scripts" and
						($with_dkms or (not $with_dkms and not is_deb_available("$package")))) {
					print_and_log_colored("$kernel_sources/scripts is required to build $package package.", $verbose2, "RED");
					$missing_packages{"linux-headers-$kernel"} = 1;
					$kernel_dev_missing = 1;
				}
				# from kernel 4.14 we need elf devel package when CONFIG_UNWINDER_ORC=y
				if ( not is_installed_deb($kernel_elfutils_devel) and check_autofconf('CONFIG_STACK_VALIDATION') eq "1" and check_autofconf('CONFIG_UNWINDER_ORC') eq "1" and $kernel =~ /^[5-9]|^4\.[1-9][4-9]\./) {
                                $missing_packages{"$kernel_elfutils_devel"} = 1;
                                print_and_log_colored("$kernel_elfutils_devel is required to build $package RPM.", $verbose2, "RED");
                            }
			}

			# Check rpmbuild requirements
			if (not $packages_info{$package}{$ver}{'deb_exist'}) {
				for my $req ( @{ $packages_info{$package}{'dist_req_build'} } ) {
					print_and_log_colored("$req deb is required to build $package $ver", $verbose2, "RED");
					$missing_packages{"$req"} = 1;
				}
			}

			# Check installation requirements
			for my $req_name ( @{ $packages_info{$package}{'dist_req_inst'} } ) {
                                next if not $req_name;
				my $is_installed_flag = is_installed_deb($req_name);
				if (not $is_installed_flag) {
					print_and_log("$req_name deb is required to install $package $ver", $verbose2, "RED");
					$missing_packages{"$req_name"} = 1;
				}
			}
		}
	}

	# display a summary of missing packages
	if (keys %missing_packages) {
		print_and_log_colored("One or more required packages for installing OFED-internal are missing.", 1, "RED");
		if ($kernel_dev_missing) {
			print_and_log_colored("$kernel_sources/scripts is required for the Installation.", 1, "RED");
		}
		if ($check_deps_only) {
			print_and_log("Run:\napt-get install " . join(' ', (keys %missing_packages)) . "\n", 1);
			exit $PREREQUISIT;
		} else {
			print_and_log_colored("Attempting to install the following missing packages:\n" . join(' ', (keys %missing_packages)), 1, "RED");
			my $cmd = "apt-get install -y " . join(' ', (keys %missing_packages));
			print_and_log("Running: apt-get update\n", $verbose2);
			system("apt-get update >> $glog 2>&1");
			ex "$cmd";
		}
	}

	if ($check_deps_only) {
		print_and_log("All required packages are installed, the system is ready for $PACKAGE installation.\n", 1);
		exit 0;
	}
}

sub get_module_list_from_dkmsConf
{
	my $conf = shift;

	my @modules = ();
	open(IN, "$conf") or print_and_log_colored("Error: cannot open file: $conf", 1, "RED");
	while(<IN>) {
		my $mod = $_;
		chomp $mod;
		if ($mod =~ /BUILT_MODULE_NAME/) {
			$mod =~ s/BUILT_MODULE_NAME\[[0-9]*\]=//g;
			$mod =~ s@^ib_@@g;
			if ($mod =~ /eth_ipoib/) {
				$mod =~ s/eth_ipoib/e_ipoib/g;
			}
			push(@modules, $mod);
		}
	}
	close(IN);
	return @modules;
}

sub is_module_in_deb
{
	my $name = shift;
	my $module = shift;

	my $ret = 0;
	my $deb = "";
	my $commons_dir = "$DEBS/$COMMON_DIR";

	if ($name =~ /mlnx-ofed-kernel/) {
		($deb) = glob ("$commons_dir/$mlnX_ofed_kernel*.deb");
	} elsif ($name =~ /mlnx-en/) {
		($deb) = glob ("$commons_dir/$mlnx_en*.deb");
	} else {
		($deb) = glob ("$commons_dir/$name*.deb");
	}

	if ($deb) {
		if ($module =~ /srp|iser|sdp|rds|nfsrdma|mlnx-nvme|mlnx-rdma-rxe/) {
			return 1;
		}
		rmtree "$builddir/$name\_module-check";
		mkpath "$builddir/$name\_module-check";
		ex "$DPKG_DEB -x $deb $builddir/$name\_module-check 2>/dev/null";
		if (basename($deb) =~ /dkms/) {
			my $conf = `find $builddir/$name\_module-check -name dkms.conf 2>/dev/null | grep -vE "drivers/|net/"`;
			chomp $conf;
			if (grep( /$module.*$/, get_module_list_from_dkmsConf($conf))) {
				print_and_log("is_module_in_deb: $module is in $deb\n", $verbose2);
				$ret = 1;
			} else {
				print_and_log("is_module_in_deb: $module is NOT in $deb\n", $verbose2);
				$ret = 0;
			}
		} else {
			my $modpath = `find $builddir/$name\_module-check -name "*${module}*.ko" 2>/dev/null`;
			chomp $modpath;
			if ($modpath ne "") {
				print_and_log("is_module_in_deb: $module is in $deb\n", $verbose2);
				$ret = 1;
			} else {
				print_and_log("is_module_in_deb: $module is NOT in $deb\n", $verbose2);
				$ret = 0;
			}
		}
		rmtree "$builddir/$name\_module-check";
	} else {
		print_and_log("deb file was not found for pacakge: $name\n", $verbose2);
	}

	return $ret;
}

#
# print usage message
#
sub usage
{
   print GREEN;
   print "\n";
   print "Usage: $0 [-c <packages config_file>|--all|--hpc|--vma|--basic|--bluefield] [OPTIONS]\n";

   print "\n";
   print "Installation control:\n";
   print "    --force              Force installation\n";
   print "    --tmpdir             Change tmp directory. Default: $TMPDIR\n";
   print "    -k|--kernel <version>\n";
   print "                         Default on this system: $kernel (relevant if --without-dkms is given)\n";
   print "    -s|--kernel-sources <path>\n";
   print "                         Default on this system: $kernel_sources (relevant if --without-dkms is given)\n";
   print "    -b|--build-only      Build binary DEBs without installing them (relevant if --without-dkms is given)\n";
   print "                         - This option is supported only when '--kernel-only' option is given.\n";
   print "    --distro             Set Distro name for the running OS (e.g: ubuntu14.04). Default: Use auto-detection.\n";
   print "    --without-depcheck   Run the installation without verifying that all required Distro's packages are installed\n";
   print "    --check-deps-only    Check for missing required Distro's packages and exit\n";
   print "    --force-dkms         Force installing kernel packages with DKMS support\n";
   print "    --without-dkms       Don't install kernel packages with DKMS support\n";
   print "    --builddir           Change build directory. Default: $builddir\n";
   print "    --umad-dev-rw        Grant non root users read/write permission for umad devices instead of default\n";
   print "    --umad-dev-na        Prevent from non root users read/write access for umad devices. Overrides '--umad-dev-rw'\n";
   print "    --enable-mlnx_tune   Enable Running the mlnx_tune utility\n";
   print "    --enable-opensm      Run opensm upon boot\n";
   print "    --without-mlx5-ipsec Disable IPsec support on ConnectX adapters\n";
   print "\n";
   print "    --package-install-options\n";
   print "                         DPKG install options to use when installing DEB packages (comma separated list)\n";
   print "    --pre-build-<package> <path to script>\n";
   print "                         Run given script before given package's build\n";
   print "    --post-build-<package> <path to script>\n";
   print "                         Run given script after given package's build\n";
   print "    --pre-install-<package> <path to script>\n";
   print "                         Run given script before given package's install\n";
   print "    --post-install-<package> <path to script>\n";
   print "                         Run given script after given package's install\n";
   print "\n";
   print "Package selection:\n";
   print "    -c|--config <packages config_file>\n";
   print "                         Example of the config file can be found under docs (ofed.conf-example)\n";
if (not $install_option eq 'eth-only') {
   print "    --all                Install all available packages\n";
   print "    --bluefield          Install BlueField packages\n";
   print "    --hpc                Install minimum packages required for HPC\n";
   print "    --basic              Install minimum packages for basic functionality\n";
} else {
   print "    --eth-only           Install Ethernet drivers only\n";
}
   print "    --dpdk               Install minimum packages required for DPDK\n";
   print "    --ovs-dpdk           Install DPDK and OVS packages\n";
if (not $install_option eq 'eth-only') {
   print "    --with-vma           Enable installing and configuring VMA package (to be used with any of the above installation options)\n";
}
   print "    --vma|--vma-vpi      Install minimum packages required by VMA to support VPI\n";
   print "    --vma-eth            Install minimum packages required by VMA to work over Ethernet\n";
if (not $install_option eq 'eth-only') {
   print "    --guest              Install minimum packages required by guest OS\n";
   print "    --hypervisor         Install minimum packages required by hypervisor OS\n";
   print "    --with-libdisni      Install the libdisni package - Java interface to ib_verbs\n";
   print "                         see https://github.com/zrlio/disni for more information\n";
   print "User-Space and libraries selection:\n";
   print "    --upstream-libs      Install Upstream rdma-core-based libraries\n";
}
   print "Extra package filtering:\n";
if (not $install_option eq 'eth-only') {
   print "    --kernel-only        Install kernel space packages only\n";
   print "    --user-space-only    Filter selected packages and install only User Space packages\n";
}
   print "    --without-<package>  Do not install package\n";
   print "    --with-<package>     Force installing package\n";
   print "    --with-memtrack      Build ofa_kernel RPM with memory tracking enabled for debugging\n";
   print "    --with-cx4lx-optimizations   Build ofa_kernel RPM with ConnectX4 optimizations\n";
   print "    --without-cx4lx-optimizations   Build ofa_kernel RPM with ConnectX4 optimizations (default)\n";
   print "\n";
   print "Miscellaneous:\n";
   print "    -h|--help            Display this help message and exit\n";
   print "    -p|--print-available Print available packages for current platform\n";
   print "                         And create corresponding ofed.conf file\n";
   print "    --conf-dir           Destination directory to save the configuration file. Default: $CWD\n";
   print "\n";
   print "Output control:\n";
   print "    -v|-vv|-vvv          Set verbosity level\n";
   print "    -q                   Set quiet - no messages will be printed\n";
   print RESET "\n\n";
}

sub count_ports
{
	my $cnt = 0;
	open(LSPCI, "/usr/bin/lspci -n|");

	while (<LSPCI>) {
		if (/15b3:6282/) {
			$cnt += 2;  # infinihost iii ex mode
		}
		elsif (/15b3:5e8c|15b3:6274/) {
			$cnt ++;    # infinihost iii lx mode
		}
		elsif (/15b3:5a44|15b3:6278/) {
			$cnt += 2;  # infinihost mode
		}
		elsif (/15b3:6340|15b3:634a|15b3:6354|15b3:6732|15b3:673c|15b3:6746|15b3:6750|15b3:1003/) {
			$cnt += 2;  # connectx
		}
	}
	close (LSPCI);

	return $cnt;
}

# removes the settings for a given interface from /etc/network/interfaces
sub remove_interface_settings
{
	my $interface = shift @_;

	open(IFCONF, $ifconf) or die "Can't open $ifconf: $!";
	my @ifconf_lines;
	while (<IFCONF>) {
		push @ifconf_lines, $_;
	}
	close(IFCONF);

	open(IFCONF, ">$ifconf") or die "Can't open $ifconf: $!";
	my $remove = 0;
	foreach my $line (@ifconf_lines) {
		chomp $line;
		if ($line =~ /(iface|mapping|auto|allow-|source) $interface/) {
			$remove = 1;
		}
		if ($remove and $line =~ /(iface|mapping|auto|allow-|source)/ and $line !~ /$interface/) {
			$remove = 0;
		}
		next if ($remove);
		print IFCONF "$line\n";
	}
	close(IFCONF);
}

sub is_carrier
{
	my $ifcheck = shift @_;
	open(IFSTATUS, "ip link show dev $ifcheck |");
	while ( <IFSTATUS> ) {
		next unless m@(\s$ifcheck).*@;
		if( m/NO-CARRIER/ or not m/UP/ ) {
			close(IFSTATUS);
			return 0;
		}
	}
	close(IFSTATUS);
	return 1;
}

sub config_interface
{
	my $interface = shift @_;
	my $ans;
	my $dev = "ib$interface";
	my $ret;
	my $ip;
	my $nm;
	my $nw;
	my $bc;
	my $onboot = 1;
	my $found_eth_up = 0;
	my $eth_dev;

	if (not $config_net_given) {
		return;
	}
	print "Going to update $dev in $ifconf\n" if ($verbose2);
	if ($ifcfg{$dev}{'LAN_INTERFACE'}) {
		$eth_dev = $ifcfg{$dev}{'LAN_INTERFACE'};
		if (not -e "/sys/class/net/$eth_dev") {
			print "Device $eth_dev is not present\n" if (not $quiet);
			return;
		}
		if ( is_carrier ($eth_dev) ) {
			$found_eth_up = 1;
		}
	}
	else {
		# Take the first existing Eth interface
		my @eth_devs = </sys/class/net/eth*>;
		for my $tmp_dev ( @eth_devs ) {
			$eth_dev = $tmp_dev;
			$eth_dev =~ s@/sys/class/net/@@g;
			if ( is_carrier ($eth_dev) ) {
				$found_eth_up = 1;
				last;
			}
		}
	}

	if ($found_eth_up) {
		get_net_config($eth_dev, \%ifcfg, '');
	}

	if (not $ifcfg{$dev}{'IPADDR'}) {
		print "IP address is not defined for $dev\n" if ($verbose2);
		print "Skipping $dev configuration...\n" if ($verbose2);
		return;
	}
	if (not $ifcfg{$dev}{'NETMASK'}) {
		print "Netmask is not defined for $dev\n" if ($verbose2);
		print "Skipping $dev configuration...\n" if ($verbose2);
		return;
	}
	if (not $ifcfg{$dev}{'NETWORK'}) {
		print "Network is not defined for $dev\n" if ($verbose2);
		print "Skipping $dev configuration...\n" if ($verbose2);
		return;
	}
	if (not $ifcfg{$dev}{'BROADCAST'}) {
		print "Broadcast address is not defined for $dev\n" if ($verbose2);
		print "Skipping $dev configuration...\n" if ($verbose2);
		return;
	}

	my @ipib = (split('\.', $ifcfg{$dev}{'IPADDR'}));
	my @nmib = (split('\.', $ifcfg{$dev}{'NETMASK'}));
	my @nwib = (split('\.', $ifcfg{$dev}{'NETWORK'}));
	my @bcib = (split('\.', $ifcfg{$dev}{'BROADCAST'}));

	my @ipeth = (split('\.', $ifcfg{$eth_dev}{'IPADDR'}));
	my @nmeth = (split('\.', $ifcfg{$eth_dev}{'NETMASK'}));
	my @nweth = (split('\.', $ifcfg{$eth_dev}{'NETWORK'}));
	my @bceth = (split('\.', $ifcfg{$eth_dev}{'BROADCAST'}));

	for (my $i = 0; $i < 4 ; $i ++) {
		if ($ipib[$i] =~ m/\*/) {
			if ($ipeth[$i] =~ m/(\d\d?\d?)/) {
				$ipib[$i] = $ipeth[$i];
			}
			else {
				print "Cannot determine the IP address of the $dev interface\n" if (not $quiet);
				return;
			}
		}
		if ($nmib[$i] =~ m/\*/) {
			if ($nmeth[$i] =~ m/(\d\d?\d?)/) {
				$nmib[$i] = $nmeth[$i];
			}
			else {
				print "Cannot determine the netmask of the $dev interface\n" if (not $quiet);
				return;
			}
		}
		if ($bcib[$i] =~ m/\*/) {
			if ($bceth[$i] =~ m/(\d\d?\d?)/) {
				$bcib[$i] = $bceth[$i];
			}
			else {
				print "Cannot determine the broadcast address of the $dev interface\n" if (not $quiet);
				return;
			}
		}
		if ($nwib[$i] !~ m/(\d\d?\d?)/) {
			$nwib[$i] = $nweth[$i];
		}
	}

	$ip = "$ipib[0].$ipib[1].$ipib[2].$ipib[3]";
	$nm = "$nmib[0].$nmib[1].$nmib[2].$nmib[3]";
	$nw = "$nwib[0].$nwib[1].$nwib[2].$nwib[3]";
	$bc = "$bcib[0].$bcib[1].$bcib[2].$bcib[3]";

	print GREEN "IPoIB configuration for $dev\n";
	if ($onboot) {
		print "auto $dev\n";
	}
	print "iface $dev inet static\n";
	print "address $ip\n";
	print "netmask $nm\n";
	print "network $nw\n";
	print "broadcast $bc\n";
	print RESET "\n";

	# Remove old interface's settings
	remove_interface_settings($dev);

	# append the new interface's settings to the interfaces file
	open(IF, ">>$ifconf") or die "Can't open $ifconf: $!";
	print IF "\n";
	if ($onboot) {
		print IF "auto $dev\n";
	}
	print IF "iface $dev inet static\n";
	print IF "\taddress $ip\n";
	print IF "\tnetmask $nm\n";
	print IF "\tnetwork $nw\n";
	print IF "\tbroadcast $bc\n";
	close(IF);
}

sub ipoib_config
{
	if (not $config_net_given) {
		return;
	}

	my $ports_num = count_ports();
	for (my $i = 0; $i < $ports_num; $i++ ) {
		config_interface($i);
	}
}

sub get_tarball_available
{
	my $name = shift;

	for my $ver (keys %{$main_packages{$name}}) {
		if ($main_packages{$name}{$ver}{'tarballpath'}) {
			return $main_packages{$name}{$ver}{'tarballpath'};
		}
	}

	return "";
}

sub is_tarball_available
{
	my $name = shift;

	for my $ver (keys %{$main_packages{$name}}) {
		if ($main_packages{$name}{$ver}{'tarballpath'}) {
			return 1;
		}
	}

	return 0;
}

sub is_deb_available
{
	my $name = shift;
	for my $ver (keys %{$main_packages{$name}}) {
		if ($main_packages{$name}{$ver}{'debpath'}) {
			return 1;
		}
	}

	return 0;
}

# select packages to install
sub select_packages
{
	my $cnt = 0;
	if ($config_given) {
		open(CONFIG, "$config") || die "Can't open $config: $!";;
		while(<CONFIG>) {
			next if (m@^\s+$|^#.*@);
			my ($package,$selected) = (split '=', $_);
			chomp $package;
			chomp $selected;

			# fix kernel packages names
			# DKMS enabled
			if ($with_dkms) {
				if ($package =~ /-modules/) {
					$package =~ s/-modules/-dkms/g;;
				}
			} else {
			# DKMS disabled
				if ($package =~ /-dkms/) {
					$package =~ s/-dkms/-modules/g;;
				}
			}

			print_and_log("$package=$selected\n", $verbose2);

			if (not $packages_info{$package}{'parent'} or $package =~ /iser|srp$/) {
				my $modules = "@kernel_modules";
				chomp $modules;
				$modules =~ s/ /|/g;
				if ($package =~ m/$modules/) {
					if ( $selected eq 'y' ) {
						if (not $kernel_modules_info{$package}{'available'}) {
							print_and_log("$package is not available on this platform\n", (not $quiet));
						}
						else {
							push (@selected_modules_by_user, $package);
						}
						next if ($package !~ /iser|srp/);
					}
				}
				else {
					print_and_log("Unsupported package: $package\n", (not $quiet));
					next;
				}
			}

			if (not $packages_info{$package}{'available'} and $selected eq 'y') {
				print_and_log("$package is not available on this platform\n", (not $quiet));
				next;
			}

			if ( $selected eq 'y' ) {
				my $parent = $packages_info{$package}{'parent'};
				if (not is_tarball_available($parent)) {
					print_and_log("Unsupported package: $package\n", (not $quiet));
					next;
				}
				push (@selected_by_user, $package);
				print_and_log("select_package: selected $package\n", $verbose2);
				$cnt ++;
			}
		}
	}
	else {
		$config = $conf_dir . "/ofed-$install_option.conf";
		chomp $config;
		open(CONFIG, ">$config") || die "Can't open $config: $!";
		flock CONFIG, $LOCK_EXCLUSIVE;
		if ($install_option eq 'all') {
			for my $package ( @all_packages ) {
				next if (not $packages_info{$package}{'available'});
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		elsif ($install_option eq 'bluefield') {
			for my $package ( @bluefield_packages ) {
				next if (not $packages_info{$package}{'available'});
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @bluefield_kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		elsif ($install_option eq 'hpc') {
			for my $package ( @hpc_packages ) {
				next if (not $packages_info{$package}{'available'});
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @hpc_kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		elsif ($install_option =~ m/vma/) {
			my @list = ();
			if ($install_option eq 'vma') {
				@list = (@vma_packages);
			} elsif ($install_option eq 'vmavpi') {
				@list = (@vmavpi_packages);
			} elsif ($install_option eq 'vmaeth') {
				@list = (@vmaeth_packages);
			}
			for my $package ( @list ) {
				next if (not $packages_info{$package}{'available'});
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @vma_kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		elsif ($install_option eq 'basic') {
			for my $package (@basic_packages) {
				next if (not $packages_info{$package}{'available'});
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @basic_kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		elsif ($install_option eq 'hypervisor') {
			for my $package ( @hypervisor_packages ) {
				next if (not $packages_info{$package}{'available'});
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @hypervisor_kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		elsif ($install_option eq 'guest') {
			for my $package ( @guest_packages ) {
				next if (not $packages_info{$package}{'available'});
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @guest_kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		elsif ($install_option eq 'kernel-only') {
			for my $package ( @all_packages ) {
				next if (not $packages_info{$package}{'available'});
				next if (not $packages_info{$package}{'mode'} eq 'kernel');
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		elsif ($install_option eq 'eth-only') {
			for my $package (@eth_packages) {
				next if (not $packages_info{$package}{'available'});
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @eth_kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		elsif ($install_option =~ m/dpdk/) {
			for my $package ( @dpdk_packages ) {
				next if (not $packages_info{$package}{'available'});
				my $parent = $packages_info{$package}{'parent'};
				next if (not is_tarball_available($parent));
				push (@selected_by_user, $package);
				print CONFIG "$package=y\n";
				$cnt ++;
			}
			for my $module ( @dpdk_kernel_modules ) {
				next if (not $kernel_modules_info{$module}{'available'});
				push (@selected_modules_by_user, $module);
				print CONFIG "$module=y\n";
			}
		}
		else {
			print_and_log_colored("\nUnsupported installation option: $install_option", (not $quiet), "RED");
			exit 1;
		}
	}

	if ($with_ovs_dpdk) {
		for my $package ( @mlnx_dpdk_packages, @openvswitch_packages) {
			next if (grep /^$package$/, @selected_by_user);
			next if (not $packages_info{$package}{'available'});
			my $parent = $packages_info{$package}{'parent'};
			next if (not is_tarball_available($parent));
			push (@selected_by_user, $package);
			print CONFIG "$package=y\n";
			$cnt ++;
		}
	}

	flock CONFIG, $UNLOCK;
	close(CONFIG);

	return $cnt;
}

# It should be possible for the user to pass extra build options
# from outside:
sub add_build_option($) {
	my $option = shift;

	if (exists $ENV{'DEB_BUILD_OPTIONS'}) {
		$ENV{'DEB_BUILD_OPTIONS'} = "$ENV{'DEB_BUILD_OPTIONS'} $option";
	} else {
		$ENV{'DEB_BUILD_OPTIONS'} = $option;
	}
}

# Reset DEB_BUILD_OPTIONS options between builds of different packages.
# Leave it as we got it:
sub reset_build_options(@) {
	return unless (exists $ENV{'DEB_BUILD_OPTIONS'});

	foreach my $option (@_) {
		$ENV{'DEB_BUILD_OPTIONS'} =~ s/$option//;
	}
	if ($ENV{'DEB_BUILD_OPTIONS'} =~ /^ *$/) {
		delete $ENV{'DEB_BUILD_OPTIONS'};
		return;
	}
	$ENV{'DEB_BUILD_OPTIONS'} =~ s/ +/ /g;
}

#
# install selected packages by the user (@selected_packages)
#
sub install_selected
{
	if ($build_only) {
		print_and_log("Installing new packages\n", 1);
	} else {
		print_and_log("Building packages\n", 1);
	}
	my $i = 0;

	chdir $CWD;
	foreach my $name (@selected_packages) {
		delete $ENV{"DEB_CONFIGURE_EXTRA_FLAGS"};
		delete $ENV{"configure_options"};
		delete $ENV{"PACKAGE_VERSION"};
		delete $ENV{"MPI_HOME"};
		delete $ENV{"KNEM_PATH"};
		delete $ENV{"DESTDIR"};
		delete $ENV{"libpath"};
		delete $ENV{"rdmascript"};
		delete $ENV{"CONFIG_ARGS"};
		delete $ENV{"WITH_DKMS"};
		delete $ENV{"MLNX_KO_NO_STRIP"};
		delete $ENV{"kernelver"};
		delete $ENV{"kernel_source_dir"};
		delete $ENV{"KVER"};
		delete $ENV{"K_BUILD"};
		delete $ENV{"MLX4"};
		delete $ENV{"MLX5"};
		delete $ENV{"MLXFW"};
		delete $ENV{"mellanox_autodetect"};
		$BUILD_ENV = '';
		reset_build_options('mlnx_ofed');

		my $parent = $packages_info{$name}{'parent'};
		my $deb_name = $packages_info{$name}{'name'};
		my $gz = get_tarball_available($parent);
		$gz =~ s/.*\/SOURCES/SOURCES/g;
		my $version = $gz;

		$version =~ s/^SOURCES\/${parent}_//;
		$version =~ s/(.orig).*//;
		my @debs = ();
		my $commons_dir = "$DEBS/$COMMON_DIR";
		my $target_subdir = package_subdir($name);
		my $target_dir = "$DEBS/$target_subdir";
		@debs = glob ("$target_dir/${deb_name}[-_]${version}*.deb");
		if ($name =~ /dapl/ or (not @debs and $name =~ /fca|mxm|ucx|openmpi|mpitests|openshmem/)) {
			# TODO: this is neeeded only because of the bad version number in changelog
			@debs = glob ("$target_dir/${deb_name}[-_]*.deb");
		} elsif ($parent =~ /mlnx-ofed-kernel/) {
			if ($with_dkms) {
				@debs = glob ("$commons_dir/${deb_name}[-_]${version}*OFED*.deb");
			} else {
				@debs = glob ("$commons_dir/${deb_name}[-_]${version}-*.kver.${kernel}_*.deb");
				if (not @debs) {
					@debs = glob ("$commons_dir/${deb_name}[-_]${version}-${kernel}_*.deb");
				}
				if (not @debs and $user_space_only) {
					# running user space only with non-dkms mode on a kernel that
					# we don't have bins for. so just take the dkms utils package
					@debs = glob ("$commons_dir/${deb_name}[-_]${version}*OFED*.deb");
				}
			}
		} elsif (not $with_dkms and $parent =~ /iser|srp$|knem|ummunotify|kernel-mft|mlnx-en|mlnx-sdp|mlnx-rds|mlnx-nfsrdma|mlnx-nvme|mlnx-rdma-rxe/) {
			@debs = glob ("$commons_dir/${deb_name}[-_]${version}-*.kver.${kernel}_*.deb");
			if (not @debs) {
				@debs = glob ("$commons_dir/${deb_name}[-_]${version}-${kernel}_*.deb");
			}
		}

		if (not $gz and not @debs) {
			print_and_log("Tarball for $parent and DEBs for $name are missing\n", 1);
			next;
		}

		# check if selected modules are in the found deb file
		if (@debs and ($name =~ /$mlnX_ofed_kernel/)) {
			my $kernel_rpm = "$mlnX_ofed_kernel";
			my $pname = $packages_info{$kernel_rpm}{'parent'};
			for my $ver (keys %{$main_packages{$pname}}) {
				for my $module (@selected_kernel_modules) {
					if (not is_module_in_deb($kernel_rpm, "$module")) {
						@debs = ();
						last;
					}
				}
				if ($with_memtrack) {
					if (not is_module_in_deb($kernel_rpm, "memtrack")) {
						@debs = ();
						last;
					}
				}
			}
		}

		if (not @debs) {
			my $build_args = "";
			print_and_log("Building DEB for ${name}-${version} ($parent)...\n", (not $quiet), 1);
			# Build debs from source
			rmtree "$builddir/$parent";
			mkpath "$builddir/$parent";
			if ($parent =~ /mpitests/) {
				# MPI_HOME directory should be set to corresponding MPI before package build.
				my $openmpiVer = glob ("SOURCES/openmpi_*gz");
				$openmpiVer =~ s/^SOURCES\/openmpi_//;
				$openmpiVer =~ s/(.orig).*//;
				$ENV{"MPI_HOME"} = "/usr/mpi/gcc/openmpi-$openmpiVer";
				$ENV{"DESTDIR"} = "$builddir/$parent/$parent-$version";
			} elsif ($parent =~ /openmpi/) {
				my $config_args = "$packages_info{'openmpi'}{'configure_options'} ";
				my $openmpiVer = glob ("SOURCES/openmpi_*gz");
				$openmpiVer =~ s/^SOURCES\/openmpi_//;
				$openmpiVer =~ s/(.orig).*//;
				$config_args .= " --prefix=/usr/mpi/gcc/openmpi-$openmpiVer";
				$config_args .= " --with-platform=contrib/platform/mellanox/optimized";
				$ENV{"CONFIG_ARGS"} = "$config_args";
				# Let openmpi automatically find plugins and use them
				$ENV{"mellanox_autodetect"} = "yes";
				$BUILD_ENV .= " mellanox_autodetect='yes'";
			} elsif ($parent =~ /openshmem/) {
				my $knemVer = glob ("SOURCES/knem_*gz");
				$knemVer =~ s/^SOURCES\/knem_//;
				$knemVer =~ s/(.orig).*//;
				$ENV{"KNEM_PATH"} = "/opt/knem-$knemVer/";
			} elsif ($parent =~ /hcoll/) {
				my $config_args = "";
				$config_args .= " --with-sharp=/opt/mellanox/sharp" if (-d "/opt/mellanox/sharp");
				$ENV{"CONFIG_ARGS"} = "$config_args";
			} elsif ($parent =~ /ibdump/) {
				if (not $use_upstream_libs) {
					add_build_option('mlnx_ofed');
				}
			} elsif ($parent =~ /mlnx-ofed-kernel/) {
				$kernel_configure_options = "";

				my $CONFIG_XFRM_OFFLOAD = check_autofconf('CONFIG_XFRM_OFFLOAD');
				my $CONFIG_INET_ESP_OFFLOAD = check_autofconf('CONFIG_INET_ESP_OFFLOAD');
				my $CONFIG_INET6_ESP_OFFLOAD = check_autofconf('CONFIG_INET6_ESP_OFFLOAD');

				for my $module ( @selected_kernel_modules ) {
					if ($module eq "core") {
						$kernel_configure_options .= " --with-core-mod --with-user_mad-mod --with-user_access-mod --with-addr_trans-mod";
					}
					elsif ($module eq "ipath") {
						$kernel_configure_options .= " --with-ipath_inf-mod";
					}
					elsif ($module eq "qib") {
						$kernel_configure_options .= " --with-qib-mod";
					}
					elsif ($module eq "srpt") {
						$kernel_configure_options .= " --with-srp-target-mod";
					}
					elsif ($module eq "mlx5_fpga_tools") {
						# Innova/FPGA FLEX supported from kernel 3.10 and up
						$kernel_configure_options .= " --with-innova-flex";

						# Innova/FPGA IPsec supported only in MLNX_OFED, from kernel 4.13 and up
						# and it requires some kernel configs to be enabled
						if ($kernel =~ /^[5-9]|^4\.1[3-9]\./) {
							print_and_log("\n-W- CONFIG_XFRM_OFFLOAD is not enabled in the kernel, Cannot build mlx5_core with Innova support\n", $verbose) if ($CONFIG_XFRM_OFFLOAD ne "1");
							print_and_log("\n-W- None of CONFIG_INET6_ESP_OFFLOAD and CONFIG_INET_ESP_OFFLOAD enabled in the kernel, Cannot build mlx5_core with Innova support\n", $verbose) if ($CONFIG_INET_ESP_OFFLOAD ne "1" and $CONFIG_INET6_ESP_OFFLOAD ne "1");

							if ($CONFIG_XFRM_OFFLOAD eq "1" and
								($CONFIG_INET_ESP_OFFLOAD eq "1" or $CONFIG_INET6_ESP_OFFLOAD eq "1")) {
								$kernel_configure_options .= " --with-innova-ipsec";
							}
						}
					}
					else {
						$kernel_configure_options .= " --with-$module-mod";
						if ($module eq "mlx5" and $with_mlx5_ipsec eq "1") {
							#ConnectX IPsec option.
							if ($CONFIG_XFRM_OFFLOAD eq "1" and
							    $CONFIG_INET_ESP_OFFLOAD eq "1" and $CONFIG_INET6_ESP_OFFLOAD eq "1") {
								$kernel_configure_options .= " --with-mlx5-ipsec";
								print_and_log("\n-W- --with-mlx5-ipsec is enabled\n", $verbose);
							}
						}
						if ($module eq "mlx5" and $with_cx4lx_opt) {
							$kernel_configure_options .= " --with-cx4lx-optimizations";
						}
					}

					if ($arch =~ m/aarch64/ and $kernel =~ /^4\.20/) {
						$kernel_configure_options .= " --with-bf-power-failure-event";
						$kernel_configure_options .= " --with-mlx5-miniflow";
					}
				}
				if ($with_memtrack) {
					$kernel_configure_options .= " --with-memtrack";
				}

				$ENV{"configure_options"} = $kernel_configure_options;
				$ENV{"PACKAGE_VERSION"} = "$version";
			} elsif ($parent eq "ibacm") {
				$ENV{"rdmascript"} = " openibd";
			} elsif ($parent eq "librdmacm") {
				$ENV{"DEB_CONFIGURE_EXTRA_FLAGS"} = " --with-ib_acm";
			} elsif ($parent eq "ibsim") {
				$ENV{"libpath"} = "/usr/lib";
			} elsif ($parent =~ /mlnx-en/) {
				$ENV{"MLX4"} = (grep( /^mlx4$/, @selected_kernel_modules )) ? "1" : "0";
				$ENV{"MLX5"} = (grep( /^mlx5$/, @selected_kernel_modules )) ? "1" : "0";
				$ENV{"MLXFW"} = (grep( /^mlxfw$/, @selected_kernel_modules )) ? "1" : "0";
			} elsif ($parent =~ /mstflint/) {
				$ENV{"DEB_CONFIGURE_EXTRA_FLAGS"} = '--disable-inband';
			} elsif ($parent =~ /libdisni/) {
				my $jdk_home = `readlink -f /usr/bin/java | sed -e "s@/jre/bin/java@@"`;
				$ENV{"JDK_HOME"} = "$jdk_home";
			} elsif ($parent =~ /openvswitch/) {
				if ($python_conch eq "") {
					$build_args .= " --no-check-builddeps";
				}
			}

			if ($parent =~ /^lib/) {
				$BUILD_ENV = " DEB_CFLAGS_SET=\"-g -O3\" DEB_LDFLAGS_SET=\"-g -O3\" DEB_CPPFLAGS_SET=\"-g -O3\" DEB_CXXFLAGS_SET=\"-g -O3\" DEB_FFLAGS_SET=\"-g -O3\" DEB_LDLIBS_SET=\"-g -O3\"";
			}

			chdir  "$builddir/$parent";
			ex "cp $CWD/$gz .";
			ex "tar xzf $CWD/$gz";
			chdir "$parent-$version";

			if (not $with_dkms and $parent =~ /mlnx-ofed-kernel|iser|srp$|knem|ummunotify|kernel-mft|mlnx-en|mlnx-sdp|mlnx-rds|mlnx-nfsrdma|mlnx-nvme|mlnx-rdma-rxe|rshim/) {
				$ENV{"WITH_DKMS"} = "0";
				$ENV{"kernelver"} = "$kernel";
				$ENV{"kernel_source_dir"} = "$kernel_sources";
				$ENV{"KVER"} = "$kernel";
				$ENV{"K_BUILD"} = "$kernel_sources";
				if ($with_kmod_debug_symbols) {
					$ENV{"MLNX_KO_NO_STRIP"} = "1";
				}
				ex "/bin/mv -f debian/control.no_dkms debian/control";
			}

			if (exists $package_pre_build_script{$name}) {
				print_and_log("Running $name pre build script: $package_pre_build_script{$name}\n", $verbose);
				ex1("$package_pre_build_script{$name}");
			}

			ex_deb_build($parent, "$BUILD_ENV $DPKG_BUILDPACKAGE -us -uc $build_args");
			ex "cp ../*.deb $target_dir/";

			if (exists $package_post_build_script{$name}) {
				print_and_log("Running $name post build script: $package_post_build_script{$name}\n", $verbose);
				ex1("$package_post_build_script{$name}");
			}

			@debs = glob ("$target_dir/${deb_name}[-_]${version}*.deb");
			if ($name =~ /dapl/ or (not @debs and $name =~ /fca|mxm|ucx|openmpi|mpitests|openshmem/)) {
				# TODO: this is neeeded only because of the bad version number in changelog
				@debs = glob ("$target_dir/${deb_name}[-_]*.deb");
			} elsif ($parent =~ /mlnx-ofed-kernel/) {
				if ($with_dkms) {
					@debs = glob ("$commons_dir/${deb_name}[-_]${version}*OFED*.deb");
				} else {
					@debs = glob ("$commons_dir/${deb_name}[-_]${version}-*.kver.${kernel}_*.deb");
					if (not @debs) {
						@debs = glob ("$commons_dir/${deb_name}[-_]${version}-${kernel}_*.deb");
					}
				}
			} elsif (not $with_dkms and $parent =~ /iser|srp$|knem|ummunotify|kernel-mft|mlnx-en|mlnx-sdp|mlnx-rds|mlnx-nfsrdma|mlnx-nvme|mlnx-rdma-rxe|rshim/) {
				@debs = glob ("$commons_dir/${deb_name}[-_]${version}-*.kver.${kernel}_*.deb");
				if (not @debs) {
					@debs = glob ("$commons_dir/${deb_name}[-_]${version}-${kernel}_*.deb");
				}
			}
			chdir $CWD;
			rmtree "$builddir/$parent";

			if (not @debs) {
				print_and_log_colored("Error: DEB for $name was not created !", 1, "RED");
				exit 1;
			}
		}

		print_and_log("Installing ${name}-${version}...\n", (not $quiet)) if(not $build_only);
		if ($parent =~ /mlnx-ofed-kernel|libvma/) {
			$ENV{"PACKAGE_VERSION"} = "$version";
			ex_deb_install($name, "$DPKG -i --force-confnew $DPKG_FLAGS @debs");
		} else {
			ex_deb_install($name, "$DPKG -i $DPKG_FLAGS @debs");
		}

		if ($build_only and $name eq "mlnx-ofed-kernel-modules") {
			my $ofa_src = "$builddir/ofed_src";
			print_and_log("Extracting mlnx-ofed-kernel sources to $ofa_src ...\n", 1);
			system("mkdir -p $ofa_src >/dev/null 2>&1");
			my ($kdeb) = glob ("$commons_dir/mlnx-ofed-kernel-modules[-_]${version}-*.kver.${kernel}_*.deb");
			if (not -e "$kdeb") {
				($kdeb) = glob ("$commons_dir/mlnx-ofed-kernel-modules[-_]${version}-${kernel}_*.deb");
			}
			print_and_log("dpkg -x $kdeb $ofa_src >/dev/null \n", 1);
			system("dpkg -x $kdeb $ofa_src >/dev/null");
			$ENV{"OFA_DIR"} = "$ofa_src/usr/src/ofa_kernel";
		}

		# verify that kernel packages were successfuly installed
		if (not $build_only and exists $kernel_packages{"$name"}) {
			system("/sbin/depmod $kernel >/dev/null 2>&1");
			for my $object (@{$kernel_packages{"$name"}{"ko"}}) {
				my $file = `$MODINFO -k $kernel $object 2> /dev/null | grep filename | cut -d ":" -f 2 | sed -s 's/\\s//g'`;
				chomp $file;
				my $origin;
				if (-f $file) {
					$origin = `$DPKG -S $file 2> /dev/null | cut -d ":" -f 1`;
					chomp $origin;
				}
				if (not $file or $origin =~ /$kernel_escaped/) {
					print_and_log_colored("\nError: $name installation failed!", 1, "RED");
					addSetupInfo ("$ofedlogs/$name.debinstall.log");
					print_and_log_colored("See:\n\t$ofedlogs/$name.debinstall.log", 1, "RED");
					my $makelog = `grep "make.log" $ofedlogs/$name.debinstall.log 2>/dev/null`;
					if ($makelog =~ /.*\s(.*make.log)\s.*/) {
						$makelog = $1;
					}
					if (not -f $makelog) {
						my $path = `grep -Ei "/var/lib/dkms/.*build.*for more information" $ofedlogs/$name.debinstall.log 2>/dev/null`;
						if ($path =~ /.*(\/var\/lib\/dkms\/.*build).*/) {
							$makelog = "$1/$makelog";
						}
					}
					if (-f $makelog) {
						system("cp $makelog $ofedlogs/$name.make.log");
						print_and_log_colored("\t$ofedlogs/$name.make.log", 1, "RED");
					}
					print_and_log_colored("Removing newly installed packages...\n", 1, "RED");
					ex "/usr/sbin/ofed_uninstall.sh --force";
					exit 1;
				}
			}
		}
	}
}

sub get_tarball_name_version
{
	my $tarname = shift @_;
	$tarname =~ s@.*/@@g;
	my $name = $tarname;
	$name =~ s/_.*//;
	my $version = $tarname;
	$version =~ s/${name}_//;
	$version =~ s/(.orig).*//;

	return ($name, $version);
}

sub get_deb_name_version
{
	my $debname = shift @_;
	$debname =~ s@.*/@@g;
	my $name = $debname;
	$name =~ s/_.*//;
	my $version = $debname;
	$version =~ s/${name}_//;
	$version =~ s/_.*//;
	$version =~ s/-.*//;# remove release if available

	return ($name, $version);
}

sub get_deb_ver_inst
{
	my $ret;
	$ret = `$DPKG_QUERY -W -f='\${Version}\n' @_ | cut -d ':' -f 2 | uniq`;
	chomp $ret;
	return $ret;
}

sub set_existing_debs
{
	my $glob = "$DEBS/$COMMON_DIR/*.deb";
	if ( $use_upstream_libs) {
		$glob .= " $DEBS/$UPSTREAM_LIBS_DIR/*.deb";
	} else {
		$glob .= " $DEBS/$MLNX_LIBS_DIR/*.deb";
	}
	for my $deb (glob $glob) {
		my ($deb_name, $ver) = get_deb_name_version($deb);
		# skip unrelevnt debs
		if ($deb_name =~ /-modules/ and $deb !~ /-${kernel_escaped}_|\.kver\.${kernel_escaped}_/) {
			next;
		}

		$main_packages{$deb_name}{$ver}{'debpath'} = $deb;
		$packages_info{$deb_name}{$ver}{'deb_exist'} = 1;
		print_and_log("set_existing_debs: $deb_name $ver DEB exist\n", $verbose2);
	}
}

sub set_cfg
{
	my $tarball_full_path = shift @_;

	my ($name, $version) = get_tarball_name_version($tarball_full_path);

	if ($name eq "perftest") {
		if (not ($use_upstream_libs xor ($version =~ /mlnxlibs/))) {
			print_and_log("set_cfg: skip redundant $name tarball " .
			"$tarball_full_path.\n", $verbose3);
			return;
		}
	}

	$main_packages{$name}{$version}{'name'} = $name;
	$main_packages{$name}{$version}{'version'} = $version;
	$main_packages{$name}{$version}{'tarballpath'} = $tarball_full_path;

	print_and_log("set_cfg: " .
	"name: $name, " .
	"version: $main_packages{$name}{$version}{'version'}, " .
	"tarballpath: $main_packages{$name}{$version}{'tarballpath'}\n", $verbose3);
}

# return 0 if pacakge not selected
# return 1 if pacakge selected
sub select_dependent
{
	my $package = shift @_;

	if ($user_space_only and ($packages_info{$package}{'mode'} eq 'kernel')) {
		print_and_log("select_dependent: in user-space-only mode, skipping kernel package: $package\n", $verbose2);
		return 0;
	}

	my $scanned = 0;
	my $pname = $packages_info{$package}{'parent'};
	for my $ver (keys %{$main_packages{$pname}}) {
		$scanned = 1;

		# prevent loop
		if (not exists $packages_info{$package}{'entered_select_dependent'}) {
			$packages_info{$package}{'entered_select_dependent'}  = 1;
		} else {
			return 0 if (not $packages_info{$package}{'available'});
			my $parent = $packages_info{$package}{'parent'};
			return 0 if (not is_tarball_available($parent));
			return 1;
		}

		if ( not $packages_info{$package}{$ver}{'deb_exist'} ) {
			for my $req ( @{ $packages_info{$package}{'ofa_req_build'} } ) {
				next if not $req;
				# W/A for -p option and --user-space-only
				if ($req eq "$mlnX_ofed_kernel" and $print_available) {
					next;
				}
				print_and_log("resolve_dependencies: $package requires $req for deb build\n", $verbose2);
				my $req_selected = 0;
				if ($packages_info{$req}{'available'}) {
					if (not $packages_info{$req}{'selected'}) {
						$req_selected = select_dependent($req);
					} else {
						$req_selected = 1;
					}
				}
				# Check if this is a strict requirment
				if (not $req_selected and not grep( /^$req$/, @{ $packages_info{$package}{'soft_req'} } )) {
					print_and_log("select_dependent: $req requiement not satisfied for $package, skipping it\n", $verbose2);
					$packages_info{$package}{'available'} = 0;
					$packages_info{$pname}{'available'} = 0;
					return 0;
				}
			}
		}

		for my $req ( @{ $packages_info{$package}{'ofa_req_inst'} } ) {
			next if not $req;
			print_and_log("resolve_dependencies: $package requires $req for deb install\n", $verbose2);
			my $req_selected = 0;
			if ($packages_info{$req}{'available'}) {
				if (not $packages_info{$req}{'selected'}) {
					$req_selected = select_dependent($req);
				} else {
					$req_selected = 1;
				}
			}
			if (not $req_selected and not grep( /^$req$/, @{ $packages_info{$package}{'soft_req'} } )) {
				print_and_log("select_dependent: $req requiement not satisfied for $package, skipping it\n", $verbose2);
				$packages_info{$package}{'available'} = 0;
				$packages_info{$pname}{'available'} = 0;
				return 0;
			}
		}

		if (not $packages_info{$package}{'selected'}) {
			return 0 if (not $packages_info{$package}{'available'});
			my $parent = $packages_info{$package}{'parent'};
			return 0 if (not is_tarball_available($parent));
			$packages_info{$package}{'selected'} = 1;
			push (@selected_packages, $package);
			print_and_log("select_dependent: Selected package $package\n", $verbose2);
			return 1;
		}
	}
	if ($scanned eq "0") {
		print_and_log("resolve_dependencies: $package does not exist. Skip dependencies check\n", $verbose2);
	}
	# if we get here, then nothing got selected.
	return 0;
}

sub select_dependent_module
{
	my $module = shift @_;

	if (not $kernel_modules_info{$module}{'available'}) {
		print_and_log("select_dependent_module: $module is not available, skipping it\n", $verbose2);
		return;
	}

	# prevent loop
	if (not exists $kernel_modules_info{$module}{'entered_select_dependent_module'}) {
		$kernel_modules_info{$module}{'entered_select_dependent_module'}  = 1;
	} else {
		return;
	}

	for my $req ( @{ $kernel_modules_info{$module}{'requires'} } ) {
		print_and_log("select_dependent_module: $module requires $req for deb build\n", $verbose2);
		if (not $kernel_modules_info{$req}{'selected'}) {
			select_dependent_module($req);
		}
	}
	if (not $kernel_modules_info{$module}{'selected'}) {
		$kernel_modules_info{$module}{'selected'} = 1;
		push (@selected_kernel_modules, $module);
		print_and_log("select_dependent_module: Selected module $module\n", $verbose2);
	}
}

sub resolve_dependencies
{
	for my $package ( @selected_by_user ) {
		# Get the list of dependencies
		select_dependent($package);
		if (exists $standalone_kernel_modules_info{$package}) {
			for my $mod (@{$standalone_kernel_modules_info{$package}}) {
				if ($kernel_modules_info{$mod}{'available'}) {
					push (@selected_modules_by_user, $mod);
				}
			}
		}
	}

	for my $module ( @selected_modules_by_user ) {
		select_dependent_module($module);
	}

	my $kernel_rpm = "$mlnX_ofed_kernel";
	my $pname = $packages_info{$kernel_rpm}{'parent'};
	for my $ver (keys %{$main_packages{$pname}}) {
		if ($packages_info{$kernel_rpm}{$ver}{'deb_exist'}) {
			for my $module (@selected_kernel_modules) {
				if (not is_module_in_deb($kernel_rpm, "$module")) {
					$packages_info{$kernel_rpm}{$ver}{'deb_exist'} = 0;
					$packages_info{'mlnx-ofed-kernel'}{$ver}{'deb_exist'} = 0;
					last;
				}
			}
			if ($with_memtrack) {
				if (not is_module_in_deb($kernel_rpm, "memtrack")) {
					$packages_info{$kernel_rpm}{$ver}{'deb_exist'} = 0;
					$packages_info{'mlnx-ofed-kernel'}{$ver}{'deb_exist'} = 0;
					last;
				}
			}
		}
	}
}

#
# set opensm service
#
sub set_opensm_service
{
	if ($enable_opensm or $install_option eq 'msm') {
		# Switch on opensmd service
		if (-e "/sbin/chkconfig") {
			system("/sbin/chkconfig --add opensmd > /dev/null 2>&1");
			system("/sbin/chkconfig --set opensmd on > /dev/null 2>&1");
			system("/sbin/chkconfig --level 345 opensmd on > /dev/null 2>&1");
		} elsif (-e "/usr/sbin/update-rc.d") {
			system("/usr/sbin/update-rc.d opensmd defaults > /dev/null 2>&1");
		} else {
			system("/usr/lib/lsb/install_initd /etc/init.d/opensmd > /dev/null 2>&1");
		}
	} else {
		# Switch off opensmd service
		if (-e "/sbin/chkconfig") {
			system("/sbin/chkconfig --del opensmd > /dev/null 2>&1");
			system("/sbin/chkconfig --set opensmd off > /dev/null 2>&1");
			system("/sbin/chkconfig opensmd off > /dev/null 2>&1");
		} elsif (-e "/usr/sbin/update-rc.d") {
			system("/usr/sbin/update-rc.d -f opensmd remove > /dev/null 2>&1");
		} else {
			system("/usr/lib/lsb/remove_initd /etc/init.d/opensmd > /dev/null 2>&1");
		}
	}
}

#
# set vma flags in /etc/modprobe.d/mlnx.conf
#
sub set_vma_flags
{
	return if ($user_space_only);
	my $mlnx_conf = "/etc/modprobe.d/mlnx.conf";
    if ($with_vma and -e "$mlnx_conf") {
        my @lines;
        open(FD, "$mlnx_conf");
        while (<FD>) {
            push @lines, $_;
        }
        close (FD);
        open(FD, ">$mlnx_conf");
        foreach my $line (@lines) {
            chomp $line;
            print FD "$line\n" unless ($line =~ /disable_raw_qp_enforcement|fast_drop|log_num_mgm_entry_size/);
        }
        print FD "options ib_uverbs disable_raw_qp_enforcement=1\n";
        print FD "options mlx4_core fast_drop=1\n";
        print FD "options mlx4_core log_num_mgm_entry_size=-1\n";
        close (FD);
    }

    if (-f "/etc/infiniband/openib.conf") {
        my @lines;
        open(FD, "/etc/infiniband/openib.conf");
        while (<FD>) {
            push @lines, $_;
        }
        close (FD);
        # Do not start SDP
        # Do not start QIB to prevent http://bugs.openfabrics.org/bugzilla/show_bug.cgi?id=2262
        open(FD, ">/etc/infiniband/openib.conf");
        foreach my $line (@lines) {
            chomp $line;
            if ($line =~ m/(^SDP_LOAD=|^QIB_LOAD=).*/) {
                    print FD "${1}no\n";
            } elsif ($line =~ m/(^SET_IPOIB_CM=).*/ and $with_vma) {
                # Set IPoIB Datagram mode in case of VMA installation
                print FD "SET_IPOIB_CM=no\n";
            } else {
                    print FD "$line\n";
            }
        }
        close (FD);
    }
}

sub print_selected
{
	print_and_log_colored("\nBelow is the list of ${PACKAGE} packages that you have chosen
	\r(some may have been added by the installer due to package dependencies):\n", 1, "GREEN");
	for my $package ( @selected_packages ) {
		print_and_log("$package\n", 1);
	}
	print_and_log("\n", 1);
}

sub disable_package
{
    my $key = shift;

    if (exists $packages_info{$key}) {
        $packages_info{$key}{'disable_package'} = 1;
        $packages_info{$key}{'available'} = 0;
        for my $requester (@{$packages_deps{$key}{'required_by'}}) {
            next if (exists $packages_info{$requester}{'disable_package'});
            disable_package($requester);
        }
    }
    # modules
    if (exists $kernel_modules_info{$key}) {
        $kernel_modules_info{$key}{'available'} = 0;
        for my $requester (@{$modules_deps{$key}{'required_by'}}) {
            disable_package($requester);
        }
    }

    if (not (exists $packages_info{$key} or exists $kernel_modules_info{$key})) {
        print_and_log_colored("Unsupported package: $key", (not $quiet), "YELLOW");
    }
}

# The subdirectory a package should reside in
sub package_subdir($) {
    my $rpm_name = shift;

    if ($packages_info{$rpm_name}{'mode'} ne "user" or
    	$rpm_name eq "mlnx-ofed-kernel-utils") { # to match set_availability
        return "COMMON"
    }
    if ($use_upstream_libs) {
        return "UPSTREAM_LIBS";
    } else {
        return "MLNX_LIBS";
    }
}

# used for blocking packages that are replaced with rdma-core and vice versa
sub block_package
{
    my $key = shift;

    if (exists $packages_info{$key}) {
        $packages_info{$key}{'available'} = 0;
        $packages_info{$key}{'disabled'} = 1;
    }
    # modules
    if (exists $kernel_modules_info{$key}) {
        $kernel_modules_info{$key}{'available'} = 0;
        $kernel_modules_info{$key}{'disabled'} = 1;
    }
}

sub enable_package
{
    my $key = shift;

    return unless (exists $packages_info{$key});
    return if (exists $packages_info{$key}{'enabled_package'});

    $packages_info{$key}{'available'} = 1;
    $packages_info{$key}{'enabled_package'} = 1;
    for my $req ( @{ $packages_info{$key}{'ofa_req_inst'} } ) {
        enable_package($req);
    }
}

sub enable_module
{
    my $key = shift;

    if (exists $kernel_modules_info{$key}) {
        $kernel_modules_info{$key}{'available'} = 1;
        for my $req ( @{ $kernel_modules_info{$key}{'requires'} } ) {
            enable_module($req);
        }
    }
}

sub add_enabled_pkgs_by_user
{
    ##############
    # handle with/enable flags
    for my $key ( keys %force_enable_packages ) {
	# fix kernel packages names
	# DKMS enabled
	if ($with_dkms) {
		if ($key =~ /-modules/) {
			$key =~ s/-modules/-dkms/g;;
		}
	} else {
	# DKMS disabled
		if ($key =~ /-dkms/) {
			$key =~ s/-dkms/-modules/g;;
		}
	}

        if (exists $packages_info{$key}) {
            next if ($packages_info{$key}{'disabled'});
            enable_package($key);
            push (@selected_by_user, $key);
        }
        if (exists $kernel_modules_info{$key}) {
            next if ($kernel_modules_info{$key}{'disabled'});
            enable_module($key);
            push (@selected_modules_by_user , $key);
        }

        if (not (exists $packages_info{$key} or exists $kernel_modules_info{$key})) {
            print_and_log_colored("Unsupported package: $key", (not $quiet), "YELLOW");
        }
    }
}

sub check_autofconf
{
	my $VAR = shift;

	my $value = `tac ${kernel_sources}/include/*/autoconf.h 2>/dev/null | grep -m1 ${VAR} 2>/dev/null | sed -ne 's/.*\\\([01]\\\)\$/\\1/gp' 2>/dev/null`;
	chomp $value;
	if ($value eq "") {
		$value = 0;
	}

	return $value;
}

sub set_availability
{
	if ( $distro =~ /debian6/) {
		$packages_info{'openshmem'}{'available'} = 0; # compilation error
	}

	if ($user_space_only) {
		$packages_info{"mlnx-ofed-kernel-utils"}{"mode"} = "user";
	}

	if ($is_bf eq "1") {
		$packages_info{$rshim}{'available'} = 0;
	}

	if ((not $with_libdisni) or $distro =~ /debian[678]|ubuntu1[2-5]/) {
		$packages_info{'libdisni-java-jni'}{'available'} = 0;
	}

	if ($arch =~ /arm|aarch/i) {
		$packages_info{'dapl'}{'available'} = 0;
		$packages_info{'libdapl2'}{'available'} = 0;
		$packages_info{'dapl2-utils'}{'available'} = 0;
		$packages_info{'libdapl-dev'}{'available'} = 0;
		$packages_info{'fca'}{'available'} = 0;
		$packages_info{'mxm'}{'available'} = 0;
	}

	if ($kernel =~ /fbk/ or $arch =~ /arm|aarch/) {
		$kernel_modules_info{'sdp'}{'available'} = 0;
		$packages_info{'libsdp1'}{'available'} = 0;
		$packages_info{'libsdp-dev'}{'available'} = 0;
		$packages_info{'sdpnetstat'}{'available'} = 0;
		$packages_info{'mlnx-sdp'}{'available'} = 0;
		$packages_info{"$mlnx_sdp"}{'available'} = 0;
	}

	if ( not $with_vma or $arch !~ m/x86_64|ppc64|arm|aarch/) {
		$packages_info{'libvma'}{'available'} = 0;
		$packages_info{'libvma-utils'}{'available'} = 0;
		$packages_info{'libvma-dev'}{'available'} = 0;
		$packages_info{'sockperf'}{'available'} = 0;
	}

	if ( $arch =~ m/aarch64/ and $with_bluefield) {
		# VMA is not suported on BF
		$packages_info{'libvma'}{'available'} = 0;
		$packages_info{'libvma-utils'}{'available'} = 0;
		$packages_info{'libvma-dev'}{'available'} = 0;
		$packages_info{'sockperf'}{'available'} = 0;
		if ($distro =~ /ubuntu18.04 | ubuntu20.04/x) {
			$packages_info{'nvme-snap'}{'available'} = 1;
		}
	}

	# turn on isert if we are on follow OS and arch
	if (not ($distro =~ /
			ubuntu16.04 | ubuntu17.10 | ubuntu1[89] | ubuntu2. |
			debian9 | debian1.
		/x and	$kernel =~ /^[5-9] | ^4\.[12][0-9]\. | ^4\.[4-9] | ^3\.1[3-9]/x
	)) {
		$kernel_modules_info{'isert'}{'available'} = 0;
		$packages_info{'isert-dkms'}{'available'} = 0;
		$packages_info{'isert-modules'}{'available'} = 0;
	}

	# disable iproute2 for unsupported OSs
	if ($distro =~ /ubuntu1[45] | debian8/x) {
			$packages_info{'mlnx-iproute2'}{'available'} = 0;
	}

	if (not $with_dkms) {
		# we use only knem-modules when not working with dkms
		$packages_info{'knem'}{'available'} = 0;
	}
	if ($cross_compiling) {
		$packages_info{'knem'}{'available'} = 0;
		$packages_info{'knem-dkms'}{'available'} = 0;
		$packages_info{'knem-modules'}{'available'} = 0;
	}

	if ($distro =~ /ubuntu16.10|ubuntu1[7-8]/) {
		$packages_info{'ibutils'}{'available'} = 0; #compilation failed with GCC6
	}

	if (($arch ne 'x86_64') or ($kernel !~ /
		^4\.15\.0-  # Ubuntu 18.04
		/x)
	) {
		$kernel_modules_info{'nfsrdma'}{'available'} = 0;
		$packages_info{"$mlnx_nfsrdma"}{'available'} = 0;
	}

	if ($kernel !~ /^4\.[8-9] | ^4\.[12][0-9] | ^[5-9]/x) {
		$packages_info{"$mlnx_nvme"}{'available'} = 0;
	}

	my $CONFIG_NET_UDP_TUNNEL = check_autofconf('CONFIG_NET_UDP_TUNNEL');
	if ($kernel !~ /^[5-9]|^4\.[8-9]|^4\.1[0-9]\./ or $CONFIG_NET_UDP_TUNNEL ne "1") {
		$packages_info{"$mlnx_rdma_rxe"}{'available'} = 0;
	}

	# turn off srp and iser if we are not on follow OS and arch
	if (not($distro =~ /
			ubuntu14.04 |
			ubuntu16.04 | ubuntu17.10 | ubuntu1[89] | ubuntu2. |
			debian8\.[7-9] | debian8\.1. | debian9 | debian1.
		/x and	$kernel =~ /^[4-9] | ^3\.1[6-9] | ^3.13.0-/x
	) ) {
		$kernel_modules_info{'srp'}{'available'} = 0;
		$packages_info{'srp'}{'available'} = 0;
		$packages_info{'srp-modules'}{'available'} = 0;
		$packages_info{'srp-dkms'}{'available'} = 0;
		$kernel_modules_info{'iser'}{'available'} = 0;
		$packages_info{'iser'}{'available'} = 0;
	}

	if (not ($kernel =~ /^([4-9]|3\.1[0-9])/)) {
		$kernel_modules_info{'mlx5_fpga_tools'}{'available'} = 0;
	}

	if ($kernel !~ /^4\.|^5\./) {
		$kernel_modules_info{'mdev'}{'available'} = 0;
	}

	# See https://redmine.mellanox.com/issues/1929856
	if ($distro eq 'ubuntu19.10') {
		block_package("ar-mgr");
	}

	if ($distro !~ /debian[5-9] | ubuntu1[2-8]/x) {
		block_package("mxm");
	}

	# make sure user cannot force adding disabled package using --with flag
	if ($use_upstream_libs) {
		block_package("infiniband-diags-compat");
		block_package("infiniband-diags-guest");
		block_package("libmlx4-1");
		block_package("libmlx4-dev");
		block_package("libmlx4-1-dbg");
		block_package("libmlx5-1");
		block_package("libmlx5-dev");
		block_package("libmlx5-1-dbg");
		block_package("librxe-1");
		block_package("librxe-dev");
		block_package("librxe-1-dbg");
		block_package("libibcm");
		block_package("libibcm1");
		block_package("libibcm-dev");
		block_package("ibacm-dev");
		block_package("libibmad-static");
		block_package("libibumad-static");
		block_package("libibcm-dev");
	} else {
		block_package("rdma-core");
		block_package("ibverbs-providers");
		block_package("libibumad3");
		block_package("libibumad-dev");
		block_package("rdmacm-utils");
		block_package("pyhon3-pyverbs");
		block_package("nvme-snap");
	}

    ##############
    # handle without/disable flags
    if (keys %disabled_packages) {
        # build deps list
        for my $pkg (keys %packages_info) {
            for my $req ( @{ $packages_info{$pkg}{'ofa_req_inst'}} , @{ $packages_info{$pkg}{'ofa_req_build'}} ) {
                next if not $req;
                push (@{$packages_deps{$req}{'required_by'}}, $pkg);
            }
        }
        for my $mod (keys %kernel_modules_info) {
            for my $req ( @{ $kernel_modules_info{$mod}{'requires'} } ) {
                next if not $req;
                push (@{$modules_deps{$req}{'required_by'}}, $mod);
            }
        }
        # disable packages
        for my $key ( keys %disabled_packages ) {
            disable_package($key);
        }
    }
    # end of handle without/disable flags

	# keep this at the end of the function.
	add_enabled_pkgs_by_user();
}

sub set_mlnx_tune
{
	# Enable/disable mlnx_tune
	if ( -e "/etc/infiniband/openib.conf") {
		my @lines;
		open(FD, "/etc/infiniband/openib.conf");
		while (<FD>) {
			push @lines, $_;
		}
		close (FD);

		open(FD, ">/etc/infiniband/openib.conf");
		foreach my $line (@lines) {
			chomp $line;
			if ($line =~ m/(^RUN_MLNX_TUNE=).*/) {
				if ($enable_mlnx_tune) {
					print FD "${1}yes\n";
				} else {
					print FD "${1}no\n";
				}
			} else {
					print FD "$line\n";
			}
		}
		close (FD);
	}
}

sub set_umad_permissions
{
	if (-f $ib_udev_rules) {
		open(IB_UDEV_RULES, $ib_udev_rules) or die "Can't open $ib_udev_rules: $!";
		my @ib_udev_rules_lines;
		while (<IB_UDEV_RULES>) {
			push @ib_udev_rules_lines, $_;
		}
		close(IB_UDEV_RULES);

		open(IB_UDEV_RULES, ">$ib_udev_rules") or die "Can't open $ib_udev_rules: $!";
		foreach my $line (@ib_udev_rules_lines) {
			chomp $line;
			if ($line =~ /umad/) {
				if ($umad_dev_na) {
					print IB_UDEV_RULES "KERNEL==\"umad*\", NAME=\"infiniband/%k\", MODE=\"0660\"\n";
				} else {
					print IB_UDEV_RULES "KERNEL==\"umad*\", NAME=\"infiniband/%k\", MODE=\"0666\"\n";
				}
			} else {
				print IB_UDEV_RULES "$line\n";
			}
		}
		close(IB_UDEV_RULES);
	}
}

sub find_misconfigured_packages() {
	my @package_problems = ();
	open(DPKG, "dpkg-query -W -f '\${Status}::\${Package} \${Version}\n' |");
	while (<DPKG>) {
		chomp;
		my ($status_str, $package) = split('::');
		my ($required, $error, $status) = split(/ /, $status_str);
		next if ($status =~ /installed | config-files/x);
		push @package_problems, ("$package:\t$status_str\n");
	}
	close(DPKG);
	return @package_problems;
}

sub addSetupInfo
{
	my $log = shift @_;

	print "Collecting debug info...\n" if (not $quiet);

	if (not open (LOG, ">> $log")) {
		print "-E- Can't open $log for appending!\n";
		return;
	}
	my @package_problems = find_misconfigured_packages();

	print LOG "\n\n\n---------------- START OF DEBUG INFO -------------------\n";
	print LOG "Install command: $CMD\n";

	print LOG "\nVars dump:\n";
	print LOG "- ofedlogs: $ofedlogs\n";
	print LOG "- distro: $distro\n";
	print LOG "- arch: $arch\n";
	print LOG "- kernel: $kernel\n";
	print LOG "- config: $config\n";

	print LOG "\nSetup info:\n";
	print LOG "\n- uname -r: " . `uname -r 2>&1`;
	print LOG "\n- uname -m: " . `uname -m 2>&1`;
	print LOG "\n- lsb_release -a: " . `lsb_release -a 2>&1`;
	print LOG "\n- cat /etc/issue: " . `cat /etc/issue 2>&1`;
	print LOG "\n- cat /proc/version: " . `cat /proc/version 2>&1`;
	print LOG "\n- gcc --version: " . `gcc --version 2>&1`;
	print LOG "\n- lspci -n | grep 15b3: " . `lspci -n 2>&1 | grep 15b3`;
	if (@package_problems) {
		print LOG "\n- Potentiall broken packages:";
		print LOG "\n- ". join("\n- ", @package_problems);
	}
	print LOG "\n- dpkg --list: " . `dpkg --list 2>&1`;

	print LOG "---------------- END OF DEBUG INFO -------------------\n";
	close (LOG);
}

sub ex1
{
    my $cmd = shift @_;

    system("$cmd 2>&1");
    my $res = $? >> 8;
    my $sig = $? & 127;
    if ($sig or $res) {
        print_and_log_colored("Command execution failed: $cmd", 1, "RED");
        exit 1;
    }
}

sub uninstall
{
	return 0 if (not $uninstall);

	print_and_log("Removing old packages\n", 1);
	my $ofed_uninstall = `which ofed_uninstall.sh 2> /dev/null`;
	chomp $ofed_uninstall;
	if (-f "$ofed_uninstall") {
		print_and_log("Uninstalling the previous version of $PACKAGE\n", (not $quiet));
		if ($force) {
				system("yes | ofed_uninstall.sh --force >> $ofedlogs/ofed_uninstall.log 2>&1");
		} else {
				system("yes | ofed_uninstall.sh >> $ofedlogs/ofed_uninstall.log 2>&1");
		}
		my $res = $? >> 8;
		my $sig = $? & 127;
		if ($sig or $res) {
			if ($res == 174) {
				print_and_log("Error: One or more packages depends on MLNX_OFED.\nThese packages should be removed before uninstalling MLNX_OFED:\n", 1);
				system("cat $ofedlogs/ofed_uninstall.log | perl -ne '/Those packages should be/ && do {\$a=1; next}; /To force uninstallation use/ && do {\$a=0}; print if \$a'");
				print_and_log("To force uninstallation use '--force' flag.\n", 1);
				addSetupInfo ("$ofedlogs/ofed_uninstall.log");
				print_and_log_colored("See $ofedlogs/ofed_uninstall.log", 1, "RED");
				exit $NONOFEDRPMS;
			}
			print_and_log_colored("Failed to uninstall the previous installation", 1, "RED");
			addSetupInfo ("$ofedlogs/ofed_uninstall.log");
			print_and_log_colored("See $ofedlogs/ofed_uninstall.log", 1, "RED");
			exit $ERROR;
		}
	}
	my @list_to_remove;
	foreach (@remove_debs){
		next if ($_ =~ /^xen|ovsvf-config|opensmtpd/);
		foreach (get_all_matching_installed_debs($_)) {
			next if ($_ =~ /^xen|ovsvf-config|opensmtpd/);
			if (not $selected_for_uninstall{$_}) {
				my $package = strip_package_arch($_);
				push (@list_to_remove, $_);
				$selected_for_uninstall{$package} = 1;
				if (not (exists $packages_info{$package} or $package =~ /mlnx-ofed-/)) {
					$non_ofed_for_uninstall{$package} = 1;
				}
				get_requires($_);
			}
		}
	}

	if (not $force and keys %non_ofed_for_uninstall) {
		print_and_log("\nError: One or more packages depends on MLNX_OFED.\nThose packages should be removed before uninstalling MLNX_OFED:\n\n", 1);
		print_and_log(join(" ", (keys %non_ofed_for_uninstall)) . "\n\n", 1);
		print_and_log("To force uninstallation use '--force' flag.\n", 1);
		exit $NONOFEDRPMS;
	}

	# verify that dpkg DB is ok
	print_and_log("Running: dpkg --configure -a --force-all\n", $verbose2);
	system("dpkg --configure -a --force-all >> $glog 2>&1");
	print_and_log("Running: apt-get install -f\n", $verbose2);
	system("apt-get install -f -y >> $glog 2>&1");

	my @list_to_remove_all = grep {not is_immuned($_)}
		(@list_to_remove, @dependant_packages_to_uninstall);
	ex "apt-get remove -y @list_to_remove_all" if (scalar(@list_to_remove_all));
	foreach (@list_to_remove_all){
		if (is_configured_deb($_)) {
			if (not /^opensm/) {
				ex "apt-get remove --purge -y $_";
			} else {
				system("apt-get remove --purge -y $_");
			}
		}
	}
	system ("/bin/rm -rf /usr/src/mlnx-ofed-kernel* > /dev/null 2>&1");
}

########
# MAIN #
########
sub main
{
	if (!$install_option) {
		$install_option = 'all';
	}

	if ($config_net_given) {
		if (not -e $config_net) {
			print_and_log_colored("Error: network config_file '$config_net' does not exist!", 1, "RED");
			exit 1;
		}

		open(NET, "$config_net") or die "Can't open $config_net: $!";
		while (<NET>) {
			my ($param, $value) = split('=');
			chomp $param;
			chomp $value;
			my $dev = $param;
			$dev =~ s/(.*)_(ib[0-9]+)/$2/;
			chomp $dev;

			if ($param =~ m/IPADDR/) {
				$ifcfg{$dev}{'IPADDR'} = $value;
			}
			elsif ($param =~ m/NETMASK/) {
				$ifcfg{$dev}{'NETMASK'} = $value;
			}
			elsif ($param =~ m/NETWORK/) {
				$ifcfg{$dev}{'NETWORK'} = $value;
			}
			elsif ($param =~ m/BROADCAST/) {
				$ifcfg{$dev}{'BROADCAST'} = $value;
			}
			elsif ($param =~ m/ONBOOT/) {
				$ifcfg{$dev}{'ONBOOT'} = $value;
			}
			elsif ($param =~ m/LAN_INTERFACE/) {
				$ifcfg{$dev}{'LAN_INTERFACE'} = $value;
			}
			else {
				print_and_log_colored("Unsupported parameter '$param' in $config_net\n", $verbose2, "RED");
			}
		}
		close(NET);
	}

	set_availability();
	for my $tarball ( <$CWD/SOURCES/*> ) {
		set_cfg ($tarball);
	}

	my $num_selected = select_packages();
	set_existing_debs();
	resolve_dependencies();

	${kernel_packages}{$mlnx_en}{'ko'} = [];
	if (grep( /^mlx4$/, @selected_kernel_modules )) {
		push(@{${kernel_packages}{$mlnx_en}{'ko'}}, "mlx4_core");
		push(@{${kernel_packages}{$mlnx_en}{'ko'}}, "mlx4_en");
	}
	if (grep( /^mlx5$/, @selected_kernel_modules )) {
		push(@{${kernel_packages}{$mlnx_en}{'ko'}}, "mlx5_core");
	}

	if (not $num_selected) {
		print_and_log_colored("$num_selected packages selected. Exiting...", 1, "RED");
		exit 1;
	}

	if ($print_available) {
		open(CONFIG, ">$config") || die "Can't open $config: $!";
		flock CONFIG, $LOCK_EXCLUSIVE;
		print "\nOFED packages: ";
		for my $package ( @selected_packages ) {
			my $parent = $packages_info{$package}{'parent'};
			next if (not $packages_info{$package}{'available'} or not is_tarball_available($parent));
			print("$package available: $packages_info{$package}{'available'}\n") if ($verbose2);
			if ($package =~ /$mlnX_ofed_kernel/) {
				print "\nKernel modules: ";
				for my $module ( @selected_kernel_modules ) {
					next if (not $kernel_modules_info{$module}{'available'});
					print $module . ' ';
					print CONFIG "$module=y\n";
				}
				print "\nRPMs: ";
			}
			print $package . ' ';
			print CONFIG "$package=y\n";
		}
		flock CONFIG, $UNLOCK;
		close(CONFIG);
		print GREEN "\nCreated $config", RESET "\n";
		exit $SUCCESS;
	}

	warn("Logs dir: $ofedlogs\n");
	warn("General log file: $glog\n");

	if (not $quiet and not $check_deps_only) {
		print_selected();
	}

	# install required packages
	check_linux_dependencies();

	print "This program will install the $PACKAGE package on your machine.\n"
	    . "Note that all other Mellanox, OEM, OFED, RDMA or Distribution IB packages will be removed.\n"
	    . "Those packages are removed due to conflicts with $PACKAGE, do not reinstall them.\n\n" if (not $quiet);

	uninstall();

	# install new packages chosen by the user
	install_selected();

	if ($build_only) {
		print_and_log_colored("Build passed successfully", (not $quiet), "GREEN");
		return 0;
	}

	if (is_module_in_deb("mlnx-ofed-kernel", "ipoib")) {
		ipoib_config();
	}

	# set vma flags in /etc/modprobe.d/mlnx.conf in case the user chosen to enable vma
	set_vma_flags();

	set_mlnx_tune();

	# set opensm service
	set_opensm_service();

	if ($umad_dev_rw or $umad_dev_na) {
		set_umad_permissions();
	}

        if ( not $quiet ) {
            check_pcie_link();
        }

	print_and_log_colored("Installation passed successfully", (not $quiet), "GREEN");
}

main();

exit $SUCCESS;

