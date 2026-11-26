#ifndef __FILES_SHARED__
#define __FILES_SHARED__

#define FILE_SEEK_SET 1
#define FILE_SEEK_END 2
#define FILE_SEEK_CUR 3

#ifndef FILE_MAX_FILENAME_LENGTH
#define FILE_MAX_FILENAME_LENGTH 72
#endif

#ifndef FILE_MAX_READWRITE_BYTES
#define FILE_MAX_READWRITE_BYTES 4096
#endif

#ifndef FILE_MAX_OPEN_FILES
#define FILE_MAX_OPEN_FILES 15
#endif


typedef struct file_descriptor {
  int inuse;
  char filename[FILE_MAX_FILENAME_LENGTH];
  int inode_handle;
  int eof;
  char mode [4];
  int current_position;
  int pid;
} file_descriptor;

#define FILE_FAIL -1
#define FILE_EOF -1
#define FILE_SUCCESS 1

#endif
