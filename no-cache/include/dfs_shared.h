#ifndef __DFS_SHARED__
#define __DFS_SHARED__

typedef struct dfs_superblock {
  // STUDENT: put superblock internals here
  int valid;
  int fs_blocksize;
  int total_fs_blocks;
  int inode_start_block;
  int num_inodes;
  int fbv_start_block;
} dfs_superblock;

#define DFS_BLOCKSIZE 1024  // Must be an integer multiple of the disk blocksize
#define DFS_MAX_INODES 256 
#define FILE_MAX_FILENAME_LENGTH 72
#define FILE_MAX_READWRITE_BYTES 4096
#define DISK_BLOCKSIZE 256

typedef struct disk_block {
  char data[DISK_BLOCKSIZE]; // This assumes that DISK_BLOCKSIZE is a multiple of 4 (for byte alignment)
} disk_block;

typedef struct dfs_block {
  char data[DFS_BLOCKSIZE];
} dfs_block;

typedef struct dfs_inode {
  // STUDENT: put inode structure internals here
  // IMPORTANT: sizeof(dfs_inode) MUST return 128 in order to fit in enough
  // inodes in the filesystem (and to make your life easier).  To do this, 
  // adjust the maximumm length of the filename until the size of the overall inode 
  // is 128 bytes.
  int inuse;
  int file_size;
  int direct[10];
  int indirect;
  int double_indirect;
  char filename[FILE_MAX_FILENAME_LENGTH];
} dfs_inode;

#define DFS_MAX_FILESYSTEM_SIZE 0x4000000  // 64MB

#define DFS_FAIL -1
#define DFS_SUCCESS 1



#endif
