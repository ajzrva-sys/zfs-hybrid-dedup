/* SPDX-License-Identifier: MIT
 * Minimal native-free API adapter. It exercises the production C scanner,
 * never substitutes for kernel/VFS proof, and never opens a real ZFS pool.
 */
#ifndef BLOCKDEDUP_ADAPTER_H
#define BLOCKDEDUP_ADAPTER_H
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define ZFS_MAX_DATASET_NAME_LEN 256
#define ZFS_TYPE_FILESYSTEM 1
#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef int boolean_t;
typedef unsigned int uint_t;
#define B_TRUE 1
#define B_FALSE 0
typedef struct {
    char key[40];
    uint64_t number;
    char string[32];
    uint8_t *bytes;
    uint_t count;
} mock_pair;
typedef struct { mock_pair pairs[24]; size_t used; } nvlist_t;
static mock_pair *mock_lookup(nvlist_t *n, const char *key)
{
    if (n)
        for (size_t i = 0; i < n->used; i++)
            if (!strcmp(n->pairs[i].key, key)) return &n->pairs[i];
    return NULL;
}
static mock_pair *mock_add(nvlist_t *n, const char *key)
{
    mock_pair *p = mock_lookup(n, key);
    if (p) return p;
    assert(n->used < 24 && strlen(key) < sizeof (n->pairs[0].key));
    p = &n->pairs[n->used++]; strcpy(p->key, key); return p;
}
static nvlist_t *fnvlist_alloc(void) { nvlist_t *n = calloc(1, sizeof (*n)); assert(n); return n; }
static void nvlist_free(nvlist_t *n)
{
    if (n) { for (size_t i = 0; i < n->used; i++) free(n->pairs[i].bytes); free(n); }
}
#define fnvlist_free nvlist_free
static void fnvlist_add_uint64(nvlist_t *n, const char *k, uint64_t v) { mock_add(n,k)->number=v; }
static void fnvlist_add_string(nvlist_t *n, const char *k, const char *v)
{ mock_pair *p=mock_add(n,k); assert(strlen(v)<sizeof (p->string)); strcpy(p->string,v); }
static int nvlist_lookup_uint64(nvlist_t *n,const char *k,uint64_t *v)
{ mock_pair *p=mock_lookup(n,k); if (!p) return ENOENT; *v=p->number; return 0; }
static int nvlist_lookup_boolean_value(nvlist_t *n,const char *k,boolean_t *v)
{ uint64_t x; int e=nvlist_lookup_uint64(n,k,&x); if (!e) *v=(boolean_t)x; return e; }
static int nvlist_lookup_uint8_array(nvlist_t *n,const char *k,uint8_t **v,uint_t *c)
{ mock_pair *p=mock_lookup(n,k); if (!p) return ENOENT; *v=p->bytes; *c=p->count; return 0; }
typedef struct { char mount[PATH_MAX]; } zfs_handle_t;
static int g_zfs;
static zfs_handle_t mock_handle;
static zfs_handle_t *zfs_open(int z, const char *name, int type)
{ (void)z; (void)type; return !strcmp(name,"cdwf-local/test") ? &mock_handle : NULL; }
static zfs_handle_t *zfs_path_to_zhandle(int z,const char *path,int type)
{ (void)z; (void)path; (void)type; return &mock_handle; }
static int zfs_get_type(zfs_handle_t *z) { (void)z; return ZFS_TYPE_FILESYSTEM; }
static const char *zfs_get_name(zfs_handle_t *z) { (void)z; return "cdwf-local/test"; }
static boolean_t zfs_is_mounted(zfs_handle_t *z,char **p) { *p=strdup(z->mount); assert(*p); return B_TRUE; }
static void zfs_close(zfs_handle_t *z) { (void)z; }
static uid_t mock_geteuid(void) { return 0; }
#define geteuid mock_geteuid

typedef struct {
    char path[PATH_MAX];
    ino_t inode;
    uint64_t block, size, physical[40];
    int sparse, eligible;
} mock_file;
static mock_file mock_files[40];
static size_t mock_count;
static unsigned mock_reads, mock_shares;
static int mock_share_error;
static mock_file *mock_find(uint64_t object)
{
    for (size_t i=0;i<mock_count;i++) if (mock_files[i].inode==object) return &mock_files[i];
    return NULL;
}
static uint64_t mock_number(nvlist_t *n,const char *k)
{ uint64_t v; assert(!nvlist_lookup_uint64(n,k,&v)); return v; }
static int lzc_blockdedup(const char *dataset,nvlist_t *in,nvlist_t **out)
{
    assert(!strcmp(dataset,"cdwf-local/test"));
    mock_file *f=mock_find(mock_number(in,"object"));
    if (!f) return ESTALE;
    if (mock_number(in,"size")!=f->size) return ESTALE;
    const char *op=mock_lookup(in,"op")->string;
    *out=fnvlist_alloc();
    if (!strcmp(op,"inspect")) {
        fnvlist_add_uint64(*out,"block_size",f->block);
        fnvlist_add_uint64(*out,"eligible",f->eligible);
        mock_pair *off=mock_lookup(in,"offset");
        if (off) {
            assert(off->number<f->size && off->number%f->block==0);
            fnvlist_add_uint64(*out,"block_length",MIN(f->block,f->size-off->number));
            fnvlist_add_uint64(*out,"block_hole",f->sparse && off->number==f->block);
            fnvlist_add_uint64(*out,"block_embedded",0);
        }
        return 0;
    }
    uint64_t offset=mock_number(in,"offset"),length=mock_number(in,"length");
    if (!strcmp(op,"read")) {
        assert(length<=256U<<10 && offset+length<=f->size);
        mock_pair *p=mock_add(*out,"bytes");
        p->bytes=malloc(length); assert(p->bytes); p->count=(uint_t)length;
        int fd=open(f->path,O_RDONLY); assert(fd>=0);
        assert(pread(fd,p->bytes,length,offset)==(ssize_t)length); close(fd); mock_reads++;
        return 0;
    }
    assert(!strcmp(op,"share-blocks")); mock_shares++;
    if (mock_share_error) { int error=mock_share_error; mock_share_error=0; return error; }
    mock_file *source=mock_find(mock_number(in,"source_object")); assert(source);
    uint64_t from=mock_number(in,"source_offset");
    assert(source->block==f->block && from%f->block==0 && offset%f->block==0);
    assert(length==MIN(f->block,f->size-offset));
    uint8_t *a=malloc(length),*b=malloc(length); assert(a&&b);
    int x=open(source->path,O_RDONLY), y=open(f->path,O_RDONLY); assert(x>=0&&y>=0);
    assert(pread(x,a,length,from)==(ssize_t)length && pread(y,b,length,offset)==(ssize_t)length);
    close(x);close(y);int equal=!memcmp(a,b,length);free(a);free(b);
    if (!equal) return EILSEQ;
    uint64_t already=source->physical[from/f->block]==f->physical[offset/f->block] ? length : 0;
    f->physical[offset/f->block]=source->physical[from/f->block];
    fnvlist_add_uint64(*out,"shared_bytes",length-already);
    fnvlist_add_uint64(*out,"already_shared_bytes",already);
    return 0;
}
#endif
