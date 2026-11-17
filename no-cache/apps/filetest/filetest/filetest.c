#include "usertraps.h"
#include "misc.h"
#include "files_shared.h"

void main (int argc, char *argv[])
{
  int fd;
  char write_data[] = "Hello DFS!  This is a test of the user file API.";
  char read_data[100];
  int bytes_written;
  int bytes_read;
  int i;
  int data_length;

  Printf("Starting Question 6 API test.\n");

  // Determine the string length manually since strlen not available
  for(data_length = 0; write_data[data_length] != 0; data_length++) {};

  data_length++;    // Null terminator

  // 1. Open a file for writing ('w' mode creates or overwrites it)
  fd = file_open("test_api.txt", "w");
  if (fd == FILE_FAIL) {
    Printf("Test failed: could not open file for writing.\n");
    return;
  }

  // 2. Write bytes to it
  bytes_written = file_write(fd, write_data, data_length);
  if (bytes_written != data_length) {
    Printf("Test failed: file_write wrtoe %d bytes, expected %d.\n", bytes_written, data_length);
    file_close(fd);
    return;
  }
  Printf("Successfully wrote %d bytes to file.\n", bytes_written);

  // 3. Close the file
  if (file_close(fd) == FILE_FAIL) {
    Printf("Test failed: could not close file after writing\n");
    return;
  }
  Printf("Successfully closed file\n");

  // 4. Reopen the file for reading
  fd = file_open("test_api.txt","r");
  if (fd == FILE_FAIL) {
    Printf("Test failed: could not reopen file for reading.\n");
    return;
  }
  Printf("Successfully reopened 'test_api.txt' for reading (fd: %d).\n", fd);

  // Clear the read buffer
  bzero(read_data, 100);

  // 5. Read the same bytes back
  bytes_read = file_read(fd, read_data, data_length);
  if (bytes_read != data_length) {
    Printf("Test failed: file_read %d bytes, expected %d\n", bytes_read, data_length);
    return;
  }

  // Verify the data
  for(i = 0; i < data_length; i++) {
    if (read_data[i] != write_data[i]) {
      Printf("Test failed, data mismatch at byte %d.  Expected '%c', got '%c'\n", i, write_data[i], read_data[i]);
      file_close(fd);
      return;
    }
  }

  Printf("Successfully read and verified %d bytes from file: '%s'\n", bytes_read, read_data);

  file_close(fd);

  Printf("All file API tests passed!\n");
}
