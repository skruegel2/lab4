#ifndef __DFS_H__
#define __DFS_H__

#include "dfs_shared.h"

#define DFS_CACHE_SLOTS 32

// Non- node prototypes
void DfsModuleInit();
void DfsInvalidate();
int DfsOpenFileSystem();
int DfsCloseFileSystem();
int DfsAllocateBlock();
int DfsFreeBlock(uint32 blocknum);
int DfsReadBlock(uint32 blocknum, dfs_block *b);
int DfsWriteBlock(uint32 blocknum, dfs_block * b);
int DfsReadBlockUncached(uint32 blocknum, dfs_block *b);
int DfsWriteBlockUncached(uint32 blocknum, dfs_block *b);
int DfsCacheHit(int blocknum);
int DfsCacheAllocateSlot(int blocknum);
int DfsCacheFlush();

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
