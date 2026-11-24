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

static lock_t dfs_lock;
static lock_t cache_lock;

// Adaptive buffer cache configuration
#define MAX_PINNED_METADATA 10  // Cap pinned metadata slots so data slots never starve
#define AGING_MISS_INTERVAL 64  // Halve freqs every 64 misses for phase adaption

typedef struct dfs_cache_slot {
  int slot_index;   // Index (0..32) in the cache[] array
  int inuse;        // 1 if valid block is cached, 0 if empty
  int dirty;        // 1 if modified and needs write-back, 0 if clean
  int is_metadata;  // 1 if indirect/double-indirect block 0 if regular data block
  uint32 blocknum;  // DFS filesystem block num
  uint32 freq;      // Access frequency (aged periodically)
  uint32 last_used; // Monotonic timestamp of most recent access
  dfs_block data;   // block payload
} dfs_cache_slot;

// Virtual shadow slot
typedef struct shadow_slot {
  int inuse;
  int is_metadata;
  uint32 blocknum;
  uint32 freq;
  uint32 last_used;
} shadow_slot;

static dfs_cache_slot cache[DFS_CACHE_SLOTS];
// Queue replacements
static shadow_slot shadow_lfu[DFS_CACHE_SLOTS];
static shadow_slot shadow_mru[DFS_CACHE_SLOTS];

// Adaptive state variables
static uint32 access_clock = 0;
static uint32 miss_since_decay = 0;
static uint32 score_lfu = 1024; // Slight initial pref for LFU/LRU
static uint32 score_mru = 1024;

// Cumulative cache and physical disk counters
static uint32 cache_hits = 0;
static uint32 cache_misses = 0;
static uint32 disk_reads = 0;
static uint32 disk_writes = 0;

// Internal Prototypes
static void PrintCacheMissStats();
static void DfsCacheResetStateInternal();
static int DfsCacheHitInternal(int blocknum);
static int DfsCacheAllocateSlotInternal(int blocknum);
static void DfsMarkBlockMetadata(uint32 blocknum);
static int SelectVictimSlot(int use_mru, int is_shadow, shadow_slot *s_arr);
static int StepShadowCache(shadow_slot *s_arr, int use_mru, uint32 blocknum, uint32 now);
static void DfsAdaptiveObserve(uint32 blocknum, uint32 now);

///////////////////////////////////////////////////////////////////
// Internal adaptive helpers
///////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------
// Resets real cache, shadow cache, and adaptive scores
//-----------------------------------------------------------------
static void DfsCacheResetStateInternal() {
  int i;

  access_clock = 0;
  miss_since_decay = 0;
  score_lfu = 1024;
  score_mru = 1024;

  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    cache[i].slot_index = i;
    cache[i].inuse = 0;
    cache[i].dirty = 0;
    cache[i].is_metadata = 0;
    cache[i].blocknum = 0;
    cache[i].freq = 0;
    cache[i].last_used = 0;

    shadow_lfu[i].inuse = 0;
    shadow_lfu[i].is_metadata = 0;
    shadow_lfu[i].blocknum = 0;
    shadow_lfu[i].freq = 0;
    shadow_lfu[i].last_used = 0;

    shadow_mru[i].inuse = 0;
    shadow_mru[i].is_metadata = 0;
    shadow_mru[i].blocknum = 0;
    shadow_mru[i].freq = 0;
    shadow_mru[i].last_used = 0;
  }
}

//-----------------------------------------------------------------
// Selects an empty slot or eviction victim for either the shadow cache or 
// real cache.
// use_mru = 1: Evicts most recently used non metadata slot
// use_mru = 0: Evicts least frequently used non metadata slot, breaking ties with LRU
//-----------------------------------------------------------------
static int SelectVictimSlot(int use_mru, int is_shadow, shadow_slot* s_arr) {
  int i;
  int meta_count = 0;
  int skip_meta;
  int victim = -1;
  int inuse;
  int meta;
  uint32 freq;
  uint32 used;
  uint32 v_freq;
  uint32 v_used;

  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    inuse = is_shadow ? s_arr[i].inuse : cache[i].inuse;
    meta = is_shadow ? s_arr[i].is_metadata: cache[i].is_metadata;
    if (!inuse) {
      return i; // Prefer unused slot
    }
    if (meta) {
      meta_count++;
    }
  }

  // Safeguard indirect/double-indirect blocks unless metadata exceeds MAX_PINNED_METADATA 
  skip_meta = (meta_count < MAX_PINNED_METADATA) ? 1: 0;

  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    meta = is_shadow ? s_arr[i].is_metadata : cache[i].is_metadata;
    freq = is_shadow ? s_arr[i].freq : cache[i].freq;
    used = is_shadow ? s_arr[i].last_used: cache[i].last_used;

    if (skip_meta && meta) {
      continue;
    }

    if (victim == -1) {
      victim = i;
    }
    else {
      v_freq = is_shadow ? s_arr[victim].freq : cache[victim].freq;
      v_used = is_shadow ? s_arr[victim].last_used : cache[victim].last_used;

      if (use_mru) {
        // MRU: evict slot with newest timestamp
        if (used > v_used) {
          victim = i;
        }
      }
      else {
        // LFU with LRU tiebreaker; evict smallest freq, then oldest timestamp
        if (freq < v_freq || (freq == v_freq && used < v_used)) {
          victim = i;
        }
      }
    }
  }
  return (victim == -1) ? 0 : victim;
}

//-----------------------------------------------------------------
// Simulates an access in one shadow cache; returns 1 on hit, 0 on miss
//-----------------------------------------------------------------
static int StepShadowCache(shadow_slot *s_arr, int use_mru, uint32 blocknum, uint32 now)
{
  int i;
  int victim;

  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    if (s_arr[i].inuse && s_arr[i].blocknum == blocknum) {
      if (s_arr[i].freq < 0xFFFF0000) {
        s_arr[i].freq++;
      }
      s_arr[i].last_used = now;
      return 1;
    }
  }

  // Age LFU shadow freqs at the same cadence as the real cache
  if (!use_mru && ((miss_since_decay + 1) >= AGING_MISS_INTERVAL)) {
    for (i = 0; i < DFS_CACHE_SLOTS; i++) {
      if (s_arr[i].inuse && s_arr[i].freq > 1) {
        s_arr[i].freq >>= 1;
      }
    }
  }

  victim = SelectVictimSlot(use_mru, 1, s_arr);
  s_arr[victim].inuse = 1;
  s_arr[victim].is_metadata = 0;
  s_arr[victim].blocknum = blocknum;
  s_arr[victim].freq = 1;
  s_arr[victim].last_used = now;
  return 0;
}

//-----------------------------------------------------------------
// Update both shadow caches and decays/rewards
//-----------------------------------------------------------------
static void DfsAdaptiveObserve(uint32 blocknum, uint32 now) {
  int hit_lfu;
  int hit_mru;

  hit_lfu = StepShadowCache(shadow_lfu, 0, blocknum, now);
  hit_mru = StepShadowCache(shadow_mru, 1, blocknum, now);

  if (hit_lfu != hit_mru) {
    score_lfu = (score_lfu * 15) / 16;
    score_mru = (score_mru * 15) / 16;

    if (hit_lfu) {
      score_lfu += 1024;
    }
    if (hit_mru) {
      score_mru += 1024;
    }
  }
}

///////////////////////////////////////////////////////////////////
// Non-inode functions first
///////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------
// DfsModuleInit is called at boot time to initialize things and
// open the file system for use.
//-----------------------------------------------------------------
void DfsModuleInit() {

  dfs_lock = LockCreate();
  cache_lock = LockCreate();

  // Reset telemetry counters
  cache_hits = 0;
  cache_misses = 0;
  disk_reads = 0;
  disk_writes = 0;

  // Initialize all 32 cache slots and shadow structures
  DfsCacheResetStateInternal();

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

  // Read superblock from physical block 1
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

  // Ensure clean cache state when mounting
  LockHandleAcquire(cache_lock);
  DfsCacheResetStateInternal();
  LockHandleRelease(cache_lock);

  // Calculate conversion ratio from DFS blocks to Physical Blocks
  phys_block_ratio = sb.fs_blocksize / DiskBytesPerBlock();

  // Read inodes
  inode_ptr = (char*)inodes;
  phys_block_start = sb.inode_start_block * phys_block_ratio;
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
// DfsAllocateBlock allocates a DFS block for use.
//-----------------------------------------------------------------
int DfsAllocateBlock() {
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
  LockHandleAcquire(dfs_lock);

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
  LockHandleRelease(dfs_lock);

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
  int i;

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
  LockHandleAcquire(dfs_lock);

  // Use bitwise AND to clear the specific bit to 0 (free)
  fbv[array_index] &= mask;

  // Release lock
  LockHandleRelease(dfs_lock);

  // Evict and discard if this freed block is currently in the buffer cache
  LockHandleAcquire(cache_lock);
  slot = DfsCacheHitInternal(blocknum);
  if (slot != DFS_FAIL) {
    cache[slot].inuse = 0;
    cache[slot].dirty = 0;
    cache[slot].is_metadata = 0;
    cache[slot].blocknum = 0;
    cache[slot].freq = 0;
    cache[slot].last_used = 0;
  }
  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    if (shadow_lfu[i].inuse && shadow_lfu[i].blocknum == blocknum) {
      shadow_lfu[i].inuse = 0;
      shadow_lfu[i].is_metadata = 0;
      shadow_lfu[i].blocknum = 0;
      shadow_lfu[i].freq = 0;
      shadow_lfu[i].last_used = 0;
    }
    if (shadow_mru[i].inuse && shadow_mru[i].blocknum == blocknum) {
      shadow_mru[i].inuse = 0;
      shadow_mru[i].is_metadata = 0;
      shadow_mru[i].blocknum = 0;
      shadow_mru[i].freq = 0;
      shadow_mru[i].last_used = 0;
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
// DfsReadBlock checks the buffer cache for blocknum, loading it
// from disk on a miss, and copies sb.fs_blocksize bytes into b
//-----------------------------------------------------------------
int DfsReadBlock(uint32 blocknum, dfs_block *b) {
  int slot;
  int i;

  if (sb.valid == 0 || blocknum >= sb.total_fs_blocks) {
    return DFS_FAIL;
  }

  LockHandleAcquire(cache_lock);

  access_clock++;
  DfsAdaptiveObserve(blocknum, access_clock);

  slot = DfsCacheHitInternal(blocknum);
  if (slot != DFS_FAIL) {
    cache_hits++;    
    if (cache[slot].freq < 0xFFFF0000) {
      cache[slot].freq++;
    }
    cache[slot].last_used = access_clock;
    dbprintf('b', "Cache Hit: Block %d found in slot %d (Hits=%d, Misses=%d)\n",
      blocknum, slot, cache_hits, cache_misses);
  }
  else {
    cache_misses++;
    miss_since_decay++;
    if (miss_since_decay >= AGING_MISS_INTERVAL) {
      miss_since_decay = 0;
      for (i = 0; i < DFS_CACHE_SLOTS; i++) {
        if (cache[i].inuse && cache[i].freq > 1) {
          cache[i].freq >>= 1;
        }
      }
    }

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

    PrintCacheMissStats();
  }

  bcopy(cache[slot].data.data, b->data, sb.fs_blocksize);

  LockHandleRelease(cache_lock);
  return sb.fs_blocksize;
}

//-----------------------------------------------------------------
// DfsWriteBlock checks the buffer cache for blocknum, reading it
// from disk into the cache on a miss, updates the cached copy, and 
// marks the slot dirty
//-----------------------------------------------------------------
int DfsWriteBlock(uint32 blocknum, dfs_block * b) {
  int slot;
  int i;

  if (sb.valid == 0 || blocknum >= sb.total_fs_blocks) {
    return DFS_FAIL;
  }

  LockHandleAcquire(cache_lock);

  access_clock++;
  DfsAdaptiveObserve(blocknum, access_clock);

  slot = DfsCacheHitInternal(blocknum);
  if (slot != DFS_FAIL) {
    cache_hits++;
    if (cache[slot].freq < 0xFFFF0000) {
      cache[slot].freq++;
    }
    cache[slot].last_used = access_clock;
    dbprintf('b', "Cache Hit: Block %d found in slot %d (Hits=%d, Misses=%d)\n",
      blocknum, slot, cache_hits, cache_misses);
  }
  else {
    cache_misses++;
    miss_since_decay++;
    if (miss_since_decay >= AGING_MISS_INTERVAL) {
      miss_since_decay = 0;
      for (i = 0; i < DFS_CACHE_SLOTS; i++) {
        if (cache[i].inuse && cache[i].freq > 1) {
          cache[i].freq >>= 1;
        }
      }
    }

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

    PrintCacheMissStats();
  }

  bcopy(b->data, cache[slot].data.data, sb.fs_blocksize);
  cache[slot].dirty = 1;

  LockHandleRelease(cache_lock);
  return sb.fs_blocksize;
}

///-----------------------------------------------------------------
// DfsCacheFlush writes all dirty cached blocks back to disk and
// returns all occupied slots to free_cache_q without counting
// hits or misses.
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
          cache[i].blocknum, cache[i].slot_index);
        if (DfsWriteBlockUncached(cache[i].blocknum, &cache[i].data) == DFS_FAIL) {
          LockHandleRelease(cache_lock);
          return DFS_FAIL;
        }
        cache[i].dirty = 0;
      }
    }
  }
  
  DfsCacheResetStateInternal();
  LockHandleRelease(cache_lock);
  return DFS_SUCCESS;
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
  int i;

  // Check that the file system has been loaded and is valid.
  if (sb.valid == 0) {
    return DFS_FAIL;
  }

  // Iterate through all allocated inodes to find a matching filename
  for(i = 0; i < sb.num_inodes; i++) {
    if (inodes[i].inuse == 1) {
      if (dstrncmp(inodes[i].filename, filename, FILE_MAX_FILENAME_LENGTH) == 0) {
        return i; // Return the inode array index as the handle
      }
    }
  }

  return DFS_FAIL;
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
  if (sb.valid == 0) {
    return DFS_FAIL;
  }  

  // Acquire lock before modifying shared inodes array
  LockHandleAcquire(dfs_lock);

  // Check inside lock as well
  for (i = 0; i < sb.num_inodes; i++) {
    if (inodes[i].inuse == 1 &&
        dstrncmp(inodes[i].filename, filename, FILE_MAX_FILENAME_LENGTH) == 0) {
      LockHandleRelease(dfs_lock);
      return i;
    }
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
  LockHandleRelease(dfs_lock);
  
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
  if (sb.valid == 0 || handle >= (uint32)sb.num_inodes || inodes[handle].inuse == 0) {
    return DFS_FAIL;
  } 

  entries_per_block = sb.fs_blocksize / sizeof(int);

  // 1. Calculate the max number of virtual blocks used by the file
  if (inodes[handle].file_size == 0) {
    max_vblocks = 0;
  }
  else {
    // Round up to the nearest whole block
    max_vblocks = (inodes[handle].file_size + sb.fs_blocksize - 1) / sb.fs_blocksize;
  }

  // 2. Free all allocated data blocks
  for (i = 0; i < max_vblocks; i++) {
    phys_block = DfsInodeTranslateVirtualToFilesys(handle, i);
    if (phys_block != DFS_FAIL && phys_block != 0) {
      DfsFreeBlock(phys_block);
    }    
  }

  // 3. Free the single indirect table itself
  if (inodes[handle].indirect != 0) {
    DfsFreeBlock(inodes[handle].indirect);
  }

  // 4. Free the double indirect table blocks
  if (inodes[handle].double_indirect != 0) {
    // Read the top-level table to find and free the mid-level tables
    if (DfsReadBlock(inodes[handle].double_indirect, &b) != DFS_FAIL) {
      table = (int*)b.data;
      // The table holds DFS_BLOCKSIZE / 4 bytes = 256 entries
      for(i = 0; i < entries_per_block; i++) {
        if (table[i] != 0) {
          DfsFreeBlock(table[i]);
        }
      }
    }
    // Free the top-level table block
    DfsFreeBlock(inodes[handle].double_indirect);
  }

  // 5. Mark the inodes as free
  LockHandleAcquire(dfs_lock);
  inodes[handle].file_size = 0;
  for (i = 0; i < 10; i++) {
    inodes[handle].direct[i] = 0;
  }
  inodes[handle].indirect = 0;
  inodes[handle].double_indirect = 0;
  inodes[handle].inuse = 0;
  LockHandleRelease(dfs_lock);

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

  // Ensure the new filename isn't already in use
  if (DfsInodeFilenameExists(newname) != DFS_FAIL) {
    return DFS_FAIL;
  }

  // Find the existing file
  handle = DfsInodeFilenameExists(oldname);
  if (handle == DFS_FAIL) {
    return DFS_FAIL;
  }

  // Acquire lock to safely modify the inode data
  LockHandleAcquire(dfs_lock);

  for(i = 0; i < FILE_MAX_FILENAME_LENGTH; i++) {
    inodes[handle].filename[i] = newname[i];
    if (newname[i] == '\0') {
      break;
    }
  }

  // Guarantee null termination
  inodes[handle].filename[FILE_MAX_FILENAME_LENGTH - 1] = '\0';

  LockHandleRelease(dfs_lock);

  return DFS_SUCCESS;
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
  if (sb.valid == 0 || mem == NULL || start_byte < 0 || num_bytes < 0 ||
    handle >= sb.num_inodes || inodes[handle].inuse == 0) {
    return DFS_FAIL;
  }

  // Prevent reading past the actual file size
  if (start_byte >= inodes[handle].file_size) {
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

    physical_block = DfsInodeTranslateVirtualToFilesys(handle, virtual_block);
    if (physical_block == DFS_FAIL) {
      // Sparse hole 
      bzero(dest + bytes_read, bytes_to_read);
    }
    else {
      if (DfsReadBlock(physical_block, &b) == DFS_FAIL) {
        return DFS_FAIL;
      }
      bcopy(b.data + offset, dest + bytes_read, bytes_to_read);
    }

    bytes_read += bytes_to_read;
    start_byte += bytes_to_read;
    num_bytes -= bytes_to_read;
  }

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
  int existing_block;
  int physical_block;
  int bytes_to_write;
  int bytes_written = 0;
  char *src = (char*)mem;

  // Validate filesystem and inode
  if (sb.valid == 0 || mem == NULL || start_byte < 0 || num_bytes < 0 ||
     handle >= sb.num_inodes || inodes[handle].inuse == 0) {
    return DFS_FAIL;
  }

  while (num_bytes > 0) {
    virtual_block = start_byte / sb.fs_blocksize;
    offset = start_byte % sb.fs_blocksize;

    bytes_to_write = sb.fs_blocksize - offset;
    if (bytes_to_write > num_bytes) {
      bytes_to_write = num_bytes;
    }

    // Check whether this virutual block existed prior to allocation
    existing_block = DfsInodeTranslateVirtualToFilesys(handle, virtual_block);

    // Allocate or translate the virtual block to a physical DFS block
    physical_block = DfsInodeAllocateVirtualBlock(handle, virtual_block);
    if (physical_block == DFS_FAIL) {
      return DFS_FAIL;
    }

    // Read modify write if updating partial block already allocated
    if (bytes_to_write < sb.fs_blocksize) {
      if (existing_block != DFS_FAIL) {
        if (DfsReadBlock(physical_block, &b) == DFS_FAIL) {
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

  return bytes_written;
}


//-----------------------------------------------------------------
// DfsInodeFilesize simply returns the size of an inode's file. 
// This is defined as the maximum virtual byte number that has 
// been written to the inode thus far. Return DFS_FAIL on failure.
//-----------------------------------------------------------------
int DfsInodeFilesize(uint32 handle) {
  // Validate filesystem and inode handle
  if (sb.valid == 0 || handle >= sb.num_inodes || inodes[handle].inuse == 0) {
    return DFS_FAIL;
  }

  return inodes[handle].file_size;
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
  dfs_block b;
  int* table;
  uint32 new_block;
  uint32 indirect_block;
  uint32 single_indirect_index;
  uint32 data_index;
  uint32 entries_per_block;
  uint32 max_vblocks;

  // Check that the file system has been loaded and is valid.
  if (sb.valid == 0 || handle >= (uint32)sb.num_inodes || inodes[handle].inuse == 0) {
    return DFS_FAIL;
  }   

  entries_per_block = sb.fs_blocksize / sizeof(int);
  max_vblocks = 10 + entries_per_block + (entries_per_block * entries_per_block);

  if (virtual_blocknum >= max_vblocks) {
    return DFS_FAIL;
  }

  // Return existing block if already allocated
  new_block = DfsInodeTranslateVirtualToFilesys(handle, virtual_blocknum);
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
        DfsFreeBlock(new_block);  // Rollback data allocation
        return DFS_FAIL;
      }
      // Zero out the newly created indirect block
      bzero(b.data, sb.fs_blocksize);
      if (DfsWriteBlock(inodes[handle].indirect, &b) == DFS_FAIL) {
        DfsFreeBlock(inodes[handle].indirect);
        inodes[handle].indirect = 0;
        DfsFreeBlock(new_block);
        return DFS_FAIL;
      }
      DfsMarkBlockMetadata(inodes[handle].indirect);
    }

    // Read the indirect block, update the table, and write it back
    if (DfsReadBlock(inodes[handle].indirect, &b) == DFS_FAIL) {
      return DFS_FAIL;
    }
    DfsMarkBlockMetadata(inodes[handle].indirect);


    table = (int*)b.data;
    table[virtual_blocknum - 10] = new_block;

    if (DfsWriteBlock(inodes[handle].indirect, &b) == DFS_FAIL) {
      return DFS_FAIL;
    }
    DfsMarkBlockMetadata(inodes[handle].indirect);
    return new_block;
  }

  // 3. Double indirect addressing 
  single_indirect_index = (virtual_blocknum - (10 + entries_per_block)) / entries_per_block;
  data_index = (virtual_blocknum - (10 + entries_per_block)) % entries_per_block;

  // Allocate top-level double indirect table if missing
  if (inodes[handle].double_indirect == 0) {
    inodes[handle].double_indirect = DfsAllocateBlock();
    if (inodes[handle].double_indirect == DFS_FAIL) {
      DfsFreeBlock(new_block);
      return DFS_FAIL;
    }
    bzero(b.data, sb.fs_blocksize);
    if (DfsWriteBlock(inodes[handle].double_indirect, & b) == DFS_FAIL) {
      DfsFreeBlock(inodes[handle].double_indirect);
      inodes[handle].double_indirect = 0;
      DfsFreeBlock(new_block);
      return DFS_FAIL;
    }
    DfsMarkBlockMetadata(inodes[handle].double_indirect);
  }

  // Read the top-level table
  if (DfsReadBlock(inodes[handle].double_indirect, &b) == DFS_FAIL) {
    return DFS_FAIL;
  }
  DfsMarkBlockMetadata(inodes[handle].double_indirect);
  
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
      DfsFreeBlock(indirect_block);
      DfsFreeBlock(new_block);
      return DFS_FAIL;
    }
    DfsMarkBlockMetadata(inodes[handle].double_indirect);


    // Zero out the new mid-level block and write it to disk
    bzero(b.data, sb.fs_blocksize);
    if (DfsWriteBlock(indirect_block, &b) == DFS_FAIL) {
      return DFS_FAIL;
    }
    DfsMarkBlockMetadata(indirect_block);
  }

  // Read the mid-level table, insert the new data block number, and write it back
  if (DfsReadBlock(indirect_block, &b) == DFS_FAIL) {
    return DFS_FAIL;
  }
  DfsMarkBlockMetadata(indirect_block);

  table = (int*)b.data;
  table[data_index] = new_block;

  if (DfsWriteBlock(indirect_block, &b) == DFS_FAIL) {
    return DFS_FAIL;
  }

  DfsMarkBlockMetadata(indirect_block);
  return new_block;
}

//-----------------------------------------------------------------
// DfsInodeTranslateVirtualToFilesys translates the 
// virtual_blocknum to the corresponding file system block using 
// the inode identified by handle. Return DFS_FAIL on failure.
//-----------------------------------------------------------------
int DfsInodeTranslateVirtualToFilesys(uint32 handle, uint32 virtual_blocknum) {
  dfs_block b;
  int *table;
  uint32 indirect_blocknum;
  uint32 single_indirect_index;
  uint32 data_index;
  uint32 block_result;
  uint32 entries_per_block;
  uint32 max_vblocks;

  // Check that the file system has been loaded and is valid.
  if (sb.valid == 0 || handle >= (uint32)sb.num_inodes || inodes[handle].inuse == 0) {
    return DFS_FAIL;
  }  

  entries_per_block = sb.fs_blocksize / sizeof(int);
  max_vblocks = 10 + entries_per_block + (entries_per_block * entries_per_block);

  if (virtual_blocknum >= max_vblocks) {
    return DFS_FAIL;
  }

  // 1. Direct addressing
  if (virtual_blocknum < 10) {
    block_result = inodes[handle].direct[virtual_blocknum];
    if (block_result == 0) {
      return DFS_FAIL;
    }
    return block_result;
  }

  // 2. Single indirect addressing 
  if (virtual_blocknum < (10 + entries_per_block)) {
    if (inodes[handle].indirect == 0) {
      return DFS_FAIL;  // Indirect table not allocated
    }

    // Read the single indirect table block
    if (DfsReadBlock(inodes[handle].indirect, &b) == DFS_FAIL) {
      return DFS_FAIL;
    }
    DfsMarkBlockMetadata(inodes[handle].indirect);

    // Cast the raw block data into an array of integers
    table = (int*)b.data;
    block_result = table[virtual_blocknum - 10];

    if (block_result == 0) {
      return DFS_FAIL;
    }
    return block_result;
  }

  // 3. Double indirect addressing 
  if (inodes[handle].double_indirect == 0) {
    return DFS_FAIL;
  }

  // Read the double indirect table block
  if (DfsReadBlock(inodes[handle].double_indirect, &b) == DFS_FAIL) {
    return DFS_FAIL;
  }
  DfsMarkBlockMetadata(inodes[handle].double_indirect);

  table = (int*)b.data;

  single_indirect_index = (virtual_blocknum - (10 + entries_per_block)) / entries_per_block;
  data_index = (virtual_blocknum - (10 + entries_per_block)) % entries_per_block;

  indirect_blocknum = table[single_indirect_index];
  if (indirect_blocknum == 0) {
    return DFS_FAIL;  // Midlevel single indirect table not allocated
  }

  // Read the midlevel single indirect table block
  if (DfsReadBlock(indirect_blocknum, &b) == DFS_FAIL) {
    return DFS_FAIL;
  }
  DfsMarkBlockMetadata(indirect_blocknum);

  table = (int*)b.data;
  block_result = table[data_index];

  if (block_result == 0) {
    return DFS_FAIL;
  }
  return block_result;
}

//-----------------------------------------------------------------
// Internal helper: prints cumulative cache miss and disk I/O stats
// Format: Cache Miss: Hit Rate = XX.XXX%, Miss Rate = XX.XXX%,
// Disk Reads = N, Disk Writes = M 
//-----------------------------------------------------------------
static void PrintCacheMissStats() {
  uint32 total = cache_hits + cache_misses;
  uint32 hit_scaled;
  uint32 miss_scaled;

  if (total == 0) {
    return;
  }

  // Compute percentages scaled by 1000
  hit_scaled = (uint32)((((double)cache_hits * 100000.0) / (double)total) + 0.5);
  miss_scaled = 100000 - hit_scaled;

  printf("Cache Miss: Hit Rate = %d.%03d%%, Miss Rate = %d.%03d%%, Disk Reads = %d, Disk Writes = %d\n",
          hit_scaled / 1000, hit_scaled % 1000,
          miss_scaled / 1000, miss_scaled % 1000,
          disk_reads, disk_writes);
}

//-----------------------------------------------------------------
// Helper function
//-----------------------------------------------------------------
static void DfsMarkBlockMetadata(uint32 blocknum) {
  int slot;
  int i;

  LockHandleAcquire(cache_lock);
  slot = DfsCacheHitInternal(blocknum);
  if (slot != DFS_FAIL) {
    cache[slot].is_metadata = 1;
  }

  for (i = 0; i < DFS_CACHE_SLOTS; i++) {
    if (shadow_lfu[i].inuse && shadow_lfu[i].blocknum == blocknum) {
      shadow_lfu[i].is_metadata = 1;
    }
    if (shadow_mru[i].inuse && shadow_mru[i].blocknum == blocknum) {
      shadow_mru[i].is_metadata = 1;
    }
  }
  LockHandleRelease(cache_lock);
}

//-----------------------------------------------------------------
// Internal unlocked helper for DfsCacheHit (caller holds cache lock)
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
// Internal unlocked helper for DfsCacheAllocateSlot (caller holds cache_lock)
//-----------------------------------------------------------------
static int DfsCacheAllocateSlotInternal(int blocknum) {
  int use_mru;
  int victim;
  dfs_cache_slot *slot_ptr;

  // Choose the currently winning policy
  use_mru = (score_mru > score_lfu) ? 1 : 0;
  victim = SelectVictimSlot(use_mru, 0, NULL);
  slot_ptr = &cache[victim];

  // Write bck to disk if evicting a valid dirty block
  if (slot_ptr->inuse == 1 && slot_ptr->dirty == 1) {
    dbprintf('b', "Cache Evict (%s): Writing back dirty block %d from slot %d\n",
        use_mru ? "MRU": "LFU", slot_ptr->blocknum, slot_ptr->slot_index);
    if (DfsWriteBlockUncached(slot_ptr->blocknum, &slot_ptr->data) == DFS_FAIL) {
      return DFS_FAIL;
    }
    slot_ptr->dirty = 0;
  }

  slot_ptr->inuse = 1;
  slot_ptr->dirty = 0;
  slot_ptr->is_metadata = 0;
  slot_ptr->blocknum = (uint32)blocknum;
  slot_ptr->freq = 1;
  slot_ptr->last_used = access_clock;

  return slot_ptr->slot_index;
}

//-----------------------------------------------------------------
// DfsCacheAllocateSlot
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

