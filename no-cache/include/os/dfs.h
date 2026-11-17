#ifndef __DFS_H__
#define __DFS_H__

#include "dfs_shared.h"

// Non- node prototypes
void DfsModuleInit();
void DfsInvalidate();
int DfsOpenFileSystems();
int DfsCloseFileSystem();
int DfsAllocateBlock();
int DfsFreeBlock(uint32 blocknum);
int DfsReadBlock(uint32 blocknum, dfs_block *b);
int DfsWriteBlock(uint32 blocknum, dfs_block * b);

// Inode prototypes
int DfsInodeFilenameExists(char* filename);
int DfsInodeOpen(char* filename);
int DfsInodeDelete(uint32 handle);
int DfsInodeReadBytes(uint32 handle, void *mem, int start_byte, int num_bytes);
int DfsInodeWriteBytes(uint32 handle, void *mem, int start_byte, int num_bytes);
int DfsInodeFilesize(uint32 handle);
int DfsInodeAllocateVirtualBlock(uint32 handle, uint32 virtual);
int DfsInodeTranslateVirtualToFilesys(uint32 handle, uint32 virtual_blocknum);

int DfsInodeRename(char* oldname, char* newname);
#endif
