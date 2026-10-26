// Linux build: Darwin's clonefile(2) as a recursive copy (reflinked per file where the file system can, FICLONE on btrfs / XFS), keeping modes and extended attributes
// as clonefile does (hle_fs.c keeps guest names in an xattr). Unlike APFS's clone, the copy takes time proportional to the save's size; the transactional save
// semantics (copy, then an atomic swap) are unchanged
#pragma once
#include <darwin_compat.h>
#define CLONE_NOFOLLOW 1
#define CLONE_NOOWNERCOPY 2
int clonefile(const char *src, const char *dst, uint32_t flags);
