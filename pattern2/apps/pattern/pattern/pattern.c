#include "usertraps.h"
#include "misc.h"
#include "files_shared.h"

//---------------------------------------------------------------------
// Pattern 2
//---------------------------------------------------------------------

// Define file system and test parameters
#define BLOCK_SIZE          1024
#define INT_SIZE            ((int)sizeof(int))
#define INTS_PER_BLOCK      (BLOCK_SIZE / INT_SIZE)   // 1024 / 4 = 256

#define P2_NUM_FILES        10
#define P2_BLOCKS_PER_FILE  4                         // must stay <= 10
#define P2_WORKING_SET      (P2_NUM_FILES * P2_BLOCKS_PER_FILE)
#define P2_ITERATIONS       200

// One name per file.  Keep this table in sync with P2_NUM_FILES.
static char *p2_names[P2_NUM_FILES] = {
  "p2_0.dat", "p2_1.dat", "p2_2.dat", "p2_3.dat", "p2_4.dat",
  "p2_5.dat", "p2_6.dat", "p2_7.dat", "p2_8.dat", "p2_9.dat"
};

// First integer stored in block b of file f.  Used both to fill the
// blocks during setup and to verify them during the loop.
#define P2_TAG(f, b)  (((f) * 1000000) + ((b) * INTS_PER_BLOCK))

void main(int argc, char *argv[])
{
  int handles[P2_NUM_FILES];
  int block_buf[INTS_PER_BLOCK];  // 1KB buffer, one whole DFS block
  int f;
  int b;
  int j;
  int it;
  int written;
  int read_bytes;
  int error_count;

  error_count = 0;

  // ------------------- Setup: create the working set -------------------
  Printf("pattern2: Creating %d files of %d blocks each (%d blocks total).\n",
         P2_NUM_FILES, P2_BLOCKS_PER_FILE, P2_WORKING_SET);

  for (f = 0; f < P2_NUM_FILES; f++) {
    handles[f] = file_open(p2_names[f], "w");
    if (handles[f] == FILE_FAIL) {
      Printf("pattern2: Failed to open %s for writing!\n", p2_names[f]);
      Exit();
    }

    for (b = 0; b < P2_BLOCKS_PER_FILE; b++) {
      // Fill the buffer with a predictable pattern
      for (j = 0; j < INTS_PER_BLOCK; j++) {
        block_buf[j] = P2_TAG(f, b) + j;
      }
      written = file_write(handles[f], (void *)block_buf, BLOCK_SIZE);
      if (written != BLOCK_SIZE) {
        Printf("pattern2: Error: Short write (%d) on file %d, block %d!\n",
               written, f, b);
        file_close(handles[f]);
        Exit();
      }
    }

    if (file_close(handles[f]) == FILE_FAIL) {
      Printf("pattern2: Failed to close %s after writing!\n", p2_names[f]);
      Exit();
    }
  }

  // ------------------- Re-open the whole set for reading -------------------
  for (f = 0; f < P2_NUM_FILES; f++) {
    handles[f] = file_open(p2_names[f], "r");
    if (handles[f] == FILE_FAIL) {
      Printf("pattern2: Failed to open %s for reading!\n", p2_names[f]);
      Exit();
    }
  }

  // ------------------------- Pattern 2 -------------------------
  Printf("pattern2: Starting Pattern 2: %d iterations over %d blocks.\n",
         P2_ITERATIONS, P2_WORKING_SET);

  for (it = 0; it < P2_ITERATIONS; it++) {
    for (f = 0; f < P2_NUM_FILES; f++) {

      // Rewind to the start of this file for the next pass
      if (file_seek(handles[f], 0, FILE_SEEK_SET) != FILE_SUCCESS) {
        Printf("pattern2: Error: Seek failed on file %d, iteration %d!\n",
               f, it);
        error_count++;
        break;
      }

      for (b = 0; b < P2_BLOCKS_PER_FILE; b++) {
        read_bytes = file_read(handles[f], (void *)block_buf, BLOCK_SIZE);

        if (read_bytes != BLOCK_SIZE) {
          Printf("pattern2: Error: Short read (%d) on file %d, block %d, iteration %d!\n",
                 read_bytes, f, b, it);
          error_count++;
          break;
        }

        if (block_buf[0] != P2_TAG(f, b)) {
          Printf("pattern2: Incorrect data in file %d, block %d! Expected %d, Got %d\n",
                 f, b, P2_TAG(f, b), block_buf[0]);
          error_count++;
        }
      }
    }
  }

  Printf("pattern2: Pattern 2 complete. Total errors: %d\n", error_count);

  // -------------------------  Closing stuff -------------------------
  for (f = 0; f < P2_NUM_FILES; f++) {
    if (file_close(handles[f]) == FILE_FAIL) {
      Printf("pattern2: Failed to close %s.\n", p2_names[f]);
    }
    if (file_delete(p2_names[f]) == FILE_FAIL) {
      Printf("pattern2: Failed to delete %s.\n", p2_names[f]);
    }
  }

  Printf("pattern2: Done.\n");
}
