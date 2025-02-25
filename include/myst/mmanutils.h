// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#ifndef _MYST_MMANUTILS_H
#define _MYST_MMANUTILS_H

#include <myst/buf.h>
#include <myst/mman.h>
#include <myst/refstr.h>
#include <sys/types.h>

#define MYST_FDMAPPING_USED 0x1ca0597f

/*
defines a file-page to memory-page mapping
The mapping entries in the fdmappings vector are populated at mmap, and cleaned
up at munmap.
*/
typedef struct myst_fdmapping
{
    uint32_t used;           /* whether entry is used */
    int32_t fd;              /* duplicated fd */
    uint64_t offset;         /* offset of page within file */
    myst_refstr_t* pathname; /* full pathname associated with fd */
} myst_fdmapping_t;

int myst_setup_mman(void* data, size_t size);

int myst_teardown_mman(void);

long myst_mmap(
    void* addr,
    size_t length,
    int prot,
    int flags,
    int fd,
    off_t offset);

int myst_munmap(void* addr, size_t length);

long myst_syscall_brk(void* addr);

void* myst_mremap(
    void* old_address,
    size_t old_size,
    size_t new_size,
    int flags,
    void* new_address);

int myst_mprotect(const void* addr, const size_t len, const int prot);

int myst_get_total_ram(size_t* size);

int myst_get_free_ram(size_t* size);

int myst_release_process_mappings(pid_t pid);

int myst_msync(void* addr, size_t length, int flags);

typedef struct myst_mman_stats
{
    size_t brk_size;
    size_t map_size;
    size_t free_size;
    size_t used_size;
    size_t total_size;
} myst_mman_stats_t;

void myst_mman_stats(myst_mman_stats_t* buf);

int proc_pid_maps_vcallback(myst_buf_t* vbuf, const char* entrypath);

/* marks all pages in the mapping as owned by the given process */
int myst_mman_pids_set(const void* addr, size_t length, pid_t pid);

/* return the length in bytes that are owned by the given process */
ssize_t myst_mman_pids_test(const void* addr, size_t length, pid_t pid);

void myst_mman_lock(void);

void myst_mman_unlock(void);

#endif /* _MYST_MMANUTILS_H */
