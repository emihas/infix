#!/bin/sh
# This script can be used to start an Infix OS image in Qemu.  It reads
# either a .config, generated from Config.in, or qemu.cfg from a release
# tarball, for the required configuration data.
#
# Debian/Ubuntu users can change the configuration post-release, install
# the kconfig-frontends package:
#
#    sudo apt install kconfig-frontends
#
# and then call this script with:
#
#    ./qemu.sh -c
#
# To bring up a menuconfig dialog.  Select `Exit` and save the changes.
# For more help, see:_
#
#    ./qemu.sh -h
#

# Local variables
prognm=$(basename "$0")

usage()
{
    echo "usage: $prognm [opts]"
    echo
    echo " -c    Run menuconfig to change Qemu settings"
    echo " -h    This help text"
    echo
    echo "Note: 'kconfig-frontends' package (Debian/Ubuntu) must be installed"
    echo "      for -c to work: sudo apt install kconfig-frontents"

    exit 1
}

die()
{
    echo "$@" >&2
    exit 1
}

load_qemucfg()
{
    local tmp=$(mktemp -p /tmp)

    grep ^CONFIG_QEMU_ $1 >$tmp
    .  $tmp
    rm $tmp

    [ "$CONFIG_QEMU_MACHINE" ] || die "Missing QEMU_MACHINE"
    [ "$CONFIG_QEMU_ROOTFS"  ] || die "Missing QEMU_ROOTFS"

    [ "$CONFIG_QEMU_KERNEL" -a "$CONFIG_QEMU_BIOS" ] \
	&& die "QEMU_KERNEL conflicts with QEMU_BIOS"

    [ ! "$CONFIG_QEMU_KERNEL" -a ! "$CONFIG_QEMU_BIOS" ] \
	&& die "QEMU_KERNEL or QEMU_BIOS must be set"
}

loader_args()
{
    if [ "$CONFIG_QEMU_BIOS" ]; then
	echo -n "-bios $CONFIG_QEMU_BIOS "
    elif [ "$CONFIG_QEMU_KERNEL" ]; then
	echo -n "-kernel $CONFIG_QEMU_KERNEL "
    fi
}

append_args()
{
# Disabled, not needed anymore with virtconsole (hvc0)
#    [ "$CONFIG_QEMU_CONSOLE" ] && echo -n "console=$CONFIG_QEMU_CONSOLE "

    echo -n "console=hvc0 "
    if [ "$CONFIG_QEMU_ROOTFS_INITRD" = "y" ]; then
	# Size of initrd, rounded up to nearest kb
	local size=$((($(stat -c %s $CONFIG_QEMU_ROOTFS) + 1023) >> 10))
	echo -n "root=/dev/ram ramdisk_size=${size} "
    elif [ "$CONFIG_QEMU_ROOTFS_VSCSI" = "y" ]; then
	echo -n "root=PARTLABEL=primary "
    fi

    if [ "$V" != "1" ]; then
	echo -n "quiet "
    else
	echo -n "debug "
    fi

    echo -n "${QEMU_APPEND} ${QEMU_EXTRA_APPEND} "
}

rootfs_args()
{
    if [ "$CONFIG_QEMU_ROOTFS_INITRD" = "y" ]; then
	echo -n "-initrd $CONFIG_QEMU_ROOTFS "
    elif [ "$CONFIG_QEMU_ROOTFS_MMC" = "y" ]; then
	echo -n "-device sdhci-pci "
	echo -n "-device sd-card,drive=mmc "
	echo -n "-drive id=mmc,file=$CONFIG_QEMU_ROOTFS,if=none,format=raw "
    elif [ "$CONFIG_QEMU_ROOTFS_VSCSI" = "y" ]; then
	echo -n "-drive file=$CONFIG_QEMU_ROOTFS,if=virtio,format=raw,bus=0,unit=0 "
    fi
}

rw_args()
{
    [ "$CONFIG_QEMU_RW" ] || return

    if ! [ -f "$CONFIG_QEMU_RW" ]; then
	dd if=/dev/zero of="$CONFIG_QEMU_RW" bs=16M count=1 >/dev/null 2>&1
	mkfs.ext4 -L cfg "$CONFIG_QEMU_RW" >/dev/null 2>&1
    fi

    echo -n "-drive file=$CONFIG_QEMU_RW,if=virtio,format=raw,bus=0,unit=1 "

    if [ "$CONFIG_QEMU_RW_VAR_OPT" ]; then
	if ! [ -f "$CONFIG_QEMU_RW_VAR" ]; then
	    dd if=/dev/zero of="$CONFIG_QEMU_RW_VAR" bs=256M count=1 >/dev/null 2>&1
	    mkfs.ext4 -L var "$CONFIG_QEMU_RW_VAR" >/dev/null 2>&1
	fi

	echo -n "-drive file=$CONFIG_QEMU_RW_VAR,if=virtio,format=raw,bus=0,unit=2 "
    fi
}

host_args()
{
    [ "${QEMU_HOST}" ] || return

    echo -n "-virtfs local,path=${QEMU_HOST},security_model=none,writeout=immediate,mount_tag=hostfs "
}

net_args()
{
    QEMU_NET_MODEL=${QEMU_NET_MODEL:-virtio}

    if [ "$CONFIG_QEMU_NET_BRIDGE" = "y" ]; then
	QEMU_NET_BRIDGE_DEV=${QEMU_NET_BRIDGE_DEV:-virbr0}
	echo -n "-nic bridge,br=$CONFIG_QEMU_NET_BRIDGE_DEV,model=$CONFIG_QEMU_NET_MODEL "
    elif [ "$CONFIG_QEMU_NET_TAP" = "y" ]; then
	QEMU_NET_TAP_N=${QEMU_NET_TAP_N:-1}
	mactab=$(dirname "$CONFIG_QEMU_ROOTFS")/mactab
	rm -f "$mactab"
	for i in $(seq 0 $(($CONFIG_QEMU_NET_TAP_N - 1))); do
	    printf "e$i	52:54:00:12:34:%02x\n" $((0x56 + i)) >>"$mactab"
	    echo -n "-netdev tap,id=nd$i,ifname=qtap$i -device e1000,netdev=nd$i "
	done
	echo -n "-fw_cfg name=opt/mactab,file=$mactab "
    elif [ "$CONFIG_QEMU_NET_USER" = "y" ]; then
	[ "$CONFIG_QEMU_NET_USER_OPTS" ] && QEMU_NET_USER_OPTS="$CONFIG_QEMU_NET_USER_OPTS,"

	echo -n "-nic user,${QEMU_NET_USER_OPTS}model=$CONFIG_QEMU_NET_MODEL "
    else
	echo -n "-nic none"
    fi
}

wdt_args()
{
    echo -n "-device i6300esb -rtc clock=host"
}

run_qemu()
{
    local qemu
    read qemu <<EOF
	$CONFIG_QEMU_MACHINE \
	  -display none -rtc base=utc,clock=vm \
	  -device virtio-serial -chardev stdio,mux=on,id=console0 \
	  -device virtconsole,chardev=console0 -mon chardev=console0 \
	  -chardev socket,id=gdbserver,path=gdbserver.sock,server=on,wait=off \
	  -device virtconsole,name=console1,chardev=gdbserver \
	  $(loader_args) \
	  $(rootfs_args) \
	  $(rw_args) \
	  $(host_args) \
	  $(net_args) \
	  $(wdt_args) \
	  $CONFIG_QEMU_EXTRA
EOF

    if [ "$CONFIG_QEMU_KERNEL" ]; then
	$qemu -append "$(append_args)" "$@"
    else
	$qemu "$@"
    fi
}

dtb_args()
{
    [ "$CONFIG_QEMU_LOADER_UBOOT" ] || return

    if [ "$CONFIG_QEMU_DTB_EXTEND" ]; then
	# On the current architecture, QEMU will generate an internal
	# DT based on the system configuration.

	# So we extract a copy of that
	run_qemu -M dumpdtb=qemu.dtb >/dev/null 2>&1

	# Extend it with the environment and signing information in
	# u-boot.dtb.
	echo "qemu.dtb u-boot.dtb" | \
	    xargs -n 1 dtc -I dtb -O dts | \
	    { echo "/dts-v1/;"; sed  -e 's:/dts-v[0-9]\+/;::'; } | \
	    dtc >qemu-extended.dtb 2>/dev/null

	# And use the combined result to start the instance
	echo -n "-dtb qemu-extended.dtb "
    else
	# Otherwise we just use the unmodified one
	echo -n "-dtb u-boot.dtb "
    fi
}

generate_dot()
{
    [ "$CONFIG_QEMU_NET_TAP" = "y" ] || return

    hostports="<qtap0> qtap0"
    targetports="<e0> e0"
    edges="host:qtap0 -- target:e0;"
    for tap in $(seq 1 $(($CONFIG_QEMU_NET_TAP_N - 1))); do
	hostports="$hostports | <qtap$tap> qtap$tap "
	targetports="$targetports | <e$tap> e$tap "
	edges="$edges host:qtap$tap -- target:e$tap;"
    done
    cat >qemu.dot <<EOF
graph "qemu" {
	layout="neato";
	overlap="false";
	esep="+20";

        node [shape=record, fontname="monospace"];
	edge [color="cornflowerblue", penwidth="2"];

	host [
	    label="host | { $hostports }"
	    pos="0,0!",

	    kind="controller",
	];

        target [
	    label="{ $targetports } | target",
	    pos="10,0!",

	    kind="infix",
	];

	$edges
}
EOF
}

menuconfig()
{
    grep -q QEMU_MACHINE Config.in || die "$prognm: must be run from the output/images directory"
    command -v kconfig-mconf >/dev/null || die "$prognm: cannot find kconfig-mconf for menuconfig"
    exec kconfig-mconf Config.in
}

cd $(dirname $(readlink -f "$0"))

while [ "$1" != "" ]; do
    case $1 in
	-c)
	    menuconfig
	    ;;
	-h)
	    usage
	    ;;
	*)
	    break
    esac
    shift
done

if [ -f .config ]; then
    # Customized settings from 'qemu.sh -c'
    load_qemucfg .config
else
    # Shipped defaults from release tarball
    load_qemucfg qemu.cfg
fi

generate_dot

echo "Starting Qemu  ::  Ctrl-a x -- exit | Ctrl-a c -- toggle console/monitor"
line=$(stty -g)
stty raw
run_qemu $(dtb_args)
stty "$line"

