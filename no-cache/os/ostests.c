#include "ostraps.h"
#include "dlxos.h"
#include "traps.h"
#include "disk.h"
#include "dfs.h"

void RunOSTests() {
  uint32 blocknum;
  dfs_block write_b;
  dfs_block read_b;
  int i;

  printf("Starting DFS driver tests\n");

  // 1. Test allocation
  blocknum = DfsAllocateBlock();
  if (blocknum == DFS_FAIL) {
    printf("Test failed: DfsAllocateBlock returned DFS_FAIL.\n");
    return;
  }
  printf("Allocated DFS block %d\n", blocknum);
  // Expected: should be 41 or higher, since blocks 0-40 are reserved.

  // 2. Test Write
  // Fill the DFS block (1024 bytes) with a recognizable pattern.
  for(i = 0; i < 1024; i++) {
    write_b.data[i] = 'Z';
  }

  if (DfsWriteBlock(blocknum, &write_b) == DFS_FAIL) {
    printf("Test failed: DfsWriteBlock failed.\n");
    return;
  }
  printf("Successfully wrote to DFS block %d.\n", blocknum);

  // 3. Test read
  // Clear the read buffer first so we are getting a fresh read
  bzero(read_b.data, 1024);

  if (DfsReadBlock(blocknum, &read_b) == DFS_FAIL)
  {
    printf("Test failed, DfsReadBlock failed.\n");
    return;
  }

  // Verify the data matches
  for(i = 0; i < 1024; i++) {
    if (read_b.data[i] != 'Z') {
      printf("Test failed. Data mismatch at byte %d. Expected 'Z' got '%c'\n", i, read_b.data[i]);
      return;
    }
  }
  printf("Successfully read and verified data from DFS block %d.\n", blocknum);

  // 4. Test deallocation
  if (DfsFreeBlock(blocknum) == DFS_FAIL) {
    printf("Test failed: DfsFreeBlock failed\n");
    return;
  }
  printf("Successfully freed DFS block %d.\n", blocknum);
  printf("All internal DFS block tests passed.\n");
}

