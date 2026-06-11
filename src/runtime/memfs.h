/*
 * In-memory filesystem for Chrysopolis.
 */
#ifndef CHRYSOPOLIS_MEMFS_H
#define CHRYSOPOLIS_MEMFS_H

#include <sys/stat.h>
#include <sys/types.h>

/* Initialize the in-memory filesystem */
void memfs_init(void);

/* Open an in-memory file (returns fd or negative errno) */
int memfs_open(const char *path, int flags);

/* Stat an in-memory file (returns 0 on success, negative errno on error) */
int memfs_stat(const char *path, struct stat *st);

/* Lookup file metadata (returns NULL if not found) */
const void *memfs_lookup(const char *path);

#endif /* CHRYSOPOLIS_MEMFS_H */
