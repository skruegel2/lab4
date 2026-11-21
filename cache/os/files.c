#include "ostraps.h"
#include "dlxos.h"
#include "process.h"
#include "dfs.h"
#include "files.h"
#include "synch.h"

// You have already been told about the most likely places where you should use locks. You may use 
// additional locks if it is really necessary.
static file_descriptor open_files[FILE_MAX_OPEN_FILES];
static lock_t files_lock;

// FileModuleInit initializes the file descriptor table and the lock
void FileModuleInit() {
  int i;
  files_lock = LockCreate();

  for(i = 0; i < FILE_MAX_OPEN_FILES; i++) {
    open_files[i].inuse = 0;
    open_files[i].inode_handle = 0;
    open_files[i].current_position = 0;
  }
}

// FileOpen opens a file by name.  Mode can be 'r', 'w'  or 'a'.  Returns a file descriptor on 
// success, or FILE_FAIL on failure
int FileOpen(char* filename, char* mode) {
  int i;
  int fd = FILE_FAIL;
  int inode_handle;

  LockHandleAcquire(files_lock);

  // 1. Find a free file descriptor from the table
  for(i = 0; i < FILE_MAX_OPEN_FILES; i++) {
    if (open_files[i].inuse == 0) {
      fd = i;
      break;
    }
  }

  if (fd == FILE_FAIL) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;   // Too many open files
  }

  // 2. Locate the inode for the file
  inode_handle = DfsInodeFilenameExists(filename);

  if (inode_handle == DFS_FAIL) {
    // If file doesn't exist, 'r' mode fails. 'w' and 'a' create it.
    if (mode[0] == 'r') {
      LockHandleRelease(files_lock);
      return FILE_FAIL;
    }
    inode_handle = DfsInodeOpen(filename);
    if (inode_handle == DFS_FAIL) {
      LockHandleRelease(files_lock);
      return FILE_FAIL;   // Could not allocate a new inode
    }
  }

  // 3. Initialize the file descriptor
  open_files[fd].inuse = 1;
  open_files[fd].inode_handle = inode_handle;

  // Set the correct starting position based on the mode
  if (mode[0] == 'a') {
    open_files[fd].current_position = DfsInodeFilesize(inode_handle);
  }
  else {
    // 'r' and 'w' start at the beginning of the file
    open_files[fd].current_position = 0;
  }

  LockHandleRelease(files_lock);

  return fd;
}

// FileClose marks the file descriptor as available for reuse.
int FileClose(int handle) {
  if (handle >= FILE_MAX_OPEN_FILES || open_files[handle].inuse == 0) {
    return FILE_FAIL;
  }

  LockHandleAcquire(files_lock);
  open_files[handle].inuse = 0;
  LockHandleRelease(files_lock);

  return FILE_SUCCESS;
}

// FileRead reads from the file descriptor's current position.
// Automatically advances the position pointer.
int FileRead(int handle, void* mem, int num_bytes) {
  int bytes_read;

  if (handle >= FILE_MAX_OPEN_FILES || open_files[handle].inuse == 0) {
    return FILE_FAIL;
  }

  bytes_read = DfsInodeReadBytes(
    open_files[handle].inode_handle,
    mem,
    open_files[handle].current_position,
    num_bytes
  );

  if (bytes_read != DFS_FAIL) {
    open_files[handle].current_position += bytes_read;
  }

  return bytes_read;
}

// FileWrite writes to the file descriptors current position
// Automatically advances to the position pointer
int FileWrite(int handle, void* mem, int num_bytes) {
  int bytes_written;

  if (handle >= FILE_MAX_OPEN_FILES || open_files[handle].inuse == 0) {
    return FILE_FAIL;
  }

  bytes_written = DfsInodeWriteBytes(
    open_files[handle].inode_handle,
    mem,
    open_files[handle].current_position,
    num_bytes
  );

  if (bytes_written != DFS_FAIL) {
    open_files[handle].current_position += bytes_written;
  }

  return bytes_written;
}

// FileSeek adjusts the file descriptor's current read/write position.
int FileSeek(int handle, int num_bytes, int from_where) {
  int filesize;

  if (handle >= FILE_MAX_OPEN_FILES || open_files[handle].inuse == 0) {
    return FILE_FAIL;
  }

  filesize = DfsInodeFilesize(open_files[handle].inode_handle);
  if (filesize == DFS_FAIL) {
    return FILE_FAIL;
  }

  if (from_where == FILE_SEEK_SET) {
    open_files[handle].current_position = num_bytes;
  }
  else if (from_where == FILE_SEEK_CUR) {
    open_files[handle].current_position += num_bytes;
  }
  else if (from_where == FILE_SEEK_END) {
    open_files[handle].current_position = filesize + num_bytes;
  }
  else {
    return FILE_FAIL;
  }

  // Prevent seeking before the start of the file
  if (open_files[handle].current_position < 0) {
    open_files[handle].current_position = 0;
  }

  return FILE_SUCCESS;
}

// FileDelete removes a file from the file system.
int FileDelete(char* filename) {
  int inode_handle;

  inode_handle = DfsInodeFilenameExists(filename);
  if (inode_handle == DFS_FAIL) {
    return FILE_FAIL;
  }

  return DfsInodeDelete(inode_handle);
}

int FileRename(char* oldname, char* newname) {
  return DfsInodeRename(oldname, newname);
}