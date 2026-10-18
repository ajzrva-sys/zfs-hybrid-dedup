#ifndef MOCK_ZFS_CONTEXT_H
#define MOCK_ZFS_CONTEXT_H
#define _KERNEL 1
#define ZFS_DEBUG 1
#define ZFS_MODULE_PARAM(a,b,c,d,e,f)
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
typedef unsigned int uint_t;
typedef unsigned long long u_longlong_t;
typedef int boolean_t;
typedef int kmutex_t;
#define B_TRUE 1
#define B_FALSE 0
#define KM_SLEEP 0
#define SET_ERROR(e) (e)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define ISP2(v) (((v) & ((v)-1))==0)
#define P2ALIGN(v,a) ((v)&~((a)-1))
#define ASSERT(v) assert(v)
#define VERIFY(v) assert(v)
#define VERIFY0(v) assert((v)==0)
#define VERIFY3U(a,op,b) assert((a) op (b))
#define MUTEX_HELD(m) (*(m)==1)
static inline void mutex_enter(kmutex_t *m) { assert(*m==0); *m=1; }
static inline void mutex_exit(kmutex_t *m) { assert(*m==1); *m=0; }
static inline void *kmem_alloc(size_t n,int f) {(void)f;void *p=malloc(n); assert(p);return p;}
static inline void kmem_free(void *p,size_t n) {(void)n;free(p);}
#define MASTER_NODE_OBJ 1
#define SPA_MINBLOCKSIZE 512
#define ZFS_CHUNKDEDUP (UINT64_C(1)<<48)
#define SA_ZPL_FLAGS(z) 1
#define DMU_NEW_OBJECT UINT64_MAX
#define DMU_OBJECT_END UINT64_MAX
#define DMU_OT_NONE 0
#define DMU_OT_ZAP_OTHER 1
#define DMU_OT_PLAIN_FILE_CONTENTS 2
#define DMU_READ_PREFETCH 0
#define DMU_TX_WAIT 0
#define DMU_MAX_ACCESS (64U<<20)
#define UIO_READ 0
#define SPA_FEATURE_CHUNKDEDUP 0
#define SHA512 1

typedef struct pair {char *name;uint64_t *value;uint64_t count;struct pair *next;} pair_t;
typedef struct object {int type;size_t size,block,blockcount;uint8_t *data,*allocated;pair_t *pairs;} object_t;
typedef struct blkptr {int hole;} blkptr_t;
#define BP_IS_HOLE(p) ((p)->hole)
typedef struct dsl_dataset {kmutex_t ds_lock;void *ds_feature_activation[1];} dsl_dataset_t;
typedef struct objset {object_t objects[4096];uint64_t next;dsl_dataset_t ds;int fail_assign;} objset_t;
typedef struct dmu_tx {objset_t *os;} dmu_tx_t;
typedef struct zfsvfs {objset_t *z_os;kmutex_t z_chunkdedup_lock;} zfsvfs_t;
typedef struct znode {zfsvfs_t *z_zfsvfs;uint64_t z_id,z_size,z_pflags;void *z_sa_hdl;uint_t z_blksz;} znode_t;
#define ZTOZSB(zp) ((zp)->z_zfsvfs)
typedef struct zfs_uio {uint64_t offset;char *buffer;size_t length;} zfs_uio_t;
#define zfs_uio_offset(u) ((u)->offset)
static inline int zfs_uiomove(void *p,size_t n,int f,zfs_uio_t *u) {(void)f;memcpy(u->buffer,p,n);u->buffer+=n;u->offset+=n;u->length-=n;return 0;}
typedef struct zap_cursor {objset_t *os;uint64_t obj;pair_t *entry;} zap_cursor_t;
typedef struct zap_attribute {int za_integer_length;uint64_t za_num_integers;char za_name[256];} zap_attribute_t;
typedef struct nvlist {pair_t *pairs;} nvlist_t;
#define NV_UNIQUE_NAME 1
#ifdef __APPLE__
typedef CC_SHA512_CTX SHA2_CTX;
static inline void SHA2Init(int t,SHA2_CTX *c) {(void)t;CC_SHA512_Init(c);}
static inline void SHA2Update(SHA2_CTX *c,const void *p,size_t n) {CC_SHA512_Update(c,p,(CC_LONG)n);}
static inline void SHA2Final(void *p,SHA2_CTX *c) {CC_SHA512_Final(p,c);}
#else
typedef SHA512_CTX SHA2_CTX;
static inline void SHA2Init(int t,SHA2_CTX *c) {(void)t;SHA512_Init(c);}
static inline void SHA2Update(SHA2_CTX *c,const void *p,size_t n) {SHA512_Update(c,p,n);}
static inline void SHA2Final(void *p,SHA2_CTX *c) {SHA512_Final(p,c);}
#endif

int zap_count(objset_t*,uint64_t,uint64_t*);
int zap_contains(objset_t*,uint64_t,const char*);
int zap_lookup(objset_t*,uint64_t,const char*,uint64_t,uint64_t,void*);
int zap_length(objset_t*,uint64_t,const char*,uint64_t*,uint64_t*);
int zap_add(objset_t*,uint64_t,const char*,uint64_t,uint64_t,const void*,dmu_tx_t*);
int zap_update(objset_t*,uint64_t,const char*,uint64_t,uint64_t,const void*,dmu_tx_t*);
int zap_remove(objset_t*,uint64_t,const char*,dmu_tx_t*);
uint64_t zap_create(objset_t*,int,int,int,dmu_tx_t*);
dmu_tx_t *dmu_tx_create(objset_t*);
void dmu_tx_hold_sa(dmu_tx_t*,void*,int);
int sa_update(void*,int,void*,size_t,dmu_tx_t*);
void dmu_tx_hold_zap(dmu_tx_t*,uint64_t,int,const char*);
void dmu_tx_hold_write(dmu_tx_t*,uint64_t,uint64_t,int);
void dmu_tx_hold_free(dmu_tx_t*,uint64_t,uint64_t,uint64_t);
void dmu_tx_mark_netfree(dmu_tx_t*);
int dmu_tx_assign(dmu_tx_t*,int);
void dmu_tx_abort(dmu_tx_t*);
void dmu_tx_commit(dmu_tx_t*);
int dmu_offset_next(objset_t*,uint64_t,boolean_t,uint64_t*);
int dmu_read_l0_bps(objset_t*,uint64_t,uint64_t,uint64_t,blkptr_t*,size_t*);
int dmu_read(objset_t*,uint64_t,uint64_t,uint64_t,void*,int);
void dmu_write(objset_t*,uint64_t,uint64_t,uint64_t,const void*,dmu_tx_t*,int);
int dmu_free_range(objset_t*,uint64_t,uint64_t,uint64_t,dmu_tx_t*);
int dmu_object_free(objset_t*,uint64_t,dmu_tx_t*);
uint64_t dmu_object_alloc(objset_t*,int,int,int,int,dmu_tx_t*);
static inline objset_t *dmu_objset_spa(objset_t *os) {return os;}
static inline dsl_dataset_t *dmu_objset_ds(objset_t *os) {return &os->ds;}
static inline int spa_feature_is_enabled(objset_t *os,int f) {(void)os;(void)f;return 1;}
zap_attribute_t *zap_attribute_alloc(void);
void zap_attribute_free(zap_attribute_t*);
void zap_cursor_init(zap_cursor_t*,objset_t*,uint64_t);
int zap_cursor_retrieve(zap_cursor_t*,zap_attribute_t*);
void zap_cursor_advance(zap_cursor_t*);
void zap_cursor_fini(zap_cursor_t*);
int nvlist_alloc(nvlist_t**,int,int);
void nvlist_free(nvlist_t*);
int nvlist_lookup_uint64(nvlist_t*,const char*,uint64_t*);
int nvlist_add_uint64(nvlist_t*,const char*,uint64_t);
int nvlist_lookup_uint64_array(nvlist_t*,const char*,uint64_t**,uint_t*);
void fnvlist_add_uint64(nvlist_t*,const char*,uint64_t);
void fnvlist_add_uint64_array(nvlist_t*,const char*,const uint64_t*,uint_t);
void fnvlist_add_boolean_value(nvlist_t*,const char*,boolean_t);
#endif
