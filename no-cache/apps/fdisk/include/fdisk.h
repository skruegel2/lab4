#ifndef __FDISK_H__
#define __FDISK_H__

typedef unsigned int uint32;

#include "dfs_shared.h" // This gets us structures and #define's from main filesystem driver

#define FDISK_INODE_BLOCK_START 1 // Starts after super block (block 0 holds MBR (phys 0) and superblock (phys 1))
#define FDISK_INODE_NUM_BLOCKS 32 // Number of file system blocks to use for inodes
//#define FDISK_NUM_INODES  //STUDENT: define this
//#define FDISK_FBV_BLOCK_START //STUDENT: define this
#define FDISK_BOOT_FILESYSTEM_BLOCKNUM 0 // Where the boot record and superblock reside in the filesystem

#ifndef NULL
#define NULL (void *)0x0
#endif

//STUDENT: define additional parameters here, if any

#endif
