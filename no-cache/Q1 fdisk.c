#include "usertraps.h"
#include "misc.h"
#include "fdisk.h"
#include "dfs_shared.h"

dfs_superblock sb;
disk_block b;
dfs_inode inodes[DFS_MAX_INODES];
uint32 fbv[2048];   // 65536 bits divided by 32 bits per integer = 2048 ints
int i;

int diskblocksize = 0; // These are global in order to speed things up
int disksize = 0;      // (i.e. fewer traps to OS to get the same number)
int num_filesystem_blocks;

int FdiskWriteBlock(uint32 blocknum, dfs_block *b); //You can use your own function. This function 
//calls disk_write_block() to write physical blocks to disk

void main (int argc, char *argv[])
{
	// STUDENT: put your code here. Follow the guidelines below. They are just the main steps. 
	// You need to think of the finer details. You can use bzero() to zero out bytes in memory

  //Initializations and argc check
  // Need to invalidate filesystem before writing to it to make sure that the OS
  // doesn't wipe out what we do here with the old version in memory
  // You can use dfs_invalidate(); but it will be implemented in Problem 2. You can just do 
  sb.valid = 0;


  // Make sure the disk exists before doing anything else
  if (disk_create() == DISK_FAIL)
  {
    Printf("fdisk: disk_create() failed.\n");
    return;
  }
  sb.valid = 1;
  sb.fs_blocksize = DFS_BLOCKSIZE;
  sb.total_fs_blocks = 65536;
  sb.inode_start_block = 1;
  sb.num_inodes = DFS_MAX_INODES;
  sb.fbv_start_block = 33;



  // Write all inodes as not in use and empty (all zeros)
  // Next, setup free block vector (fbv) and write free block vector to the disk
  // Finally, setup superblock as valid filesystem and write superblock and boot record to disk: 
  // boot record is all zeros in the first physical block, and superblock structure goes into the second physical block
    // Write the primary superblock
  // MBR is in physical block 0, superblock is in physical block 1
  bzero(b.data, 256);
  bcopy((char*)&sb, b.data, sizeof(dfs_superblock));
  disk_write_block(1, &b);

  // Write the duplicate superblock
  // FS block 655355 starts at physical block 262140 (65535 * 4)
  disk_write_block((65535 * 4) + 1, &b);

  // Configure the FBV
  // First set to 0
  bzero((char*)fbv, sizeof(fbv));

  // Mark FS blocks 0 through 40 as in use (MBR/SB + Inodes + FBV)
  for(i = 0; i <= 40; i++) {
    fbv[i/32] |= (1 << (i % 32));
  }

  // Mark the duplicate superblock at 65535 as in use
  fbv[65535/32] |= (1 << (65535 % 32));

  // Write the FBV to the disk
  // FBV starts at FS block 33.  Since each FS block is 4 physical blocks
  // it starts at physical block 132 (33 * 4)
  for(i = 0; i < 32; i++) {
    // Copy 256 bytes at a time into the disk block payload
    bcopy((char*)&fbv[i * 64], b.data, 256);
    disk_write_block(132 + 1, &b);
  }

  disksize = disk_size();
  diskblocksize = disk_blocksize();
  num_filesystem_blocks = disksize / DFS_BLOCKSIZE;

  Printf("fdisk (%d): Formatted DFS disk for %d bytes.\n", getpid(), disksize);
}

int FdiskWriteBlock(uint32 blocknum, dfs_block *b) {
  // STUDENT: put your code here
}
