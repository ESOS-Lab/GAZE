# SPDX-License-Identifier: GPL-2.0
# ScaleLFS, imported from /home/aec/ScaleLFS (out-of-tree module) into the
# GAZE tree. Built whenever CONFIG_F3FS_FS is enabled (same switch as the
# fs/pgc baseline -- both are renamed-f2fs baselines and share the F3FS_*
# feature Kconfig symbols). Registers filesystem type "scalelfs".
#
# mkfs for this filesystem MUST be the traditional ScaleLFS f2fs-tools
# (/home/aec/f2fs-tools), NOT kernel/f2fs-tools-1.15.0 (GAZE's zoned one).
# See experiment_script/general_resource/setup_fs.sh.
obj-m += scalelfs.o

ccflags-y += -UCONFIG_DEBUG_FS

scalelfs-y		:= dir.o file.o inode.o namei.o hash.o super.o inline.o
scalelfs-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o
scalelfs-y		+= shrinker.o extent_cache.o sysfs.o lockfree_list.o
scalelfs-$(CONFIG_F3FS_STAT_FS) += debug.o
scalelfs-$(CONFIG_F3FS_FS_XATTR) += xattr.o
scalelfs-$(CONFIG_F3FS_FS_POSIX_ACL) += acl.o
scalelfs-$(CONFIG_FS_VERITY) += verity.o
scalelfs-$(CONFIG_F3FS_FS_COMPRESSION) += compress.o
scalelfs-$(CONFIG_F3FS_IOSTAT) += iostat.o

default:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean
