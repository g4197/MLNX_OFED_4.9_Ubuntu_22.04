#!/usr/bin/perl
# Copyright (c) 2015 Mellanox Technologies. All rights reserved.
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
#
# Author: Alaa Hleihel - alaa@mellanox.com
#

use strict;
use File::Basename;
use File::Path;
use File::Find;
use File::Copy;
use Cwd;

####################################

my $WDIR    = dirname(`readlink -f $0`);
chdir $WDIR;
require "$WDIR/common_installers.pl";
my $CWD = getcwd;

our (@inbox_obsoleted_packages, @obsolete_for_upstream_libs_mode);

my $mofed = "";
my $RPMS = "";
my $tmpwd = "/tmp";
my $tmpdir = "";
my $logfile = "";
my $topdir = "";
my $output_dir = "";
my $rpms_tdir = "";
my $with_hpc = 0;
my $version = "";
my $ver = "";
my $release = "";
my $arch = "noarch";
my $kernel = "";
my $isDeb = 0;
my %groups_info = ();
my %rpms_info = ();
my $build_num = "";
my %allowed_groups = ();
my %ignore_groups = ();
my $rpm_name_prefix = "mlnx-ofed";
my $ofed_arch =  "";
my $ofed_distro =  "";
my $upstream_libs = 0;
my $origin = "";

# HPC packages (will not be included in the xml file by default)
my %hpc_packages = (
		'mxm' => 1,
		'fca' => 1,
		'mpitests' => 1,
		'openmpi' => 1,
		'openshmem' => 1,
		'bupc' => 1,
		'mpi-selector' => 1,
		'mvapich2' => 1,
		'libibprof' => 1,
		'ummunotify' => 1,
		'ummunotify-mlnx' => 1,
		'hcoll' => 1,
		'knem' => 1,
		'knem-dkms' => 1,
		'knem-modules' => 1,
	);

##
my $xml_header = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
 <comps>\n";
my $xml_groups;
my $xml_footer = "</comps>\n";
##

sub usage
{
	print "\nyum grouplist XML creator for MLNX_OFED.\n";
	print "Usage: $0\n";
	print "\t\t--name       Prefix for packages names (Default: $rpm_name_prefix)\n";
	print "\t\t--mofed      Path to MLNX_OFED directory\n";
	print "\t\t--output     Path to output directory (file name will be always 'comps.xml')\n";
	print "\t\t             default is /$output_dir\n";
	print "\t\t--rpms-tdir  Where to copy the installer rpms/debs.\n";
	print "\t\t--rpms-path  Path to directory containing binaries to scan (Default: autodetect).\n";
	print "\t\t--tmpdir     Temp work directory (Default: /tmp)\n";
	print "\t\t--debian     Build installer for debian (otherwise will build for Linux).\n";
	print "\t\t--kernel     Kernel Version to put in the packages names (used when KMP/DKMS is not supported).\n";
	print "\t\t             Also metadata rpm will select kernel rpms for the given kernel version.\n";
	print "\t\t--build-num  Number to append to the release version number.\n";
	print "\t\t--upstream-libs  Create installers for UPSTREAM_LIBS. (Default: MLNX_LIBS)\n";
	print "\t\t--with-hpc   Include HPC packages in the xml file.\n";
	print "\t\t--groups     Build only the given groups (comma separated list), this option can be repeated.\n";
	print "\t\t--ignore-groups     Ignore the given groups (comma separated list), this option can be repeated.\n";
	print "\tNote: The script expects to find '<group name>_group.txt' files for each needed group under the given RPMS directory.";
	print "\n";
	exit 0;
}

sub ex
{
	my $cmd = shift @_;
	my $sig;
	my $res;
	system("echo -e '\n$cmd' >> $logfile");
	system("$cmd >> $logfile 2>&1");
	$res = $? >> 8;
	$sig = $? & 127;
	if ($sig or $res) {
		print "Failed command: $cmd\n";
		print "See: $logfile\n";
		exit 1;
	}
}

sub get_rpm_name
{
	my $rpm = shift;
	my $name = `rpm -qp --queryformat "[%{NAME}]" $rpm 2>/dev/null`;
	chomp $name;
	return $name;
}

sub get_rpm_version
{
	my $rpm = shift;
	my $ver = `rpm -qp --queryformat "[%{VERSION}]-[%{RELEASE}]" $rpm 2>/dev/null`;
	chomp $ver;
	return $ver;
}

sub get_deb_name
{
	my $rpm = shift;
	my $name = `dpkg --info $rpm 2>/dev/null | grep Package: | cut -d' ' -f'3'`;
	chomp $name;
	return $name;
}

sub get_deb_version
{
	my $rpm = shift;
	my $ver = `dpkg --info $rpm 2>/dev/null | grep Version: | cut -d' ' -f'3'`;
	chomp $ver;
	return $ver;
}

sub init_hash
{
	chdir $RPMS;
	my @files = <*_packages.txt>;
	if (not @files) {
		print STDERR "Error: no packages files were found in '$RPMS' directory!\n";
		usage();
	}

	print("Getting list of packages per group...\n");
	for my $file (@files) {
		open(IN,"$file") or die "Error: Cannot open file: $RPMS/$file";
		my $content = "";
		while (<IN>) {
			chomp $_;
			$content .= $_;
		}
		close(IN);

		my $pkgs;
		if ($content =~ m/RPMs: (.*) .*Created/) {
			$pkgs = $1;
			chomp $pkgs;
		}
		if ($content =~ m/OFED packages: (.*) Kernel modules:/) {
			$pkgs .= " $1";
			chomp $pkgs;
		}
		if ($content =~ m/OFED packages: (.*) .*Created/) {
			$pkgs .= " $1";
			chomp $pkgs;
		}

		chomp $pkgs;
		my @names = split(' ', $pkgs);

		if (not scalar(@names)) {
			next;
		}

		my $group_name = $file;
		$group_name =~ s/_packages.txt//g;

		if (%allowed_groups and
			not exists $allowed_groups{$group_name}) {
			next;
		}
		if (%ignore_groups and
			exists $ignore_groups{$group_name}) {
			next;
		}

		my $group_id = $group_name;
		if ($kernel ne "" and $group_name !~ /user-only/) {
			$group_id = "$group_id-$kernel";
		}
		$group_id =~ s/\s|\.|-/_/g;

		# include hpc packages only if the user used --with-hpc flag
		if (not $with_hpc and $group_id =~ /hpc/) {
			next;
		}

		$groups_info{$group_id}{'name'} = $group_name;
		$groups_info{$group_id}{'desc'} = "MLNX_OFED $group_name installer package $origin";
		if ($group_name =~ /user-only/) {
			$groups_info{$group_id}{'desc'} .= " (User Space packages only)";
		} else {
			if ($isDeb) {
				$groups_info{$group_id}{'desc'} .= ($kernel eq "") ? " (with DKMS support)" : " for kernel $kernel (without DKMS support)";
			} else {
				$groups_info{$group_id}{'desc'} .= ($kernel eq "") ? " (with KMP support)" : " for kernel $kernel (without KMP support)";
			}
			if ($kernel ne "") {
				$groups_info{$group_id}{'name'} = "$groups_info{$group_id}{'name'}-${kernel}";
			}
		}

		for my $name (@names) {
			# include hpc packages only if the user used --with-hpc flag
			if (not $with_hpc and (exists($hpc_packages{$name}) or $name =~ /mpitests/)) {
				next;
			}
			# skip debuginfo rpms, ofed-docs
			if ($name =~ /debuginfo|debugsource|-dbg|ofed-docs/) {
				next;
			}
			# don't add fabric-collector, mlnx-nvme, mlnx-nfsrdma to metadata rpms or yum groups
			if ($name =~ /fabric-collector|mlnx-nfsrdma|mlnx-nvme|libdisni|neohost-backend|mlx-steering-dump/) {
				next;
			}
			# vma rpms should be added only to VMA groups/metadata rpms
			if ($name =~ /vma|sockperf/ and $groups_info{$group_id}{'name'} !~ /vma/i) {
				next;
			}
			# xlio rpms should be added only to xlio groups/metadata rpms
			if ($name =~ /xlio/ and $groups_info{$group_id}{'name'} !~ /xlio/i) {
				next;
			}
			# don't add SRP to metadata rpms or yum groups on PPC64
			if ($name =~ /srp/ and $name !~ /srptools/ and $ofed_arch =~ /ppc64/) {
				next;
			}
			# don't add RSHIM to metadata rpms or yum groups on AARCH64 as conflicts with tmfifo
			if ($name =~ /rshim/ and $ofed_arch =~ /aarch64/) {
				next;
			}
			if ($name =~ /^ucx-(cuda|gdrcopy) | ^hcoll-cuda/x) {
				next;
			}
			$groups_info{$group_id}{'packages'}{$name} = 1;
		}

		# add MFT
		if (not ($group_name =~ /dpdk|kernel-only/)) {
			$groups_info{$group_id}{'packages'}{'mft'} = 1;
			if ($isDeb) {
				$groups_info{$group_id}{'packages'}{'kernel-mft-dkms'} = 1 if ($kernel eq "");
				$groups_info{$group_id}{'packages'}{'kernel-mft-modules'} = 1 if ($kernel ne "");
			}
		}
	}

	print("Getting info about available binaries...");
	if (not $isDeb) {
		# RPM
		$kernel =~ s/-/_/g;
		my @rpm_files = <$RPMS/*rpm>;
		my $has_multiple_devel_packages = 0;
		if (grep /_kernel-devel.*\.kver\./, @rpm_files) {
			$has_multiple_devel_packages = 1;
		}
		for my $rpm (@rpm_files) {
			chomp $rpm;
			my $name = get_rpm_name($rpm);
			my $ver = get_rpm_version($rpm);

			if ($name =~ /kmp|kmod/) {
				next if ($kernel ne "");
				my $pname = $name;
				$pname =~ s/kmod-//g;
				$pname =~ s/-kmp-(default|trace|ppc64|bigsmp|debug|ppc|kdump|kdumppae|smp|vmi|vmipae|xen|xenpae|pae)//g;
				$rpms_info{'kmps'}{"$pname"}{"$name"} = $ver;
				next;
			}
			my $is_rshim_user = 1;
			if (($name =~ /^rshim/) and ($version =~ /^1/)) {
				$is_rshim_user = 0;
			}
			my $kernel_regex = 'kernel|-modules$|';
			my @modules = qw/
				knem iser isert srp kernel-mft gpio-mlxbf
				i2c-mlx mlxbf-livefish mlx-bootctl
				pka-mlxbf tmfifo mlx-trio mlx-l3cache
				mlx-pmc/;
			if (not $is_rshim_user) {
				push @modules, 'rshim';
			}
			$kernel_regex .= join '|', map "^$_\$", @modules;
			my $devel_regex_prefix = "mlnx-ofa_kernel-devel|";
			if ($has_multiple_devel_packages) {
				$devel_regex_prefix = "";
			}
			if ($kernel ne "" and
			    $name =~ /$kernel_regex/ and
			    not ($name =~ /${devel_regex_prefix}mlnx-ofa_kernel$/)) {
				if ($ver !~ /$kernel/) {
					warn "$name $ver <> $kernel\n";
					next;
				}
				if ($has_multiple_devel_packages) {
					$rpms_info{'rpms_kvers'}{"$name"}{"$ver"} = 1;
				}
			} elsif ($kernel eq "" and
				 $name =~ /$kernel_regex/ and not ($name =~ /mlnx-ofa_kernel-devel|mlnx-ofa_kernel$|kmp|kmod/)) {
				next;
			}
			$rpms_info{'rpms'}{"$name"}{"$ver"} = 1;
		}
	} else {
		# Debian
		$kernel =~ s/_/-/g;
		for my $deb (<$RPMS/*deb>) {
			chomp $deb;
			my $name = get_deb_name($deb);
			my $ver = get_deb_version($deb);

			next if ($name =~ /dkms/ and $kernel ne "");
			if ($kernel ne "" and
			    $name =~ /-modules$|mlnx-ofed-kernel-utils|mlnx-en-utils/ and
			    $ver !~ /$kernel$/) {
				next;
			} elsif ($kernel eq "" and
				 $name =~ /-modules/){
				next;
			}
			$rpms_info{'rpms'}{"$name"}{"$ver"} = 1;
		}
	}

	chdir $WDIR;
}

sub create_comps_xml
{
	for my $group_id (keys %groups_info) {
		my $group_name = uc($groups_info{$group_id}{'name'});
		my $xml_section = "\t<group>

		\t\t<id>$group_id</id>
		\t\t<name>MLNX_OFED $group_name</name>
		\t\t<default>true</default>
		\t\t<description>Mellanox OpenFabrics Enterprise Distribution for Linux: MLNX_OFED $group_name packages $origin</description>
		\t\t<uservisible>true</uservisible>
		\t\t<packagelist>\n";
		print ("Creating XML section for: MLNX_OFED $group_name\n");
		# add dependencies
		for my $pname (keys %{$groups_info{$group_id}{'packages'}}) {
			if (exists $rpms_info{'rpms'}{"$pname"} and (not grep(/>$pname</, $xml_section))) {
				$xml_section .= "\t\t\t<packagereq type=\"default\">$pname</packagereq>\n";
			}
			# kmp
			for my $kname (keys %{$rpms_info{'kmps'}{"$pname"}}) {
				next if ($group_id =~ /user_only/);
				if (not grep(/>$kname</, $xml_section)) {
					$xml_section .= "\t\t\t<packagereq type=\"default\">$kname</packagereq>\n";
				}
			}
		}

		# Add the relevant metadata rpm so that it will do the post-install stuff.
		my $mdrpm = "$rpm_name_prefix-$groups_info{$group_id}{'name'}";
		$xml_section .= "\t\t\t<packagereq type=\"default\">$mdrpm</packagereq>\n";

		$xml_section .= "\t\t</packagelist>
		\t</group>\n";
		$xml_groups .= $xml_section;
	}

	# create XML file
	mkpath "$output_dir" unless -d "$output_dir";
	system("rm $output_dir/comps.xml 2>/dev/null") if -f "$output_dir/comps.xml";
	open(OUT, "> $output_dir/comps.xml") or die "Error: unable to open $output_dir/comps.xml for writing!";
	print OUT "$xml_header";
	print OUT "$xml_groups";
	print OUT "$xml_footer";
	print "output was saved in: $output_dir/comps.xml\n";
}

sub get_postinstall_script
{
	my $gname = shift @_;

	my $PACKAGE = "MLNX_OFED_LINUX";
	if ($rpm_name_prefix eq "mlnx-en") {
		$PACKAGE = "mlnx-en";
	}

	my $script = <<EOF;
cd /lib/modules
for dd in \`/bin/ls\`
do
	/sbin/depmod \$dd >/dev/null 2>&1
done

if [ -f /usr/bin/ofed_info ]; then
	sed -i -r -e "s/^(OFED)(.*)(-[0-9]*.*-[0-9]*.*):/${PACKAGE}-${version}-${release} (\\1\\3):\\n/" /usr/bin/ofed_info
	sed -i -r -e "s/(.*then echo) (.*):(.*)/\\1 ${PACKAGE}-${version}-${release}: \\3/" /usr/bin/ofed_info
	sed -i -r -e "s/(.*X-n\\\" \]; then echo) (.*)(; exit.*)/\\1 ${version}-${release} \\3/" /usr/bin/ofed_info
	sed -i -e "s/OFED-internal/${PACKAGE}/g" /usr/bin/ofed_info
fi

EOF

	if ($gname !~ /eth-only/) {
		$script .= <<EOF;
# Switch off opensmd service
/sbin/chkconfig --set opensmd off > /dev/null 2>&1 || true
/sbin/chkconfig opensmd off > /dev/null 2>&1 || true
if [ -f "/etc/init.d/opensmd" ] ; then
	if [ -e /sbin/chkconfig ]; then
	    /sbin/chkconfig --del opensmd > /dev/null 2>&1 || true
	elif [ -e /usr/sbin/update-rc.d ]; then
	    /usr/sbin/update-rc.d -f opensmd remove > /dev/null 2>&1 || true
	else
	    /usr/lib/lsb/remove_initd /etc/init.d/opensmd > /dev/null 2>&1 || true
	fi
fi

# Disable ibacm daemon by default
chkconfig --del ibacm > /dev/null 2>&1 || true

# disable SDP and QIB loading by default
if [ -e /etc/infiniband/openib.conf ]; then
	sed -i -r -e "s/^SDP_LOAD=.*/SDP_LOAD=no/" /etc/infiniband/openib.conf
	sed -i -r -e "s/^QIB_LOAD=.*/QIB_LOAD=no/" /etc/infiniband/openib.conf
fi

/sbin/ldconfig > /dev/null 2>&1 || true

EOF
	}

	# VMA package special conf
	if ($gname =~ /vma/) {
		$script .= <<EOF;
# VMA special module param values
EOF

		if ($ver =~ /^([234]\. | 5\.[01] | 5.2-[01])/x) {
			$script .= <<EOF
mlnx_conf=/etc/modprobe.d/mlnx.conf
if [ -e \$mlnx_conf ]; then
	if ! (grep -qw disable_raw_qp_enforcement \$mlnx_conf 2>/dev/null); then
		echo "options ib_uverbs disable_raw_qp_enforcement=1" >> \$mlnx_conf
	fi
	if ! (grep -qw fast_drop \$mlnx_conf 2>/dev/null); then
		echo "options mlx4_core fast_drop=1" >> \$mlnx_conf
	fi
	if ! (grep -qw log_num_mgm_entry_size \$mlnx_conf 2>/dev/null); then
		echo "options mlx4_core log_num_mgm_entry_size=-1" >> \$mlnx_conf
	fi
fi

EOF
		}
		$script .= <<EOF;
# Set IPoIB Datagram mode in case of VMA installation
if [ -e /etc/infiniband/openib.conf ]; then
	sed -i -r -e "s/^SET_IPOIB_CM=.*/SET_IPOIB_CM=no/" /etc/infiniband/openib.conf
fi

EOF
	}

	return $script;
}

sub create_installer_rpms
{
	for my $group_id (keys %groups_info) {
		my $rpmname = "$rpm_name_prefix-$groups_info{$group_id}{'name'}";
		print ("\nCreating rpm for: $rpmname\n");
		ex "/bin/rm -rf $topdir";
		ex "mkdir -p $topdir/SOURCES/${rpmname}-${version}";
		ex "mkdir -p $topdir/SPECS";
		ex "mkdir -p $topdir/BUILD";
		ex "mkdir -p $topdir/SRPMS";
		ex "mkdir -p $topdir/RPMS";
		open(OUT, ">$topdir/SOURCES/${rpmname}-${version}/${rpmname}-release") or die ("can't create version file!\n");
		print OUT "${rpmname}-${version}\n";
		close(OUT);
		chdir "$topdir/SOURCES";
		ex "tar czf ${rpmname}-${version}.tar.gz ${rpmname}-${version}";
		chdir $WDIR;

		print "Creating spec file: $topdir/SPECS/${rpmname}.spec\n";
		my $spec = <<EOF;
# spec file
Name: ${rpmname}
Version: ${version}
Release: ${release}.${ofed_distro}
Summary: $groups_info{$group_id}{'desc'}
Group: System Environment/Libraries
License: GPLv2 or BSD
Url: http://mellanox.com
Vendor: Mellanox Technologies
Source: http://mellanox.com/\%{name}-\%{version}.tar.gz
BuildRoot: \%(mktemp -ud \%{_tmppath}/\%{name}-\%{version}-\%{release}-XXXXXX)
Obsoletes: libmverbs-headers
Obsoletes: ibutils-libs
Obsoletes: compat-opensm-libs
Obsoletes: rdma
\@OBSOLETES\@
\@REQUIRES\@

\%description
$groups_info{$group_id}{'desc'}

\%prep
\%setup

\%install
install -d \$RPM_BUILD_ROOT/\%{_docdir}/\%{name}
cp \%{name}-release \$RPM_BUILD_ROOT/\%{_docdir}/\%{name}

\%clean
rm -rf \$RPM_BUILD_ROOT

\%post
\@POSTINSTALL\@


\%files
\%defattr(-,root,root,-)
\%{_docdir}

\%changelog
EOF
## End of spec file template

		# add dependencies
		for my $pname (keys %{$groups_info{$group_id}{'packages'}}) {
			if (exists $rpms_info{'rpms'}{"$pname"}) {
				for my $pver (keys %{$rpms_info{'rpms'}{"$pname"}}) {
					my $relation = '>=';
					if (exists $rpms_info{'rpms_kvers'}{$pname}{$pver}) {
						$relation = '=';
					}
					$spec =~ s/\@REQUIRES\@/\@REQUIRES\@\nRequires: $pname $relation $pver/;
				}
			}
			# kmp
			for my $kname (keys %{$rpms_info{'kmps'}{"$pname"}}) {
				next if ($group_id =~ /user_only/);
				$spec =~ s/\@REQUIRES\@/\@REQUIRES\@\nRequires: $kname >= $rpms_info{'kmps'}{"$pname"}{"$kname"}/;
			}
		}
		if ($groups_info{$group_id}{'name'} =~ /^dpdk-upstream-libs$/x) {
				$spec =~ s/\@REQUIRES\@/\@REQUIRES\@\nRequires: mstflint/;
		}
		$spec =~ s/\@REQUIRES\@//;
		if ($groups_info{$group_id}{'name'} =~ /^all$|^all-user-only$|^all-[0-9]/x) {
			for my $item (@inbox_obsoleted_packages) {
				chomp $item;
				next if ($item eq "");
				next if ($item eq "libmlx4" and $version =~ /^4\./);
				if ($upstream_libs and $item =~ /rdma-core|srp_daemon/) {
					next;
				}
				$spec =~ s/\@OBSOLETES\@/obsoletes: $item\n\@OBSOLETES\@/;
			}
		}
		if ($upstream_libs) {
			for my $item (@obsolete_for_upstream_libs_mode) {
				chomp $item;
				next if ($item eq "");
				$spec =~ s/\@OBSOLETES\@/obsoletes: $item\n\@OBSOLETES\@/;
			}
		}
		$spec =~ s/\@OBSOLETES\@//;

		# add post install script
		my $pscript = get_postinstall_script($groups_info{$group_id}{'name'});
		$spec =~ s/\@POSTINSTALL\@/$pscript/;

		open(OUT, ">$topdir/SPECS/${rpmname}.spec") or die ("can't create spec file!\n");
		print OUT "$spec";
		close(OUT);

		ex "rpmbuild -ba --target noarch --define '_topdir $topdir' --define '_sourcedir %{_topdir}/SOURCES' --define '_specdir %{_topdir}/SPECS' --define '_srcrpmdir %{_topdir}/SRPMS' --define '_rpmdir %{_topdir}/RPMS' $topdir/SPECS/${rpmname}.spec";
		mkpath "$rpms_tdir" unless -d "$rpms_tdir";
		ex "/bin/cp -af $topdir/RPMS/$arch/${rpmname}* $rpms_tdir";
		print "Built $rpmname rpm\n";
	}
}

sub create_installer_debs
{
	for my $group_id (keys %groups_info) {
		my $rpmname = "$rpm_name_prefix-$groups_info{$group_id}{'name'}";
		print ("\nCreating deb for: $rpmname\n");
		ex "/bin/rm -rf $topdir";
		ex "mkdir -p $topdir/${rpmname}-${version}/debian/source";
		open(OUT, ">$topdir/${rpmname}-${version}/${rpmname}-release") or die ("can't create version file!\n");
		print OUT "${rpmname}-${version}\n";
		close(OUT);

		### rules file
		my $content = <<EOF;
#!/usr/bin/make -f
# -*- makefile -*-

export DH_OPTIONS
pname:=$rpmname

%:
	dh \$@

override_dh_auto_install:

	dh_installdirs -p\$(pname) usr/share/doc/\$(pname)
	install -m 0644 \$(pname)-release debian/\$(pname)/usr/share/doc/\$(pname)
EOF
		my $fname = "$topdir/${rpmname}-${version}/debian/rules";
		open(OUT, ">$fname") or die ("can't create $fname file!\n");
		print OUT "$content";
		close(OUT);
        ex "chmod 755 $topdir/${rpmname}-${version}/debian/rules";

		### compat file
		$content = <<EOF;
9
EOF
		$fname = "$topdir/${rpmname}-${version}/debian/compat";
		open(OUT, ">$fname") or die ("can't create $fname file!\n");
		print OUT "$content";
		close(OUT);

		### source/format file
		$content = <<EOF;
3.0 (quilt)
EOF
		$fname = "$topdir/${rpmname}-${version}/debian/source/format";
		open(OUT, ">$fname") or die ("can't create $fname file!\n");
		print OUT "$content";
		close(OUT);

		### changelog file
		$content = <<EOF;
$rpmname ($version-$release) unstable; urgency=low

  * Initial release

 -- Alaa Hleihel <alaa\@mellanox.com>  Wed, 06 Aug 2014 21:00:00 +0200
EOF
		$fname = "$topdir/${rpmname}-${version}/debian/changelog";
		open(OUT, ">$fname") or die ("can't create $fname file!\n");
		print OUT "$content";
		close(OUT);

		### post install script file
		# add post install script
		my $pscript = get_postinstall_script($groups_info{$group_id}{'name'});
		$content = <<EOF;
#!/bin/bash
$pscript
EOF
		$fname = "$topdir/${rpmname}-${version}/debian/${rpmname}.postinst";
		open(OUT, ">$fname") or die ("can't create $fname file!\n");
		print OUT "$content";
		close(OUT);
        ex "chmod 755 $topdir/${rpmname}-${version}/debian/${rpmname}.postinst";

		### control file
		$content = <<EOF;
Source: $rpmname
Section: utils
Priority: extra
Maintainer: Alaa Hleihel <alaa\@mellanox.com>
Build-Depends: debhelper (>= 9.0.0)
Standards-Version: 3.9.2
Homepage: http://www.mellanox.com

Package:$rpmname
Architecture: all
Depends: \${shlibs:Depends}, \${misc:Depends}\@REQUIRES\@
Description: $groups_info{$group_id}{'desc'}
EOF

		if ($groups_info{$group_id}{'name'} =~ /^(basic | all)$/x) {
			my $rpmname_exact = "$rpm_name_prefix-$groups_info{$group_id}{'name'}-exact";
			$content .= <<EOF;

Package:$rpmname_exact
Architecture: all
Depends: \${shlibs:Depends}, \${misc:Depends}\@REQUIRES_EXACT\@
Description: $groups_info{$group_id}{'desc'} (exact)
 This is a meta-package that imposes strict dependencies: depends on
 exact version numbers of its dependencies.
EOF

		}
		# add dependencies
		for my $pname (keys %{$groups_info{$group_id}{'packages'}}) {
			if (exists $rpms_info{'rpms'}{"$pname"}) {
				for my $pver (keys %{$rpms_info{'rpms'}{"$pname"}}) {
					$content =~ s/\@REQUIRES\@/, $pname (>=$pver)\@REQUIRES\@/;
					$content =~ s/\@REQUIRES_EXACT\@/, $pname (= $pver)\@REQUIRES_EXACT\@/;
				}
			}
		}
		$content =~ s/\@REQUIRES\@//;
		$content =~ s/\@REQUIRES_EXACT\@//;
		$content =~ s/\@OBSOLETES\@//;
		$fname = "$topdir/${rpmname}-${version}/debian/control";
		open(OUT, ">$fname") or die ("can't create $fname file!\n");
		print OUT "$content";
		close(OUT);

		my $year = (localtime)[5]; $year += 1900;
		$fname = "$topdir/${rpmname}-${version}/debian/copyright";
		open(OUT, ">$fname") or die ("can't create $fname file: $!\n");
		print OUT <<EOF;
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/

Files: *
Copyright: $year, NVIDIA Corporation
License: GPLv2-and-2BSD
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 .
 On Debian systems, a copy of the General Public License version 2 could
 be found at /usr/share/common-licenses/GPL-2
EOF
		close(OUT);
		chdir "$topdir";
		ex "tar czf ${rpmname}_${version}.orig.tar.gz ${rpmname}-${version}";
		chdir "$topdir/${rpmname}-${version}";
		ex "dpkg-buildpackage -us -uc";
		mkpath "$rpms_tdir" unless -d "$rpms_tdir";
		ex "/bin/cp -af $topdir/*.deb $rpms_tdir";
		print "Built $rpmname deb\n";
		chdir $WDIR;
	}
}

##############################################
## Main

while ( $#ARGV >= 0 ) {

	my $cmd_flag = shift(@ARGV);

	if ($cmd_flag eq "--mofed") {
		$mofed = shift(@ARGV);
	} elsif ($cmd_flag eq "--name") {
		$rpm_name_prefix = shift(@ARGV);
	} elsif ($cmd_flag eq "--output") {
		$output_dir = shift(@ARGV);
	} elsif ($cmd_flag eq "--rpms-tdir") {
		$rpms_tdir = shift(@ARGV);
	} elsif ($cmd_flag eq "--rpms-path") {
		$RPMS = shift(@ARGV);
	} elsif ($cmd_flag eq "--with-hpc") {
		$with_hpc = 1;
	} elsif ($cmd_flag eq "--debian") {
		$isDeb = 1;
	} elsif ($cmd_flag eq "--tmpdir") {
		$tmpwd = shift(@ARGV);
	} elsif ($cmd_flag eq "--kernel") {
		$kernel = shift(@ARGV);
	} elsif ($cmd_flag eq "--build-num") {
		$build_num = shift(@ARGV);
	} elsif ($cmd_flag eq "--upstream-libs") {
		$upstream_libs = 1;
	} elsif ($cmd_flag eq "--groups") {
		my $groups = shift(@ARGV);
		chomp $groups;
		for my $item (split(',', $groups)) {
			$allowed_groups{$item} = 1;
		}
	} elsif ($cmd_flag eq "--ignore-groups") {
		my $groups = shift(@ARGV);
		chomp $groups;
		for my $item (split(',', $groups)) {
			$ignore_groups{$item} = 1;
		}
	} else {
		&usage();
		exit 1;
	}
}
if (not -d "$mofed" ) {
	print "--mofed parameter is required!\n";
	usage();
}
if ($RPMS eq "") {
	if (not $isDeb) {
		$RPMS = "$mofed/RPMS" if (-d "$mofed/RPMS");
	} else {
		$RPMS = "$mofed/DEBS" if (-d "$mofed/DEBS");
	}
	if (-d "$RPMS/COMMON" ) {
		if ($upstream_libs) {
			$RPMS .= "/UPSTREAM_LIBS";
			$origin = "(UPSTREAM_LIBS)";
		} else {
			$RPMS .= "/MLNX_LIBS";
			$origin = "(MLNX_LIBS)";
		}
	}
}
if (! -d $RPMS) {
	print STDERR "-E- '$RPMS' dir does not exist!\n";
	exit 1;
}

if (-f "$mofed/.mlnx") {
	$ver = `cat $mofed/.mlnx`;
} elsif (-f "$mofed/mlnx") {
	$ver = `cat $mofed/mlnx`;
} else {
	print STDERR "-E- $mofed is not a supported MLNX_OFED dir!\n";
	exit 1;
}
chomp $ver;
($version, $release) = (split("-", $ver));
if (-z "$version" or -z "$release") {
	print STDERR "-E- failed to get version!\n";
	exit 1;
}
$version =~ s/_/./g;
$release =~ s/_/./g;
if ($build_num ne "") {
	$release = "${release}.${build_num}";
}

if (-f "$mofed/.arch") {
	$ofed_arch = `cat $mofed/.arch`;
} elsif (-f "$mofed/arch") {
	$ofed_arch = `cat $mofed/arch`;
} else {
	print STDERR "-E- $mofed missing arch file !\n";
	exit 1;
}
chomp $ofed_arch;

if (-f "$mofed/distro") {
	$ofed_distro = `cat $mofed/distro | sed -e "s/-/./g"`;
} else {
	print STDERR "-E- $mofed missing distro file !\n";
	exit 1;
}
chomp $ofed_distro;

$tmpdir = `mktemp -d $tmpwd/mlnx.XXXXXXX` or die("Failed to create temp directory");
chomp $tmpdir;
$topdir = "$tmpdir/build";
$logfile = "$tmpdir/log.txt";
$output_dir = "$tmpwd/mlnxcomps" if ($output_dir eq "");
$rpms_tdir = "$tmpwd/tmp/mlnxcomps/rpms" if ($rpms_tdir eq "");

init_hash();

if (not $isDeb) {
	create_comps_xml();
	create_installer_rpms();
} else {
	create_installer_debs();
}

system("/bin/rm -rf $tmpdir >/dev/null 2>&1");
exit 0;
