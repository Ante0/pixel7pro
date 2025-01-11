#!/bin/bash

export KERNEL_REPO=..
cd susfs4ksu
cp ./kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch $KERNEL_REPO/KernelSU-Next/
cp ./kernel_patches/50_add_susfs_in_gki-android14-6.1.patch $KERNEL_REPO
cp ./kernel_patches/fs/* $KERNEL_REPO/fs/
cp ./kernel_patches/include/linux/* $KERNEL_REPO/include/linux/
cd $KERNEL_REPO/KernelSU-Next
patch -p1 < 10_enable_susfs_for_ksu.patch
cd $KERNEL_REPO
patch -p1 < 50_add_susfs_in_gki-android14-6.1.patch
