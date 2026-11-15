#include "ostraps.h"
#include "dlxos.h"
#include "traps.h"
#include "queue.h"
#include "disk.h"
#include "dfs.h"
#include "synch.h"
#include "dfs_shared.h"

static dfs_inode inodes[ DFS_MAX_INODES ]; // all inodes
static dfs_superblock sb; // superblock
static uint32 fbv[2048]; // Free block vector 65536 / 32 = 2048

static uint32 negativeone = 0xFFFFFFFF;
static inline uint32 invert(uint32 n) { return n ^ negativeone; }

// You have already been told about the most likely places where you should use locks. You may use 
// additional locks if it is really necessary.

static uint32 dfs_lock;

// STUDENT: put your file system level functions below.
// Some skeletons are provided. You can implement additional functions.

///////////////////////////////////////////////////////////////////
// Non-inode functions first
///////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------
// DfsModuleInit is called at boot time to initialize things and
// open the file system for use.
//-----------------------------------------------------------------

void DfsModuleInit() {
// You essentially set the file system as invalid and then open 
// using DfsOpenFileSystem().
  // Initialize the lock to protect the free block vector and inodes
  dfs_lock = LockCreate();

  // Set file system as invalid, then open
  DfsInvalidate();

  // Load the file system into memory
  DfsOpenFileSystem();

}

//-----------------------------------------------------------------
// DfsInavlidate marks the current version of the filesystem in
// memory as invalid.  This is really only useful when formatting
// the disk, to prevent the current memory version from overwriting
// what you already have on the disk when the OS exits.
//-----------------------------------------------------------------

void DfsInvalidate() {
// This is just a one-line function which sets the valid bit of the 
// superblock to 0.
  sb.valid = 0;
}

//-------------------------------------------------------------------
// DfsOpenFileSystem loads the file system metadata from the disk
// into memory.  Returns DFS_SUCCESS on success, and DFS_FAIL on 
// failure.
//-------------------------------------------------------------------
int DfsOpenFileSystem() {
  disk_block b;
  int i;
  uint32 phys_block_ratio;
  uint32 phys_block_start;
  uint32 inode_phys_blocks;
  uint32 fbv_phys_blocks;
  char* inode_ptr;
  char* fbv_ptr;

  // Check that filesystem is not already open
  if (sb.valid == 1) {
    return DFS_FAIL;
  }
  // Read superblock from disk.  Note this is using the disk read rather 
  // than the DFS read function because the DFS read requires a valid 
  // filesystem in memory already, and the filesystem cannot be valid 
  // until we read the superblock. Also, we don't know the block size 
  // until we read the superblock, either.

  // Read superblock from disk
  if (DiskReadBlock(1, &b) == DISK_FAIL)
  {
    return DFS_FAIL;
  }

  // Copy the data from the block we just read into the superblock in memory
  bcopy(b.data, (char*)&sb, sizeof(dfs_superblock));

  // Calculate conversion ratio from DFS blocks to Physical Blocks
  phys_block_ratio = sb.fs_blocksize / DiskBytesPerBlock();

  // All other blocks are sized by virtual block size:
  // Read inodes
  inode_ptr = (char*)inodes;
  phys_block_start = sb.inode_start_block * phys_block_ratio;

  // Calculate how many physical blocks it takes to store all inodes
  inode_phys_blocks = (sb.num_inodes * sizeof(dfs_inode)) / DiskBytesPerBlock();

  for (i = 0; i < inode_phys_blocks; i++) {
    if (DiskReadBlock(phys_block_start + i, &b) == DISK_FAIL) {
      return DFS_FAIL;
    }
    bcopy(b.data, inode_ptr + (i * DiskBytesPerBlock()), DiskBytesPerBlock());
  }

  // Read free block vector
  fbv_ptr = (char*)fbv;
  phys_block_start = sb.fbv_start_block * phys_block_ratio;
  // FBV contains 1 bit per DFS block.  Convert to bytes, then to physical blocks.
  fbv_phys_blocks = (sb.total_fs_blocks / 8) / DiskBytesPerBlock();

  for (i = 0; i < fbv_phys_blocks; i++)
  {
    if (DiskReadBlock(phys_block_start + i, &b) == DISK_FAIL) {
      return DFS_FAIL;
    }
    bcopy(b.data, fbv_ptr + (i * DiskBytesPerBlock()), DiskBytesPerBlock());
  }

  // Change superblock to be invalid, write back to disk
  sb.valid = 0;
  bzero(b.data, DiskBytesPerBlock());
  bcopy((char*)&sb, b.data, sizeof(dfs_superblock));

  // Write it to primary physical block 1
  DiskWriteBlock(1, &b);

  // Write to duplicate superblock (offset 1 in FS block 65535)
  DiskWriteBlock((65535 * phys_block_ratio) + 1, &b);

  // Change it back to be valid in memory
  sb.valid = 1;

  return DFS_SUCCESS;
}

//-------------------------------------------------------------------
// DfsCloseFileSystem writes the current memory version of the
// filesystem metadata to the disk, and invalidates the memory's 
// version.
//-------------------------------------------------------------------
int DfsCloseFileSystem() {
  disk_block b;
  int i;
  uint32 phys_block_ratio;
  uint32 phys_block_start;
  uint32 inode_phys_blocks;
  uint32 fbv_phys_blocks;
  char* inode_ptr;
  char* fbv_ptr;

  if (sb.valid == 0) {
    return DFS_FAIL;
  }

  phys_block_ratio = sb.fs_blocksize / DiskBytesPerBlock();

  // Write inodes back to disk
  inode_ptr = (char*)inodes;
  phys_block_start = sb.inode_start_block * phys_block_ratio;
  inode_phys_blocks = (sb.num_inodes * sizeof(dfs_inode)) / DiskBytesPerBlock();

  for(i = 0; i < inode_phys_blocks; i++)
  {
    bcopy(inode_ptr + (i * DiskBytesPerBlock()), b.data, DiskBytesPerBlock());
    if (DiskWriteBlock(phys_block_start + i, &b) == DISK_FAIL) {
      return DFS_FAIL;
    }
  }

  // Write Free Block Vector back to disk
  fbv_ptr = (char*)fbv;
  phys_block_start = sb.fbv_start_block * phys_block_ratio;
  fbv_phys_blocks = (sb.total_fs_blocks / 8) / DiskBytesPerBlock();

  for(i = 0; i < fbv_phys_blocks; i++) {
    bcopy(fbv_ptr + (i * DiskBytesPerBlock()), b.data, DiskBytesPerBlock());
    if (DiskWriteBlock(phys_block_start + i, &b) == DISK_FAIL) {
      return DFS_FAIL;
    }
  }

  // Set memory copy to valid and write valid superblock to disk
  sb.valid = 1;
  bzero(b.data, DiskBytesPerBlock());
  bcopy((char*)&sb, b.data, sizeof(dfs_superblock));

  // Write to primary physical block 1
  DiskWriteBlock(1, &b);

  // Write to duplicate superblock (offset 1 in FS Block 65535)
  DiskWriteBlock((65535 * phys_block_ratio) + 1, &b);

  // Invalidate memory copy after successful flush
  sb.valid = 0;

  return DFS_SUCCESS;  
}


//-----------------------------------------------------------------
// DfsAllocateBlock allocates a DFS block for use. Remember to use 
// locks where necessary.
//-----------------------------------------------------------------

int DfsAllocateBlock() {
// Check that file system has been validly loaded into memory
// Find the first free block using the free block vector (FBV), mark it in use
// Return handle to block
  int i;
  int j;
  uint32 blocknum = DFS_FAIL;
  uint32 mask;
  uint32 total_fbv_ints;

  // Check that file system had been loaded correctly into memory
  if (sb.valid == 0) {
    return DFS_FAIL;    
  }

  // Find the first free block using the fbv, mark it in use
  total_fbv_ints = sb.total_fs_blocks / 32;

  // Acquire lock before reading the shared fbv
  LockAcquire(dfs_lock);

  for (i = 0; i < total_fbv_ints; i++) {
    // If the integer is not completely full of 1s
    if (fbv[i] != 0xFFFFFFFF) {
      // Find the exact bit that is 0
      for (j = 0; j < 32; j++) {
        mask = (1 << j);
        if ((fbv[i] & mask) == 0) {
          // Bit is 0 (free).  Mark it as 1 (in-use)
          fbv[i] |= mask;
          blocknum = (i * 32) + j;
          break;
        }
      }
      // Since we found and allocated a block, break out of outer loop
      if (blocknum != DFS_FAIL) {
        break;
      }
    }
  }

  // Release the lock after modifying
  LockRelease(dfs_lock);

  // Return handle or DFS_FAIL
  return blocknum;
}


//-----------------------------------------------------------------
// DfsFreeBlock deallocates a DFS block.
//-----------------------------------------------------------------

int DfsFreeBlock(uint32 blocknum) {
  uint32 array_index;
  uint32 bit_index;
  uint32 mask;

  // Check that the file system has been correctly loaded into memory
  if (sb.valid == 0) {
    return DFS_FAIL;
  }

  // Ensure the block number is within valid bounds
  if (blocknum >= sb.total_fs_blocks) {
    return DFS_FAIL;
  }

  array_index = blocknum / 32;
  bit_index = blocknum % 32;

  // Create a mask with a 0 at the target bits and 1 everywhere else
  // Use provided invert() function
  mask = invert(1 << bit_index);

  // Acquire lock before modifying fbv
  LockAcquire(dfs_lock);

  // Use bitwise AND to clear the specific bit to 0 (free)
  fbv[array_index] &= mask;

  // Release lock
  LockRelease(dfs_lock);

  return DFS_SUCCESS;
}


//-----------------------------------------------------------------
// DfsReadBlock reads an allocated DFS block from the disk
// (which could span multiple physical disk blocks).  The block
// must be allocated in order to read from it.  Returns DFS_FAIL
// on failure, and the number of bytes read on success.  
//-----------------------------------------------------------------
int DfsReadBlock(uint32 blocknum, dfs_block *b) {
  disk_block phys_b;
  uint32 phys_block_ratio;
  uint32 phys_block_start;
  int i;

  // Check that the file system is valid
  if (sb.valid == 0) {
    return DFS_FAIL;
  }

  // A single DFS block spans multiple physical blocks
  phys_block_ratio = sb.fs_blocksize / DiskBytesPerBlock();
  phys_block_start = blocknum * phys_block_ratio;  

  // Read each physical block and copy its payload into the correct offset of the DFS block
  for (i = 0; i < phys_block_ratio; i++)
  {
    if (DiskReadBlock(phys_block_start + i, &phys_b) == DISK_FAIL) {
      return DFS_FAIL;
    }
    bcopy(phys_b.data, b->data + (i * DiskBytesPerBlock()), DiskBytesPerBlock());
  }

  // Return the total number of bytes read (which is one full DFS block)
  return sb.fs_blocksize;
}


//-----------------------------------------------------------------
// DfsWriteBlock writes to an allocated DFS block on the disk
// (which could span multiple physical disk blocks).  The block
// must be allocated in order to write to it.  Returns DFS_FAIL
// on failure, and the number of bytes written on success.  
//-----------------------------------------------------------------
int DfsWriteBlock(uint32 blocknum, dfs_block *b){
  disk_block phys_b;
  uint32 phys_block_ratio;
  uint32 phys_block_start;
  int i;

  // Check that the file system is valid in memory
  if (sb.valid == 0) {
    return DFS_FAIL;
  }

  phys_block_ratio = sb.fs_blocksize / DiskBytesPerBlock();
  phys_block_start = blocknum * phys_block_ratio;

  // Chunk the DFS block data into physical blocks and write them to the disk
  for (i = 0; i < phys_block_ratio; i++) {
    bcopy(b->data + (i * DiskBytesPerBlock()), phys_b.data, DiskBytesPerBlock());

    if (DiskWriteBlock(phys_block_start +i, &phys_b) == DISK_FAIL) {
      return DFS_FAIL;
    }
  }

  return sb.fs_blocksize;
}


////////////////////////////////////////////////////////////////////////////////
// Inode-based functions
////////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------
// DfsInodeFilenameExists looks through all the inuse inodes for 
// the given filename. If the filename is found, return the handle 
// of the inode. If it is not found, return DFS_FAIL.
//-----------------------------------------------------------------

int DfsInodeFilenameExists(char *filename) {

}


//-----------------------------------------------------------------
// DfsInodeOpen: search the list of all inuse inodes for the 
// specified filename. If the filename exists, return the handle 
// of the inode. If it does not, allocate a new inode for this 
// filename and return its handle. Return DFS_FAIL on failure. 
// Remember to use locks whenever you allocate a new inode.
//-----------------------------------------------------------------

int DfsInodeOpen(char *filename) {

}


//-----------------------------------------------------------------
// DfsInodeDelete de-allocates any data blocks used by this inode, 
// including the indirect addressing block if necessary, then mark 
// the inode as no longer in use. Use locks when modifying the 
// "inuse" flag in an inode.Return DFS_FAIL on failure, and 
// DFS_SUCCESS on success.
//-----------------------------------------------------------------

int DfsInodeDelete(uint32 handle) {

}


//-----------------------------------------------------------------
// DfsInodeReadBytes reads num_bytes from the file represented by 
// the inode handle, starting at virtual byte start_byte, copying 
// the data to the address pointed to by mem. Return DFS_FAIL on 
// failure, and the number of bytes read on success.
//-----------------------------------------------------------------

int DfsInodeReadBytes(uint32 handle, void *mem, int start_byte, int num_bytes) {

}


//-----------------------------------------------------------------
// DfsInodeWriteBytes writes num_bytes from the memory pointed to 
// by mem to the file represented by the inode handle, starting at 
// virtual byte start_byte. Note that if you are only writing part 
// of a given file system block, you'll need to read that block 
// from the disk first. Return DFS_FAIL on failure and the number 
// of bytes written on success.
//-----------------------------------------------------------------

int DfsInodeWriteBytes(uint32 handle, void *mem, int start_byte, int num_bytes) {


}


//-----------------------------------------------------------------
// DfsInodeFilesize simply returns the size of an inode's file. 
// This is defined as the maximum virtual byte number that has 
// been written to the inode thus far. Return DFS_FAIL on failure.
//-----------------------------------------------------------------

int DfsInodeFilesize(uint32 handle) {

}


//-----------------------------------------------------------------
// DfsInodeAllocateVirtualBlock allocates a new filesystem block 
// for the given inode, storing its blocknumber at index 
// virtual_blocknumber in the translation table. If the 
// virtual_blocknumber resides in the indirect address space, and 
// there is not an allocated indirect addressing table, allocate it. 
// Return DFS_FAIL on failure, and the newly allocated file system 
// block number on success.
//-----------------------------------------------------------------

int DfsInodeAllocateVirtualBlock(uint32 handle, uint32 virtual_blocknum) {


}



//-----------------------------------------------------------------
// DfsInodeTranslateVirtualToFilesys translates the 
// virtual_blocknum to the corresponding file system block using 
// the inode identified by handle. Return DFS_FAIL on failure.
//-----------------------------------------------------------------

int DfsInodeTranslateVirtualToFilesys(uint32 handle, uint32 virtual_blocknum) {

}
