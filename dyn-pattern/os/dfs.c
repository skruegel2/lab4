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

// Strict lock hierarchy:
// files_lock (files.c) -> indode_lock -> fbv_lock -> cache_lock
static lock_t inode_lock;
static lock_t fbv_lock;
static lock_t cache_lock;

// Adaptive clock with frequency aging cache structure
#define FREQ_MAX 15
#define TRANSITION_WINDOW 64
#define AGING_INTERVAL 64

typedef struct dfs_cache_slot {
  int slot_index;   // Index (0..32) in the cache[] array
  int inuse;        // 1 if valid block is cached, 0 if empty
  int dirty;        // 1 if modified and needs write-back, 0 if clean
  uint32 blocknum;  // DFS filesystem block num
  int ref_bit;      // Clock second chance bit
  uint32 freq;      // Aged frequency counter (0.. FREQ_MAX)
  dfs_block data;   // block payload
} dfs_cache_slot;

static dfs_cache_slot cache[DFS_CACHE_SLOTS];

// Adaptive Clock & Pattern Detection state (protected by cache_lock)
static int clock_hand = 0;
static int scan_mode = 0;   // 0 = clock + aging, 1 = scan/loop bypass (MRU probation)
static int last_loaded_slot = -1; // Most recently loaded slot for MRU scan/loop resistance
static uint32 transition_count = 0;
static uint32 window_transitions = 0;
static uint32 window_hits = 0;
static uint32 bypass_counter = 0;
static int recent_blocks[2] = {-1, -1}; // Filters intral-block 4B repeats and ping pong

// Cumulative cache and physical disk counters
static double cache_hits = 0.0;
static double cache_misses = 0.0;
static uint32 disk_reads = 0;
static uint32 disk_writes = 0;

// Prototypes
static void PrintCacheMissStats();
static int DfsCacheHitInternal(int blocknum);
static int DfsCacheAllocateSlotInternal(int blocknum);
static void RecordBlockAccessLocked(int blocknum, int is_hit, int slot);
static int SelectVictimSlotLocked();
static int DfsInodeFilenameExistsLocked(char* filename);
static int DfsInodeTranslateLocked(uint32 handle, uint32 virtual_blocknum);
static int DfsInodeAllocateBlockLocked(uint32 handle, uint32 virtual_blocknum);

///////////////////////////////////////////////////////////////////
// Non-inode functions first
///////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------
// DfsModuleInit is called at boot time to initialize things and
// open the file system for use.
//-----------------------------------------------------------------

void DfsModuleInit() {
  int i;

  inode_lock = LockCreate();
  fbv_lock = LockCreate();
  cache_lock = LockCreate();

  // Reset telemetry counters
  cache_hits = 0.0;
  cache_misses = 0.0;
  disk_reads = 0;
  disk_writes = 0;

  clock_hand = 0;
  scan_mode = 0;
  last_loaded_slot = -1;
  transition_count = 0;
  window_transitions =  0;
  window_hits = 0;
  bypass_counter = 0;
  recent_blocks[0] = -1;
  recent_blocks[1] = -1;

  // Initialize all 32 cache slots and place them in free_cache q
  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    cache[i].slot_index = i;
    cache[i].inuse = 0;
    cache[i].dirty = 0;
    cache[i].blocknum = 0;
    cache[i].ref_bit = 0;
    cache[i].freq = 0;
  }

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
  dbprintf ('i', "Open dfs filesystem.\n");

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

  if (sb.valid == 0 || sb.fs_blocksize <= 0) {
    sb.valid = 0;
    return DFS_FAIL;
  }

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
      sb.valid = 0;
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
      sb.valid = 0;
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

  // Write to duplicate superblock 
  DiskWriteBlock(((sb.total_fs_blocks - 1) * phys_block_ratio) + 1, &b);

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

  // Flush all dirty blocks from the buffer cache to disk before closing
  if (DfsCacheFlush() == DFS_FAIL) {
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

  // Write to duplicate superblock 
  DiskWriteBlock(((sb.total_fs_blocks - 1) * phys_block_ratio) + 1, &b);

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
  LockHandleAcquire(fbv_lock);

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
  LockHandleRelease(fbv_lock);

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
  int slot;

  // Check that the file system has been correctly loaded into memory
  if (sb.valid == 0 || blocknum >= (uint32)sb.total_fs_blocks) {
    return DFS_FAIL;
  }

  array_index = blocknum / 32;
  bit_index = blocknum % 32;

  // Create a mask with a 0 at the target bits and 1 everywhere else
  // Use provided invert() function
  mask = invert(1 << bit_index);

  // Acquire lock before modifying fbv
  LockHandleAcquire(fbv_lock);

  // Use bitwise AND to clear the specific bit to 0 (free)
  fbv[array_index] &= mask;

  // Release lock
  LockHandleRelease(fbv_lock);

  // Evict and discard if this freed block is currently in the buffer cache
  LockHandleAcquire(cache_lock);
  slot = DfsCacheHitInternal(blocknum);
  if (slot != DFS_FAIL) {
    cache[slot].inuse = 0;
    cache[slot].dirty = 0;
    cache[slot].blocknum = 0;
    cache[slot].ref_bit = 0;
    cache[slot].freq = 0;
    if (last_loaded_slot == slot) {
      last_loaded_slot = -1;
    }
  }
  LockHandleRelease(cache_lock);
  return DFS_SUCCESS;
}


//-----------------------------------------------------------------
// DfsReadBlockUncached reads an allocated DFS block directly from
// the physical disk and increments disk_reads per physical block.
//-----------------------------------------------------------------
int DfsReadBlockUncached(uint32 blocknum, dfs_block *b) {
  disk_block phys_b;
  uint32 phys_block_ratio;
  uint32 phys_block_start;
  int i;

  // Check that the file system is valid
  if (sb.valid == 0 || blocknum >= sb.total_fs_blocks) {
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
    disk_reads++;
    bcopy(phys_b.data, b->data + (i * DiskBytesPerBlock()), DiskBytesPerBlock());
  }

  // Return the total number of bytes read (which is one full DFS block)
  return sb.fs_blocksize;
}

//-----------------------------------------------------------------
// DfsWriteBlockUncached writes to an allocated DFS block directly to
// the physical disk and increments disk_writes per physical block.
//-----------------------------------------------------------------
int DfsWriteBlockUncached(uint32 blocknum, dfs_block *b){
  disk_block phys_b;
  uint32 phys_block_ratio;
  uint32 phys_block_start;
  int i;

  // Check that the file system is valid in memory
  if (sb.valid == 0 || blocknum >= sb.total_fs_blocks) {
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
    disk_writes++;
  }

  return sb.fs_blocksize;
}

//-----------------------------------------------------------------
// RecordBlockAccessLocked updates ref_bit, frequency, aging and
// scan_mode detection on inter block transitions (caller holds cache_lock)
//-----------------------------------------------------------------
static void RecordBlockAccessLocked(int blocknum, int is_hit, int slot) {
  int i;
  int is_distinct_transition;

  // Check if this access moves to a block outside the immediate 2 block
  // indirect + current data block) micro-loop caused by 4 byte reads
  is_distinct_transition = (blocknum != recent_blocks[0] && blocknum != recent_blocks[1]);

  if (blocknum != recent_blocks[0]) {
    recent_blocks[1] = recent_blocks[0];
    recent_blocks[0] = blocknum;
  }

  if (is_hit) {
    cache[slot].ref_bit = 1;
    // Always boost freq on genuine inter-block reuse or initial confirmation
    // cap at FREQ_MAX so stale blocks decay cleanly
    if (is_distinct_transition) {
      if (cache[slot].freq < FREQ_MAX) {
        cache[slot].freq++;
      }
    }
    // If the probation slot gets reused across distinct blocks, don't immediately evict
    if (is_distinct_transition && slot == last_loaded_slot) {
      last_loaded_slot = -1;
    }
  }

  if (is_distinct_transition) {
    transition_count++;
    window_transitions++;
    if (is_hit) {
      window_hits++;
    }

    // Periodic frequency aging; halve all frequencies every AGING_INTERVAL
    // so blocks pinned in an earlier phase naturally decay when workload shifts
    if ((transition_count % AGING_INTERVAL) == 0) {
      for (i = 0; i < DFS_CACHE_SLOTS; i++) {
        if (cache[i].inuse == 1) {
          cache[i].freq >>= 1;
        }
      }
    }

    // Evaluate dominant pattern every TRANSITION_WINDOW block transitions
    if (window_transitions >= TRANSITION_WINDOW) {
      if (window_hits <= 5) {
        // Hit rate <= 15% on distinct blocks => sequential scan or loop > 32 blocks
        // Scan/loop MRU probabtion mode to pin 31 slots and stop cyclic thrashing
        scan_mode = 1;
      }
      else {
        // Working set fits or skewed locality => standard clock + aging
        scan_mode = 0;
      }
      window_transitions = 0;
      window_hits = 0;
    }
  }
}

//-----------------------------------------------------------------
// SelectVictimSlotLocked picks a victim using scan/loop bypass
// when scan_mode = 1, or adaptive clock sweep otherwise
//-----------------------------------------------------------------
static int SelectVictimSlotLocked() {
  int slot;

  // 1. Scan / large-loop resistance mode:
  // Evict the probationary last_loaded slot (MRU behavior) if it hasn't
  // proven inter-block reuse, locking the remaining 31 blocks in cache so
  // loops > 32 blocks score hits on 30-31 blocks every iteration instead of 0%
  // Every 16th miss falls through to clock sweep so new phases can claim slots
  if (scan_mode == 1) {
    bypass_counter++;
    if ((bypass_counter & 15) != 0 &&
      last_loaded_slot >= 0 &&
      last_loaded_slot < DFS_CACHE_SLOTS &&
      cache[last_loaded_slot].inuse == 1 &&
      cache[last_loaded_slot].freq <= 1) {
        return last_loaded_slot;
    }
  }

  // 2. Adaptive clock sweep with frequency aging
  // Bounded loop since ref_bit <= 1 and freq <= FREQ_MAX, this always terminates
  while(1) {
    slot = clock_hand;
    clock_hand = (clock_hand + 1) % DFS_CACHE_SLOTS;

    if (cache[slot].inuse == 0) {
      return slot;
    }

    if (cache[slot].ref_bit == 1) {
      // Give second chance
      cache[slot].ref_bit = 0;
    }
    else if (cache[slot].freq > 1) {
      // High freq block, age down instead of evicting immediately
      cache[slot].freq--;
    }
    else {
      // Clock block (ref_bit == 0 and freq < 1): evict
      return slot;
    }
  }
}

//-----------------------------------------------------------------
// Internal unlocked helpeer for DfsCacheHit (caller holds cache lock)
//-----------------------------------------------------------------
static int DfsCacheHitInternal(int blocknum) {
  int i;

  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    if (cache[i].inuse == 1 && cache[i].blocknum == (uint32)blocknum) {
      return i;
    }
  }
  return DFS_FAIL;
}

//-----------------------------------------------------------------
// DfsCacheHit checks the cache for the given blocknum.
// Returns DFS_FAIL if not found, or the slot handle on success
//-----------------------------------------------------------------
int DfsCacheHit(int blocknum) {
  int slot;
  if (sb.valid == 0 || blocknum < 0 || (uint32)blocknum >= sb.total_fs_blocks) {
    return DFS_FAIL;
  }

  LockHandleAcquire(cache_lock);
  slot = DfsCacheHitInternal(blocknum);
  LockHandleRelease(cache_lock);

  return slot;
}

//-----------------------------------------------------------------
// DfsCacheAllocateSlotInternal allocates an empty slot or evicts
// a victim using adaptive clock + aging (caller holds lock)
//-----------------------------------------------------------------
static int DfsCacheAllocateSlotInternal(int blocknum) {
  int i;
  int victim = -1;

  // 1. Use any free slot first
  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    if (cache[i].inuse == 0) {
      victim = i;
      break;
    }
  }

  // 2. Otherwise select a victim via Adaptive clock with freq aging
  if (victim == -1) {
    victim = SelectVictimSlotLocked();
    if (cache[victim].inuse == 1 && cache[victim].dirty == 1) {
      dbprintf('b', "Cache evict: Writing back dirty block %d from slot %d\n", 
        cache[victim].blocknum, victim);
      if (DfsWriteBlockUncached(cache[victim].blocknum, &cache[victim].data) == DFS_FAIL) {
        return DFS_FAIL;
      }
      cache[victim].dirty = 0;
    }
  }

  // 3. Initialize new block on probation:
  // ref_bit = 0, freq = 1, so one-time scan blocks don't pollute hot blocks
  // while a second interblock access promotes freq >= 2 and ref_bit = 1.
  cache[victim].inuse = 1;
  cache[victim].dirty = 0;
  cache[victim].blocknum = (uint32)blocknum;
  cache[victim].ref_bit = (scan_mode == 1) ? 0 : 1;
  cache[victim].freq = 1;

  last_loaded_slot = victim;
  return victim;
}

//-----------------------------------------------------------------
// DfsCacheAllocate slot: public wrapper protected by cache lock
//-----------------------------------------------------------------
int DfsCacheAllocateSlot(int blocknum) {
  int slot;

  if (sb.valid == 0 || blocknum < 0 || (uint32)blocknum >= sb.total_fs_blocks) {
    return DFS_FAIL;
  }

  LockHandleAcquire(cache_lock);
  slot = DfsCacheAllocateSlotInternal(blocknum);
  LockHandleRelease(cache_lock);

  return slot;
}

//-----------------------------------------------------------------
// DfsReadBlock checks the buffer cache for blocknum, loading it
// from disk on a miss, and copies sb.fs_blocksize bytes into b
//-----------------------------------------------------------------
int DfsReadBlock(uint32 blocknum, dfs_block *b) {
  int slot;

  if (sb.valid == 0 || blocknum >= sb.total_fs_blocks) {
    return DFS_FAIL;
  }

  LockHandleAcquire(cache_lock);

  slot = DfsCacheHitInternal(blocknum);
  if (slot != DFS_FAIL) {
    dbprintf('b', "Cache Hit: Block %d found in slot %d (Hits=%d, Misses=%d)\n",
        blocknum, slot, cache_hits, cache_misses);
    cache_hits += 1.0;  
    RecordBlockAccessLocked((int)blocknum, 1, slot);
  }
  else {
    cache_misses += 1.0;
    slot = DfsCacheAllocateSlotInternal(blocknum);
    if (slot == DFS_FAIL) {
      LockHandleRelease(cache_lock);
      return DFS_FAIL;
    }

    if (DfsReadBlockUncached(blocknum, &cache[slot].data) == DFS_FAIL) {
      cache[slot].inuse = 0;
      LockHandleRelease(cache_lock);
      return DFS_FAIL;
    }
    RecordBlockAccessLocked((int)blocknum, 0, slot);
    PrintCacheMissStats();
  }

  bcopy(cache[slot].data.data, b->data, sb.fs_blocksize);

  LockHandleRelease(cache_lock);
  return sb.fs_blocksize;
}

//-----------------------------------------------------------------
// DfsWriteBlock writes blocknum via writeback adaptive cache
//-----------------------------------------------------------------
int DfsWriteBlock(uint32 blocknum, dfs_block * b) {
  int slot;

  if (sb.valid == 0 || blocknum >= sb.total_fs_blocks) {
    return DFS_FAIL;
  }

  LockHandleAcquire(cache_lock);

  slot = DfsCacheHitInternal(blocknum);
  if (slot != DFS_FAIL) {
    dbprintf('b', "Cache Hit: Block %d found in slot %d (Hits=%d, Misses=%d)\n",
        blocknum, slot, cache_hits, cache_misses);    
    cache_hits += 1.0;
    RecordBlockAccessLocked((int)blocknum, 1, slot);
  }
  else {
    cache_misses += 1.0;

    slot = DfsCacheAllocateSlotInternal((int)blocknum);
    if (slot == DFS_FAIL) {
      LockHandleRelease(cache_lock);
      return DFS_FAIL;
    }

    if (DfsReadBlockUncached(blocknum, &cache[slot].data) == DFS_FAIL) {
      cache[slot].inuse = 0;
      LockHandleRelease(cache_lock);
      return DFS_FAIL;
    }

    RecordBlockAccessLocked((int)blocknum, 0, slot);
    PrintCacheMissStats();
  }

  bcopy(b->data, cache[slot].data.data, sb.fs_blocksize);
  cache[slot].dirty = 1;

  LockHandleRelease(cache_lock);
  return sb.fs_blocksize;
}

//-----------------------------------------------------------------
// DfsCacheFlush writes all dirty cached blocks back to disk 
// without counting hits or misses.
//-----------------------------------------------------------------
int DfsCacheFlush() {
  int i;

  if (sb.valid == 0) {
    return DFS_FAIL;
  }

  LockHandleAcquire(cache_lock);

  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    if (cache[i].inuse == 1) {
      if (cache[i].dirty == 1) {
        dbprintf('b', "Cache Flush: Writing dirty block %d from slot %d\n",
          cache[i].blocknum, i);
        if (DfsWriteBlockUncached(cache[i].blocknum, &cache[i].data) == DFS_FAIL) {
          LockHandleRelease(cache_lock);
          return DFS_FAIL;
        }
        cache[i].dirty = 0;
      }
      cache[i].inuse = 0;
      cache[i].blocknum = 0;
      cache[i].ref_bit = 0;
      cache[i].freq = 0;
    }
  }
  
  last_loaded_slot = -1;
  clock_hand = 0;

  LockHandleRelease(cache_lock);
  return DFS_SUCCESS;
}
 
//-----------------------------------------------------------------
// Internal helper: prints cumulative cache miss and disk I/O stats
// Format: Cache Miss: Hit Rate = XX.XXX%, Miss Rate = XX.XXX%,
// Disk Reads = N, Disk Writes = M 
//-----------------------------------------------------------------
static void PrintCacheMissStats() {
  double total = cache_hits + cache_misses;
  uint32 hit_scaled;
  uint32 miss_scaled;

  if (total <= 0.0) {
    return;
  }

  // Compute percentages scaled by 1000
  hit_scaled = (uint32)((((double)cache_hits * 100000.0) / total) + 0.5);
  if (hit_scaled > 100000) {
    hit_scaled = 100000;
  }
  miss_scaled = 100000 - hit_scaled;

  printf("Cache Miss: Hit Rate = %d.%03d%%, Miss Rate = %d.%03d%%, Disk Reads = %d, Disk Writes = %d\n",
          hit_scaled / 1000, hit_scaled % 1000,
          miss_scaled / 1000, miss_scaled % 1000,
          disk_reads, disk_writes);
}

////////////////////////////////////////////////////////////////////////////////
// Inode-based functions (protected by inode_lock)
////////////////////////////////////////////////////////////////////////////////

static int DfsInodeFilenameExistsLocked(char* filename) {
  int i;

  for (i = 0; i < sb.num_inodes; i++) {
    if (inodes[i].inuse == 1) {
      if (dstrncmp(inodes[i].filename, filename, FILE_MAX_FILENAME_LENGTH) == 0) {
        return i;
      }
    }
  }
  return DFS_FAIL;
}

//-----------------------------------------------------------------
// DfsInodeFilenameExists looks through all the inuse inodes for 
// the given filename. If the filename is found, return the handle 
// of the inode. If it is not found, return DFS_FAIL.
//-----------------------------------------------------------------
int DfsInodeFilenameExists(char *filename) {
  int handle;

  // Check that the file system has been loaded and is valid.
  if (sb.valid == 0) {
    return DFS_FAIL;
  }

  LockHandleAcquire(inode_lock);
  handle = DfsInodeFilenameExistsLocked(filename);
  LockHandleRelease(inode_lock);

  return handle;
}


//-----------------------------------------------------------------
// DfsInodeOpen: search the list of all inuse inodes for the 
// specified filename. If the filename exists, return the handle 
// of the inode. If it does not, allocate a new inode for this 
// filename and return its handle. Return DFS_FAIL on failure. 
// Remember to use locks whenever you allocate a new inode.
//-----------------------------------------------------------------
int DfsInodeOpen(char *filename) {
  int handle;
  int i;
  int j;

  // Check that the file system has been loaded and is valid.
  if (sb.valid == 0 || filename == NULL) {
    return DFS_FAIL;
  }  

  LockHandleAcquire(inode_lock);

  // First, check if the file already exists
  handle = DfsInodeFilenameExistsLocked(filename);
  if (handle != DFS_FAIL) {
    LockHandleRelease(inode_lock);
    return handle;
  }

  // Not found, so allocate a new inode
  // Set default handle to fail in case inode table completely full
  handle = DFS_FAIL;

 
  for(i = 0; i < sb.num_inodes; i++) {
    if (inodes[i].inuse == 0) {
      // Found a free inode
      inodes[i].inuse = 1;
      inodes[i].file_size = 0;

      // Safely copy the filename into the inode
      for(j = 0; j < FILE_MAX_FILENAME_LENGTH; j++) {
        inodes[i].filename[j] = filename[j];
        if (filename[j] == '\0') {
          break;  // Stop copying upon reaching the null terminator
        }
      }
      // Make sure null terminator in place
      inodes[i].filename[FILE_MAX_FILENAME_LENGTH - 1] = '\0';

      // Zero out the block address translation tables to prevent stale mappings
      for (j = 0; j < 10; j++) {
        inodes[i].direct[j] = 0;
      }

      inodes[i].indirect = 0;
      inodes[i].double_indirect = 0;

      handle = i;
      break;  // Exit the loop since we successfully allocated the inode      
    }
  }
  // Release the lock
  LockHandleRelease(inode_lock);
  
  return handle;
}

//-----------------------------------------------------------------
// DfsInodeDelete de-allocates any data blocks used by this inode, 
// including the indirect addressing block if necessary, then mark 
// the inode as no longer in use. Use locks when modifying the 
// "inuse" flag in an inode.Return DFS_FAIL on failure, and 
// DFS_SUCCESS on success.
//-----------------------------------------------------------------
int DfsInodeDelete(uint32 handle) {
  int i;
  int max_vblocks;
  uint32 phys_block;
  dfs_block b;
  int *table;
  uint32 entries_per_block;


   // Check that the file system has been loaded and is valid.
  if (sb.valid == 0 || handle >= (uint32)sb.num_inodes) {
    return DFS_FAIL;
  } 

  LockHandleAcquire(inode_lock);

  if (inodes[handle].inuse == 0) {
    LockHandleRelease(inode_lock);
    return DFS_FAIL;
  }

  entries_per_block = sb.fs_blocksize / sizeof(int);

  if (inodes[handle].file_size == 0) {
    max_vblocks = 0;
  }
  else {
    max_vblocks = (inodes[handle].file_size + sb.fs_blocksize - 1) /sb.fs_blocksize;
  }

  // 1. Free allocated data blocks
  for (i = 0; i < max_vblocks; i++) {
    phys_block = DfsInodeTranslateLocked(handle, (uint32)i);
    if (phys_block != DFS_FAIL && phys_block != 0) {
      DfsFreeBlock((uint32)phys_block);
    }
  }

  // 2. Free single-indirect block
  if (inodes[handle].indirect != 0) {
    DfsFreeBlock((uint32)inodes[handle].indirect);
    inodes[handle].indirect = 0;
  }

  // 3. Free double indirect mid-level and top-level blocks
  if (inodes[handle].double_indirect != 0) {
    if (DfsReadBlock((uint32)inodes[handle].double_indirect, &b) != DFS_FAIL) {
      table = (int*)b.data;
      for (i = 0; i < (int)entries_per_block; i++) {
        if (table[i] != 0) {
          DfsFreeBlock((uint32)table[i]);
        }
      }
    }
    DfsFreeBlock((uint32)inodes[handle].double_indirect);
    inodes[handle].double_indirect = 0;
  }

  // 4. Clear inode fields and mark free
  for (i = 0; i < 10; i++) {
    inodes[handle].direct[i] = 0;
  }
  inodes[handle].file_size = 0;
  inodes[handle].inuse = 0;

  LockHandleRelease(inode_lock);
  return DFS_SUCCESS;
}

//-----------------------------------------------------------------
// DfsInodeRename changes the filename of an existing inode.
// Returns DFS_FAIL if the old file doesn't exist or the new
// filename is already taken.
//-----------------------------------------------------------------
int DfsInodeRename(char* oldname, char* newname)
{
  int handle;
  int i;

  // Validate filesystem
  if (sb.valid == 0 || oldname == NULL || newname == NULL) {
    return DFS_FAIL;
  }

  LockHandleAcquire(inode_lock);

  // Ensure the new filename isn't already in use
  if (DfsInodeFilenameExistsLocked(newname) != DFS_FAIL) {
    LockHandleRelease(inode_lock);
    return DFS_FAIL;
  }

  // Find the existing file
  handle = DfsInodeFilenameExists(oldname);
  if (handle == DFS_FAIL) {
    LockHandleRelease(inode_lock);
    return DFS_FAIL;
  }

  for(i = 0; i < FILE_MAX_FILENAME_LENGTH; i++) {
    inodes[handle].filename[i] = newname[i];
    if (newname[i] == '\0') {
      break;
    }
  }

  // Guarantee null termination
  inodes[handle].filename[FILE_MAX_FILENAME_LENGTH - 1] = '\0';

  LockHandleRelease(inode_lock);

  return DFS_SUCCESS;
}

//-----------------------------------------------------------------
// DfsInodeFilesize simply returns the size of an inode's file. 
// This is defined as the maximum virtual byte number that has 
// been written to the inode thus far. Return DFS_FAIL on failure.
//-----------------------------------------------------------------
int DfsInodeFilesize(uint32 handle) {
  int size;
  // Validate filesystem and inode handle
  if (sb.valid == 0 || handle >= sb.num_inodes || inodes[handle].inuse == 0) {
    return DFS_FAIL;
  }

  LockHandleAcquire(inode_lock);
  if (inodes[handle].inuse == 0) {
    LockHandleRelease(inode_lock);
    return DFS_FAIL;
  }

  size = inodes[handle].file_size;
  LockHandleRelease(inode_lock);
  return size;
}

//-----------------------------------------------------------------
// Internal unlocked helper
//-----------------------------------------------------------------
static int DfsInodeTranslateLocked(uint32 handle, uint32 virtual_blocknum) {
  dfs_block b;
  int *table;
  uint32 indirect_blocknum;
  uint32 single_indirect_index;
  uint32 data_index;
  uint32 block_result;
  uint32 entries_per_block;
  uint32 max_vblocks;

  entries_per_block = sb.fs_blocksize / sizeof(int);
  max_vblocks = 10 + entries_per_block + (entries_per_block * entries_per_block);

  if (virtual_blocknum >= max_vblocks) {
    return DFS_FAIL;
  }

  // 1. Direct addressing
  if (virtual_blocknum < 10) {
    block_result = inodes[handle].direct[virtual_blocknum];
    return (block_result == 0) ? DFS_FAIL : block_result;
  }

  // 2. Single-indirect addressing
  if (virtual_blocknum < (10 + entries_per_block)) {
    if (inodes[handle].indirect == 0) {
      return DFS_FAIL;
    }
    if (DfsReadBlock((uint32)inodes[handle].indirect, &b) == DFS_FAIL) {
      return DFS_FAIL;
    }
    table = (int*)b.data;
    block_result = table[virtual_blocknum - 10];
    return (block_result == 0) ? DFS_FAIL : block_result;
  }

  // 3. Double-indirect addressing
  if (inodes[handle].double_indirect == 0) {
    return DFS_FAIL;    
  }
  if (DfsReadBlock((uint32)inodes[handle].double_indirect, &b) == DFS_FAIL) {
    return DFS_FAIL;
  }

  table = (int*)b.data;
  single_indirect_index = (virtual_blocknum - (10 + entries_per_block)) / entries_per_block;
  data_index = (virtual_blocknum - (10 + entries_per_block)) % entries_per_block;

  indirect_blocknum = (uint32)table[single_indirect_index];
  if (indirect_blocknum == 0) {
    return DFS_FAIL;
  }

  if (DfsReadBlock(indirect_blocknum, &b) == DFS_FAIL) {
    return DFS_FAIL;    
  }

  table = (int*)b.data;
  block_result = table[data_index];

  return (block_result == 0) ? DFS_FAIL : block_result;
}

//-----------------------------------------------------------------
// DfsInodeTranslateVirtualToFilesys public wrapper
//-----------------------------------------------------------------
int DfsInodeTranslateVirtualToFilesys(uint32 handle, uint32 virtual_blocknum) {
  int block_result;

  // Check that the file system has been loaded and is valid.
  if (sb.valid == 0 || handle >= (uint32)sb.num_inodes) {
    return DFS_FAIL;
  }  

  LockHandleAcquire(inode_lock);
  if (inodes[handle].inuse == 0) {
    LockHandleRelease(inode_lock);
    return DFS_FAIL;
  }
  block_result =  DfsInodeTranslateLocked(handle, virtual_blocknum);
  LockHandleRelease(inode_lock);

  return block_result;
}

//-----------------------------------------------------------------
// DfsInodeAllocateVirtualBlockLocked allocates a new filesystem block 
// for the given inode, storing its blocknumber at index 
// virtual_blocknumber in the translation table. If the 
// virtual_blocknumber resides in the indirect address space, and 
// there is not an allocated indirect addressing table, allocate it. 
// Return DFS_FAIL on failure, and the newly allocated file system 
// block number on success.
//-----------------------------------------------------------------
static int DfsInodeAllocateVirtualBlockLocked(uint32 handle, uint32 virtual_blocknum) {
  dfs_block b;
  int* table;
  uint32 new_block;
  uint32 indirect_block;
  uint32 single_indirect_index;
  uint32 data_index;
  uint32 entries_per_block;
  uint32 max_vblocks;

  entries_per_block = sb.fs_blocksize / sizeof(int);

  max_vblocks = 10 + entries_per_block + (entries_per_block * entries_per_block);

  if (virtual_blocknum >= max_vblocks) {
    return DFS_FAIL;
  }

  // Check if the virtual block is already allocated
  new_block = DfsInodeTranslateLocked(handle, virtual_blocknum);
  if (new_block != DFS_FAIL) {
    return new_block;
  }

  // Allocate a new data block for the file
  new_block = DfsAllocateBlock();
  if (new_block == DFS_FAIL) {
    return DFS_FAIL;  // Disk is full
  }

  // 1. Direct addressing (virtual blocks 0 to 9)
  if (virtual_blocknum < 10) {
    inodes[handle].direct[virtual_blocknum] = new_block;
    return new_block;
  }

  // 2. Single indirect addressing 
  if (virtual_blocknum < (10 + entries_per_block)) {
    if (inodes[handle].indirect == 0) {
      // Allocate the single indirect table block
      inodes[handle].indirect = DfsAllocateBlock();
      if (inodes[handle].indirect == DFS_FAIL) {
        inodes[handle].indirect = 0;
        DfsFreeBlock(new_block);  // Rollback data allocation
        return DFS_FAIL;
      }
      // Zero out the newly created indirect block
      bzero(b.data, sb.fs_blocksize);
      if (DfsWriteBlock(inodes[handle].indirect, &b) == DFS_FAIL) {
        return DFS_FAIL;
      }
    }

    // Read the indirect block, update the table, and write it back
    if (DfsReadBlock(inodes[handle].indirect, &b) == DFS_FAIL) {
      return DFS_FAIL;
    }

    table = (int*)b.data;
    table[virtual_blocknum - 10] = new_block;

    if (DfsWriteBlock(inodes[handle].indirect, &b) == DFS_FAIL) {
      return DFS_FAIL;
    }
    return new_block;
  }

  // 3. Double indirect addressing 
  single_indirect_index = (virtual_blocknum - (10 + entries_per_block)) / entries_per_block;
  data_index = (virtual_blocknum - (10 + entries_per_block)) % entries_per_block;

  // Allocate top-level double indirect table if missing
  if (inodes[handle].double_indirect == 0) {
    inodes[handle].double_indirect = DfsAllocateBlock();
    if (inodes[handle].double_indirect == DFS_FAIL) {
      inodes[handle].double_indirect = 0;
      DfsFreeBlock(new_block);
      return DFS_FAIL;
    }
    bzero(b.data, sb.fs_blocksize);
    if (DfsWriteBlock(inodes[handle].double_indirect, & b) == DFS_FAIL) {
      return DFS_FAIL;
    }
  }

  // Read the top-level table
  if (DfsReadBlock(inodes[handle].double_indirect, &b) == DFS_FAIL) {
    return DFS_FAIL;
  }
  table = (int*)b.data;
  indirect_block = table[single_indirect_index];

  // Allocate mid-level single indirect table if missing
  if (indirect_block == 0) {
    indirect_block = DfsAllocateBlock();
    if (indirect_block == DFS_FAIL) {
      DfsFreeBlock(new_block);
      return DFS_FAIL;
    }

    // Write the new mid-level block number into the top-level table
    table[single_indirect_index] = indirect_block;
    if (DfsWriteBlock(inodes[handle].double_indirect, &b) == DFS_FAIL) {
      return DFS_FAIL;
    }

    // Zero out the new mid-level block and write it to disk
    bzero(b.data, sb.fs_blocksize);
    if (DfsWriteBlock(indirect_block, &b) == DFS_FAIL) {
      return DFS_FAIL;
    }
  }

  // Read the mid-level table, insert the new data block number, and write it back
  if (DfsReadBlock(indirect_block, &b) == DFS_FAIL) {
    return DFS_FAIL;
  }
  table = (int*)b.data;
  table[data_index] = new_block;

  if (DfsWriteBlock(indirect_block, &b) == DFS_FAIL) {
    return DFS_FAIL;
  }
  return new_block;
}

//-----------------------------------------------------------------
// DfsInodeAllocateVirtualBlock public wrapper
//-----------------------------------------------------------------
int DfsInodeAllocateVirtualBlock(uint32 handle, uint32 virtual_blocknum) {
  int block_result;

  if (sb.valid == 0 || handle >= (uint32)sb.num_inodes) {
    return DFS_FAIL;
  }

  LockHandleAcquire(inode_lock);
  if (inodes[handle].inuse == 0) {
    LockHandleRelease(inode_lock);
    return DFS_FAIL;
  }
  block_result = DfsInodeAllocateVirtualBlockLocked(handle, virtual_blocknum);
  LockHandleRelease(inode_lock);

  return block_result;
}

//-----------------------------------------------------------------
// DfsInodeReadBytes reads num_bytes from the file represented by 
// the inode handle, starting at virtual byte start_byte, copying 
// the data to the address pointed to by mem. Return DFS_FAIL on 
// failure, and the number of bytes read on success.
//-----------------------------------------------------------------
int DfsInodeReadBytes(uint32 handle, void *mem, int start_byte, int num_bytes) {
  dfs_block b;
  uint32 virtual_block;
  uint32 offset;
  uint32 physical_block;
  int bytes_to_read;
  int bytes_read = 0;
  char *dest = (char*)mem;

  // Validate filesystem and inode
  if (sb.valid == 0 || handle >= sb.num_inodes || inodes[handle].inuse == 0) {
    return DFS_FAIL;
  }

  LockHandleAcquire(inode_lock);

  if (inodes[handle].inuse == 0) {
    LockHandleRelease(inode_lock);
    return DFS_FAIL;
  }

  // Prevent reading past the actual file size
  if (start_byte >= inodes[handle].file_size) {
    LockHandleRelease(inode_lock);
    return 0;     // EOF reached
  }
  if (start_byte + num_bytes > inodes[handle].file_size) {
    num_bytes = inodes[handle].file_size - start_byte;
  }

  while (num_bytes > 0) {
    virtual_block = start_byte / sb.fs_blocksize;
    offset = start_byte % sb.fs_blocksize;

    // Calculate how many bytes we can read from the current block
    bytes_to_read = sb.fs_blocksize - offset;
    if (bytes_to_read > num_bytes) {
      bytes_to_read = num_bytes;
    }

    physical_block = DfsInodeTranslateLocked(handle, virtual_block);
    if (physical_block == DFS_FAIL) {
      LockHandleRelease(inode_lock);
      return DFS_FAIL;  // Should not happen if file_size is correct
    }
    if (DfsReadBlock(physical_block, &b) == DFS_FAIL) {
      LockHandleRelease(inode_lock);
      return DFS_FAIL;
    }

    bcopy(b.data + offset, dest + bytes_read, bytes_to_read);
    bytes_read += bytes_to_read;
    start_byte += bytes_to_read;
    num_bytes -= bytes_to_read;
  }
  LockHandleRelease(inode_lock);
  return bytes_read;
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
  dfs_block b;
  uint32 virtual_block;
  uint32 offset;
  uint32 physical_block;
  int bytes_to_write;
  int bytes_written = 0;
  int was_allocated;
  char *src = (char*)mem;

  // Validate filesystem and inode
  if (sb.valid == 0 || mem == NULL || start_byte < 0 || num_bytes < 0 || handle >= (uint32)sb.num_inodes) {
    return DFS_FAIL;
  }

  LockHandleAcquire(inode_lock);

  if (inodes[handle].inuse == 0) {
    LockHandleRelease(inode_lock);
    return DFS_FAIL;
  }

  while (num_bytes > 0) {
    virtual_block = start_byte / sb.fs_blocksize;
    offset = start_byte % sb.fs_blocksize;

    bytes_to_write = sb.fs_blocksize - offset;
    if (bytes_to_write > num_bytes) {
      bytes_to_write = num_bytes;
    }

    was_allocated = (DfsInodeTranslateLocked(handle, virtual_block) != DFS_FAIL);

  
    // Allocate or translate the virtual block to a physical DFS block
    physical_block = DfsInodeAllocateVirtualBlockLocked(handle, virtual_block);
    if (physical_block == DFS_FAIL) {
      LockHandleRelease(inode_lock);
      return DFS_FAIL;
    }

    // If we are doing a partial write, we must read the existing block data first
    if (bytes_to_write < sb.fs_blocksize) {
      // If the block is brand new (indicated by the write extending file_size past block bounds),
      // there's no need to read.  But if modifying exising data, read it in.
      if (was_allocated && start_byte < inodes[handle].file_size) {
        if (DfsReadBlock(physical_block, &b) == DFS_FAIL) {
          LockHandleRelease(inode_lock);
          return DFS_FAIL;
        }
      }
      else {
        // New partial block: zero it to avoid garbage data
        bzero(b.data, sb.fs_blocksize);
      }
    }

    bcopy(src + bytes_written, b.data + offset, bytes_to_write);

    if (DfsWriteBlock(physical_block, &b) == DFS_FAIL) {
      LockHandleRelease(inode_lock);
      return DFS_FAIL;
    }

    bytes_written += bytes_to_write;
    start_byte += bytes_to_write;
    num_bytes -= bytes_to_write;

    // Updata file size if we wrote past current EOF
    if (start_byte > inodes[handle].file_size) {
      inodes[handle].file_size = start_byte;
    }
  }

  LockHandleRelease(inode_lock);
  return bytes_written;
}

