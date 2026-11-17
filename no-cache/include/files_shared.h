#ifndef __FILES_SHARED__
#define __FILES_SHARED__

#define FILE_SEEK_SET 1
#define FILE_SEEK_END 2
#define FILE_SEEK_CUR 3


#ifndef FILE_MAX_OPEN_FILES
#define FILE_MAX_OPEN_FILES 15
#endif


typedef struct file_descriptor {
  int inuse;
  uint32 inode_handle;
  int current_position;
} file_descriptor;

#define FILE_FAIL -1
#define FILE_EOF -1
#define FILE_SUCCESS 1

#endif
