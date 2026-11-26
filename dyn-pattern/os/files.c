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
    bzero(open_files[i].filename, FILE_MAX_FILENAME_LENGTH);
    open_files[i].inode_handle = 0;
    open_files[i].eof = 0;
    bzero(open_files[i].mode, 4);    
    open_files[i].current_position = 0;
    open_files[i].pid = -1;
  }
}

// Validates file descriptor
static int FileDescriptorValidForCurrentProcess(int handle) {
  if (handle < 0 || handle >= FILE_MAX_OPEN_FILES) {
    return 0;
  }
  if (open_files[handle].inuse == 0) {
    return 0;
  }
  if (open_files[handle].pid != (int)GetCurrentPid()) {
    return 0;
  }
  return 1;
}

// FileOpen opens a file by name.  Mode can be 'r', 'w'  or 'a'.  Returns a file descriptor on 
// success, or FILE_FAIL on failure
int FileOpen(char* filename, char* mode) {
  int i;
  int fd = FILE_FAIL;
  int inode_handle;
  int is_r, is_w, is_a;

  if (filename == NULL || mode == NULL) {
    return FILE_FAIL;
  }

  is_r = (dstrncmp(mode, "r", 2) == 0);
  is_w = (dstrncmp(mode, "w", 2) == 0);
  is_a = (dstrncmp(mode, "a", 2) == 0);

  if (!is_r  && !is_w && !is_a) {
    return FILE_FAIL;
  }

  LockHandleAcquire(files_lock);

  // 1. Check if the file already exists in DFS
  inode_handle = DfsInodeFilenameExists(filename);

  // 2. Ensure this inode is not already open in another file descriptor
  if (inode_handle != DFS_FAIL) {
    for(i = 0; i < FILE_MAX_OPEN_FILES; i++) {
      if (open_files[i].inuse == 1 && open_files[i].inode_handle == inode_handle) {
        LockHandleRelease(files_lock);
        return FILE_FAIL;
      }
    }
  }

  // 3. Find an available file descriptor slot
  for (i = 0; i < FILE_MAX_OPEN_FILES; i++) {
    if (open_files[i].inuse == 0)     {
      fd = i;
      break;
    }
  }

  if (fd == FILE_FAIL) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  // 4. Handle mode specific inode lifecycle
  if (is_r) {
    if (inode_handle == DFS_FAIL) {
      LockHandleRelease(files_lock);
      return FILE_FAIL;
    }
  } else if (is_w) {
    // If file already exists in "w" mode, delete the inode first then reopen
    if (inode_handle != DFS_FAIL) {
      if (DfsInodeDelete(inode_handle) == DFS_FAIL) {
        LockHandleRelease(files_lock);
        return FILE_FAIL;
      }
    }
    inode_handle = DfsInodeOpen(filename);
    if (inode_handle == DFS_FAIL) {
      LockHandleRelease(files_lock);
      return FILE_FAIL;
    }
  }
  else if (is_a) {
    // In "a" mode, create the file if it does not exist
    if (inode_handle == DFS_FAIL) {
      inode_handle = DfsInodeOpen(filename);
      if (inode_handle == DFS_FAIL) {
        LockHandleRelease(files_lock);
        return FILE_FAIL;
      }
    }
  }

  // 5. Populate the file descriptor entry
  open_files[fd].inuse = 1;
  for (i = 0; i < FILE_MAX_FILENAME_LENGTH; i++) {
    open_files[fd].filename[i] = filename[i];
    if (filename[i] == '\0') {
      break;
    }
  }

  open_files[fd].filename[FILE_MAX_FILENAME_LENGTH - 1] = '\0';

  open_files[fd].inode_handle = inode_handle;
  open_files[fd].eof = 0;
  open_files[fd].mode[0] = mode[0];
  open_files[fd].mode[1] = '\0';
  open_files[fd].pid = (int)GetCurrentPid();

  if (is_a) {
    open_files[fd].current_position = DfsInodeFilesize(inode_handle);
    if (open_files[fd].current_position == DFS_FAIL) {
      open_files[fd].inuse = 0;
      LockHandleRelease(files_lock);
      return FILE_FAIL;
    }
  }
  else {
    open_files[fd].current_position = 0;
  }

  LockHandleRelease(files_lock);
  return fd;
}

// FileClose marks the file descriptor as available for reuse.
int FileClose(int handle) {

  LockHandleAcquire(files_lock);
  if (!FileDescriptorValidForCurrentProcess(handle)) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  open_files[handle].inuse = 0;
  open_files[handle].eof = 0;
  open_files[handle].current_position = 0;
  open_files[handle].pid = -1;

  LockHandleRelease(files_lock);
  return FILE_SUCCESS;
}

// FileRead reads from the file descriptor's current position.
// Automatically advances the position pointer.
int FileRead(int handle, void* mem, int num_bytes) {
  int bytes_read;
  int filesize;
  int inode_handle;
  int start_pos;

  if (mem == NULL || num_bytes < 0 || num_bytes > FILE_MAX_READWRITE_BYTES) {
    return FILE_FAIL;
  }

  if (!FileDescriptorValidForCurrentProcess(handle)) {
    return FILE_FAIL;
  }

  LockHandleAcquire(files_lock);

  if (!FileDescriptorValidForCurrentProcess(handle)) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  if (open_files[handle].eof == 1) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  inode_handle = open_files[handle].inode_handle;
  start_pos = open_files[handle].current_position;

  bytes_read = DfsInodeReadBytes(inode_handle, mem, start_pos, num_bytes);
  if (bytes_read == DFS_FAIL) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  open_files[handle].current_position += bytes_read;

  filesize = DfsInodeFilesize(open_files[handle].inode_handle);
  if (filesize != DFS_FAIL && open_files[handle].current_position >= filesize) {
    open_files[handle].eof = 1;
  }

  LockHandleRelease(files_lock);
  return bytes_read;
}

// FileWrite writes up to num_bytes (max 4096) to handle.
// Fails if file was opened in read-only ("r") mode.
int FileWrite(int handle, void* mem, int num_bytes) {
  int bytes_written;
  int inode_handle;
  int start_pos;

  if (mem == NULL || num_bytes < 0 || num_bytes > FILE_MAX_READWRITE_BYTES) {
    return FILE_FAIL;
  }

  LockHandleAcquire(files_lock);
  if (!FileDescriptorValidForCurrentProcess(handle)) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  if (open_files[handle].mode[0] == 'r') {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  inode_handle = open_files[handle].inode_handle;
  start_pos = open_files[handle].current_position;

  bytes_written = DfsInodeWriteBytes(inode_handle, mem, start_pos, num_bytes);

  if (bytes_written == DFS_FAIL) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  open_files[handle].current_position += bytes_written;

  LockHandleRelease(files_lock);
  return bytes_written;
}

// FileSeek adjusts current_position and clears the EOF flag
int FileSeek(int handle, int num_bytes, int from_where) {
  int filesize;
  int new_position;

  LockHandleAcquire(files_lock);

  if (!FileDescriptorValidForCurrentProcess(handle)) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  filesize = DfsInodeFilesize(open_files[handle].inode_handle);
  if (filesize == DFS_FAIL) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  if (from_where == FILE_SEEK_SET) {
    new_position = num_bytes;
  }
  else if (from_where == FILE_SEEK_CUR) {
    new_position = open_files[handle].current_position + num_bytes;
  }
  else if (from_where == FILE_SEEK_END) {
    new_position = filesize + num_bytes;
  }
  else {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  if (new_position < 0) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }
  open_files[handle].current_position = new_position;
  
  open_files[handle].eof = 0;

    LockHandleRelease(files_lock);
  return FILE_SUCCESS;
}

// FileDelete removes a file from the file system.
int FileDelete(char* filename) {
  int inode_handle;
  int i;

  if (filename == NULL) {
    return FILE_FAIL;
  }

  LockHandleAcquire(files_lock);

  inode_handle = DfsInodeFilenameExists(filename);
  if (inode_handle == DFS_FAIL) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  // Prevent deleting a file that is currently open
  for (i = 0; i < FILE_MAX_OPEN_FILES; i++) {
    if (open_files[i].inuse == 1 && open_files[i].inode_handle == inode_handle) {
      LockHandleRelease(files_lock);
      return FILE_FAIL;
    }
  }

  if (DfsInodeDelete(inode_handle) == DFS_FAIL) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;    
  }

  LockHandleRelease(files_lock);
  return FILE_SUCCESS;
}

// FileRename renames oldname to newname; fails if newname exists
int FileRename(char* oldname, char* newname) {
  int i, j;
  int inode_handle;

  if (oldname == NULL || newname == NULL) {
    return FILE_FAIL;
  }

  LockHandleAcquire(files_lock);

  if (DfsInodeRename(oldname, newname) == DFS_FAIL) {
    LockHandleRelease(files_lock);
    return FILE_FAIL;
  }

  // Keep descriptor filename n sync if the file is currently open
  inode_handle = DfsInodeFilenameExists(newname);
  if (inode_handle != DFS_FAIL) {
    for (i = 0; i < FILE_MAX_OPEN_FILES; i++) {
      if (open_files[i].inuse == 1 && open_files[i].inode_handle == inode_handle) {
        for (j = 0; j < FILE_MAX_FILENAME_LENGTH; j++) {
          open_files[i].filename[j] = newname[j];
          if (newname[j] == '\0') {
            break;
          }
        }
        open_files[i].filename[FILE_MAX_FILENAME_LENGTH - 1] = '\0';
      }
    }
  }
  LockHandleRelease(files_lock);
  return FILE_SUCCESS;
}