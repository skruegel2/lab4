#include "usertraps.h"
#include "misc.h"
#include "files_shared.h"

#define CHUNK_SIZE 4096
#define NUM_CHUNKS 9 // 9 * 4096 = 36 KB  (36 data blocks + 1 indirect block) = 37 DF blocks > 32 cache slots)

void main (int argc, char *argv[])
{
  int fd;
  char small_write[] = "Hello Buffer Cache!  Testing hits and write-back.";
  char small_read[100];
  char write_chunk[CHUNK_SIZE];
  char read_chunk[CHUNK_SIZE];
  int len;
  int c, i;
  int bytes_io;
  char expected;

  Printf("Starting Question 7 Buffer Cache test.\n");

  // Part 1: Single-block writeback and cache hit verification
  Printf("\n--- Part 1: Basic Cache Hit and Write-Back test\n");
  for(len = 0; small_write[len] != '\0'; len++) {}
  len++;

  // 1. Open and write small payload (expect 1 cache miss, disk reads = 4, disk writes = 0)
  fd = file_open("cache_basic.txt", "w");
  if (fd == FILE_FAIL) {
    Printf("FAIL: could not open 'cache_basic.txt' for writing.\n");
    Exit();
  }

  if (file_write(fd, small_write, len) != len) {
    Printf("FAIL: Write failed on 'cache_basic.txt'.\n");
    file_close(fd);
    Exit();
  }
  file_close(fd);

  // 2. Re-open and read back immediately (expect cache hit, no cache miss message printed)
  fd = file_open("cache_basic.txt", "r");
  if (fd == FILE_FAIL) {
    Printf("FAIL: Could not re-open 'cache_basic.txt' for reading.\n");
    Exit();
  }

  bzero(small_read, 100);
  if (file_read(fd, small_read, len) != len) {
    Printf("FAIL: read failed on cache_basic.txt.\n");
    file_close(fd);
    Exit();
  }
  file_close(fd);

  for (i = 0; i < len; i++) {
    if (small_read[i] != small_write[i]) {
      Printf("FAIL:  Part 1 mismatch at byte %d\n", i);
      Exit();
    }
  }
  Printf("Part 1 PASSED, verified cached read '%s' with zero extra misses.\n", small_read);

  // Part 2: 36 KB fifo eviction and dirty write-back stress test
  Printf("\n Part 2: 36KB fifo eviction and dirty write-back test\n");
  fd = file_open("cache_stress.bin", "w");
  if (fd == FILE_FAIL) {
    Printf("FAIL: could not open 'cache_stress.bin for writing\n");
    Exit();
  }

  // Write 9 chunks of 4 KB (36 KB total)
  // Once > 32 unique DFS blocks are touched, dirty evictions will increment by 4 each
  for(c = 0; c < NUM_CHUNKS; c++) {
    for (i = 0; i < CHUNK_SIZE; i++) {
      write_chunk[i] = (char)('A' + ((c + i) % 26));
    }

    bytes_io = file_write(fd, write_chunk, CHUNK_SIZE);
    if (bytes_io != CHUNK_SIZE) {
      Printf("FAIL: chunk %d write returned %d, expected %d.\n", c, bytes_io, CHUNK_SIZE);
      file_close(fd);
      Exit();
    }
  }
  file_close(fd);
  Printf("Successfully wrote 36 KB across 37 DFS blocks (forcing fifo dirty evictions)\n");

  // Reopen and read 36 KB back to verify both evicted blocks on disk and cached blocks
  fd = file_open("cache_stress.bin", "r");
  if (fd == FILE_FAIL) {
    Printf("FAIL: could not re-open 'cache_stress.bin' for reading.\n");
    Exit();
  }

  for(c = 0; c < NUM_CHUNKS; c++) {
    bzero(read_chunk, CHUNK_SIZE);
    bytes_io = file_read(fd, read_chunk, CHUNK_SIZE);
    if (bytes_io != CHUNK_SIZE) {
      Printf("FAIL: chunk %d read returned %d, expected %d\n", c, bytes_io, CHUNK_SIZE);
      file_close(fd);
      Exit();
    }

    for(i = 0; i < CHUNK_SIZE; i++) {
      expected = (char)('A' + ((c + i) % 26));
      if (read_chunk[i] != expected) {
        Printf("FAIL: data corruption at chunk %d, byte %d (expected '%c', got '%c')\n",
          c, i, expected, read_chunk[i]);
          file_close(fd);
          Exit();
      }
    }
  }
  file_close(fd);
  Printf("Part 2 PASSED: All 36 KB verified intact after FIFO evictions and disk reloads\n");

  Printf("All Question 7 buffer cache tests passed. (exiting will now trigger DfsCacheFlush.\n");
}
