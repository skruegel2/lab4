#include "usertraps.h"
#include "misc.h"
#include "files_shared.h"

//---------------------------------------------------------------------
// Pattern 3: 
//---------------------------------------------------------------------

// Define file system and test parameters
#define BLOCK_SIZE          1024
#define INT_SIZE            ((int)sizeof(int))
#define INTS_PER_BLOCK      (BLOCK_SIZE / INT_SIZE)   // 1024 / 4 = 256

#define P3_NUM_FILES        12
#define P3_BLOCKS_PER_FILE  9                         // must stay <= 9
#define P3_UNIVERSE         (P3_NUM_FILES * P3_BLOCKS_PER_FILE)   // 108 blocks
#define P3_HOT_BLOCKS       28                        // just under the cache
#define P3_HOT_PERCENT      70                        // % of accesses to hot set
#define P3_NUM_REQUESTS     40000

// One name per file.  Keep this table in sync with P3_NUM_FILES.
static char *p3_names[P3_NUM_FILES] = {
  "p3_0.dat",  "p3_1.dat",  "p3_2.dat",  "p3_3.dat",
  "p3_4.dat",  "p3_5.dat",  "p3_6.dat",  "p3_7.dat",
  "p3_8.dat",  "p3_9.dat",  "p3_10.dat", "p3_11.dat"
};

// First integer stored in block b of file f.  Used both to fill the
// blocks during setup and to verify them during the request loop.
#define P3_TAG(f, b)  (((f) * 1000000) + ((b) * INTS_PER_BLOCK))

static unsigned int lcg_seed = 1;

//---------------------------------------------------------------------
// get_num returns a pseudo-random value in [0, max_val).
//---------------------------------------------------------------------
int get_num(int max_val)
{
  lcg_seed = (lcg_seed * 1103515245 + 12345) & 0x7FFFFFFF;
  if (max_val == 0) {
    return 0;
  }
  return (int)((lcg_seed >> 16) % (unsigned int)max_val);
}

void main(int argc, char *argv[])
{
  int handles[P3_NUM_FILES];
  int block_buf[INTS_PER_BLOCK];  // 1KB buffer, one whole DFS block
  int f;
  int b;
  int j;
  int i;
  int g;
  int written;
  int read_bytes;
  int error_count;

  error_count = 0;

  // ------------------- Setup: create the working set -------------------
  Printf("pattern3: Creating %d files of %d blocks each (%d blocks total).\n",
         P3_NUM_FILES, P3_BLOCKS_PER_FILE, P3_UNIVERSE);

  for (f = 0; f < P3_NUM_FILES; f++) {
    handles[f] = file_open(p3_names[f], "w");
    if (handles[f] == FILE_FAIL) {
      Printf("pattern3: Failed to open %s for writing!\n", p3_names[f]);
      Exit();
    }

    for (b = 0; b < P3_BLOCKS_PER_FILE; b++) {
      // Fill the buffer with a predictable pattern
      for (j = 0; j < INTS_PER_BLOCK; j++) {
        block_buf[j] = P3_TAG(f, b) + j;
      }
      written = file_write(handles[f], (void *)block_buf, BLOCK_SIZE);
      if (written != BLOCK_SIZE) {
        Printf("pattern3: Error: Short write (%d) on file %d, block %d!\n",
               written, f, b);
        file_close(handles[f]);
        Exit();
      }
    }

    if (file_close(handles[f]) == FILE_FAIL) {
      Printf("pattern3: Failed to close %s after writing!\n", p3_names[f]);
      Exit();
    }
  }

  // ------------------- Re-open the whole set for reading -------------------
  for (f = 0; f < P3_NUM_FILES; f++) {
    handles[f] = file_open(p3_names[f], "r");
    if (handles[f] == FILE_FAIL) {
      Printf("pattern3: Failed to open %s for reading!\n", p3_names[f]);
      Exit();
    }
  }

  // ------------------------- Pattern 3 -------------------------
  Printf("pattern3: Starting Pattern 3: %d requests, %d%% of them into a hot set of %d blocks.\n",
         P3_NUM_REQUESTS, P3_HOT_PERCENT, P3_HOT_BLOCKS);

  for (i = 0; i < P3_NUM_REQUESTS; i++) {

    // Pick a global block index: mostly from the hot set, occasionally
    // from the cold remainder of the universe.
    if (get_num(100) < P3_HOT_PERCENT) {
      g = get_num(P3_HOT_BLOCKS);
    } else {
      g = P3_HOT_BLOCKS + get_num(P3_UNIVERSE - P3_HOT_BLOCKS);
    }

    // Map that global index onto (file, block within file)
    f = g / P3_BLOCKS_PER_FILE;
    b = g % P3_BLOCKS_PER_FILE;

    if (file_seek(handles[f], b * BLOCK_SIZE, FILE_SEEK_SET) != FILE_SUCCESS) {
      Printf("pattern3: Error: Seek failed on file %d, block %d, request %d!\n",
             f, b, i);
      error_count++;
      break;
    }

    read_bytes = file_read(handles[f], (void *)block_buf, BLOCK_SIZE);

    if (read_bytes != BLOCK_SIZE) {
      Printf("pattern3: Error: Short read (%d) on file %d, block %d, request %d!\n",
             read_bytes, f, b, i);
      error_count++;
      break;
    }

    if (block_buf[0] != P3_TAG(f, b)) {
      Printf("pattern3: Incorrect data in file %d, block %d! Expected %d, Got %d\n",
             f, b, P3_TAG(f, b), block_buf[0]);
      error_count++;
    }
  }

  Printf("pattern3: Pattern 3 complete. Total errors: %d\n", error_count);

  // -------------------------  Closing stuff -------------------------
  for (f = 0; f < P3_NUM_FILES; f++) {
    if (file_close(handles[f]) == FILE_FAIL) {
      Printf("pattern3: Failed to close %s.\n", p3_names[f]);
    }
    if (file_delete(p3_names[f]) == FILE_FAIL) {
      Printf("pattern3: Failed to delete %s.\n", p3_names[f]);
    }
  }

  Printf("pattern3: Done.\n");
}
