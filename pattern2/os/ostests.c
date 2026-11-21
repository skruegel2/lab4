#include "ostraps.h"
#include "dlxos.h"
#include "traps.h"
#include "disk.h"
#include "dfs.h"

void RunOSTests() {
  int inode_handle;
  int i;
  char write_buffer[4096];  // Can write max 4096 bytes at a time
  uint32 total_bytes_written = 0;
  uint32 target_bytes = 12288;  // 12KB (requires 12 DFS blocks)
  uint32 fbv_block_allocated;

  printf("Starting DFS Phase 3/4 Inode Tests\n");

  // 1. Open a new inode
  inode_handle = DfsInodeOpen("testfile.txt");
  if (inode_handle == DFS_FAIL) {
    printf("Test failed: DfsInodeOpen returned DFS_FAIL\n");
    return;
  }
  printf("Opened inode %d for 'testfile.txt'\n", inode_handle);

  // Fill write buffer with 'A'
  for(i = 0; i < 4096; i++) {
    write_buffer[i] = 'A';
  }

  // 2. Write 12 KB to the file (Spanning blocks 0 through 11)
  // Blocks 0-9 use direct addressing, blocks 10-11 require the single indirect table
  while (total_bytes_written < target_bytes) {
    if (DfsInodeWriteBytes(inode_handle, write_buffer, total_bytes_written, 4096) == DFS_FAIL) {
      printf("Test failed: DfsInodeWriteBytes failed at offset %d\n", total_bytes_written);
      return;
    }
    total_bytes_written += 4096;
  }
  printf("Successfully wrote 12KB to inode %d.\n", inode_handle);

  // 3. Verify the file size updated correctly
  if (DfsInodeFilesize(inode_handle) != 12288) {
    printf("Test failed: expected filesize 12288, got %d\n", DfsInodeFilesize(inode_handle));
    return;
  }

  // 4. Verify Phase 3: single indirect translation
  // Virtual block 11 should now translate to a valid DFS block number
  fbv_block_allocated = DfsInodeTranslateVirtualToFilesys(inode_handle, 11);
  if (fbv_block_allocated == DFS_FAIL || fbv_block_allocated == 0) {
    printf("Test failed: Virtual block 11 did not translate correctly.\n");
    return;
  }

  // 5. Test Phase 5: Deletion
  if (DfsInodeDelete(inode_handle) == DFS_FAIL) {
    printf("Test failed: DfsInodeDelete returned DFS_FAIL\n");
    return;
  }

  // Because blocks 41 through 53 were just freed,
  // allocating a new block should recycle the lowest available block (41)
  fbv_block_allocated = DfsAllocateBlock();
  if (fbv_block_allocated != 41) {
    printf("Test failed: Data blocks were not freed.  Expected block 41 to be available\n");
    return;
  }

  printf("All phase 3/4/5 tests passed\n");
}

