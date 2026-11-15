#include "usertraps.h"
#include "misc.h"
#include "fdisk.h"
#include "dfs_shared.h"

int diskblocksize = 0; // These are global in order to speed things up
int disksize = 0;      // (i.e. fewer traps to OS to get the same number)

int FdiskWriteBlock(uint32 blocknum, dfs_block *b); //You can use your own function. This function 
//calls disk_write_block() to write physical blocks to disk

void main (int argc, char *argv[])
{
  dfs_superblock sb;
  dfs_block b;
  dfs_inode inodes[DFS_MAX_INODES];
  uint32 fbv[2048];   // 65536 bits divided by 32 bits per integer = 2048 ints
  int i;
  int num_filesystem_blocks;

  // Make sure the disk exists before doing anything else
  if (disk_create() == DISK_FAIL)
  {
    Printf("fdisk: disk_create() failed.\n");
    return;
  }

  // Initialize disk properties using traps
  disksize = disk_size();
  diskblocksize = disk_blocksize();
  num_filesystem_blocks = disksize / DFS_BLOCKSIZE;

  if (disksize == 0) {
    Printf("fdisk: disk size returned 0.  Ensure disk_create was called.\n");
    return;
  }

  // Invalidate the filesystem before writing to it
  sb.valid = 0;

  // Write all inodes as not in use and empty
  bzero((char*)inodes, sizeof(inodes));
  for(i = 0; i < 32; i++) {
    bzero(b.data, DFS_BLOCKSIZE);
    bcopy((char*)&inodes[i * (DFS_BLOCKSIZE / sizeof(dfs_inode))], b.data, DFS_BLOCKSIZE);
    FdiskWriteBlock(i + 1, &b); // DFS blocks 33 through 40    
  }

  // Setup fbv
  bzero((char*)fbv, sizeof(fbv));

  // Mark DFS blocks 0 through 40 as in-use (Superblock/MBR + Inodes + FBV)
  for(i = 0; i <= 40; i++) {
    fbv[i/32] |= (1 << (i % 32));
  }

  // Mark the duplicate superblock at 65535 as in use
  fbv[65535/32] |= (1 << (65535 % 32));

  // Write the FBV to the disk
  // 65536 bits / 8 = 8192 bytes 8 DFS blocks
  for(i = 0; i < 8; i++) {
    bzero(b.data, DFS_BLOCKSIZE);
    bcopy((char*)&fbv[i * (DFS_BLOCKSIZE / sizeof(uint32))], b.data, DFS_BLOCKSIZE);
    FdiskWriteBlock(33 + i, &b);  // DFS blocks 33 through 40    
  }   

  // Setup superblock as valid filesystem
  sb.valid = 1;
  sb.fs_blocksize = DFS_BLOCKSIZE;
  sb.total_fs_blocks = 65536;
  sb.inode_start_block = 1;
  sb.num_inodes = DFS_MAX_INODES;
  sb.fbv_start_block = 33;

  // Write superblock and boot record to disk
  // DFS block 0 maps to physical blocks 0, 1, 2, and 3
  bzero(b.data, DFS_BLOCKSIZE);

  // Boot record is all zeros in first physical block
  // Superblock structure goes into second physical block
  bcopy((char*)&sb, b.data + diskblocksize, sizeof(dfs_superblock));

  // Write to primary location (DFS block 0)
  FdiskWriteBlock(0, &b);

  // Write the duplicate superblock
  FdiskWriteBlock(65535, &b);

  Printf("fdisk (%d): Formatted DFS disk for %d bytes.\n", getpid(), disksize);
}

int FdiskWriteBlock(uint32 blocknum, dfs_block *b) {
  // STUDENT: put your code here
  int i;
  int phys_blocks_per_dfs = DFS_BLOCKSIZE / diskblocksize;
  int phys_block_start = blocknum * phys_blocks_per_dfs;

  // A single DFS block spans multiple physical disk blocks
  for(i = 0; i < phys_blocks_per_dfs; i++) {
    // Write physical block size (256 bytes) in chunks to the disk using the user trap
    disk_write_block(phys_block_start + i, b->data + (i * diskblocksize));
  }
  return 0;
}
