#include <sys/zfs_context.h>
#include <sys/zfs_chunkdedup.h>
static pair_t *find(pair_t *p,const char *n) {for(;p;p=p->next)if(!strcmp(p->name,n))return p;return NULL;}
static void pairs_free(pair_t *p){while(p){pair_t *q=p->next;free(p->name);free(p->value);free(p);p=q;}}
static int put(pair_t **head,const char*n,size_t c,const void*v){pair_t*p=find(*head,n);if(!p){p=calloc(1,sizeof(*p));p->name=strdup(n);p->next=*head;*head=p;}free(p->value);p->count=c;p->value=malloc(c*8);memcpy(p->value,v,c*8);return 0;}
static uint64_t count_override_object, count_override_value;
int zap_count(objset_t*o,uint64_t id,uint64_t*n){if(id==count_override_object){*n=count_override_value;return 0;}*n=0;for(pair_t*p=o->objects[id].pairs;p;p=p->next)(*n)++;return 0;}
int zap_contains(objset_t*o,uint64_t id,const char*n){return find(o->objects[id].pairs,n)?0:ENOENT;}
int zap_lookup(objset_t*o,uint64_t id,const char*n,uint64_t sz,uint64_t c,void*v){assert(sz==8);pair_t*p=find(o->objects[id].pairs,n);if(!p)return ENOENT;if(p->count!=c)return EOVERFLOW;memcpy(v,p->value,c*8);return 0;}
int zap_length(objset_t*o,uint64_t id,const char*n,uint64_t*sz,uint64_t*c){pair_t*p=find(o->objects[id].pairs,n);if(!p)return ENOENT;*sz=8;*c=p->count;return 0;}
int zap_add(objset_t*o,uint64_t id,const char*n,uint64_t sz,uint64_t c,const void*v,dmu_tx_t*t){(void)t;assert(sz==8);if(find(o->objects[id].pairs,n))return EEXIST;return put(&o->objects[id].pairs,n,c,v);}
int zap_update(objset_t*o,uint64_t id,const char*n,uint64_t sz,uint64_t c,const void*v,dmu_tx_t*t){(void)t;assert(sz==8);return put(&o->objects[id].pairs,n,c,v);}
int zap_remove(objset_t*o,uint64_t id,const char*n,dmu_tx_t*t){(void)t;pair_t**p=&o->objects[id].pairs;while(*p){if(!strcmp((*p)->name,n)){pair_t*q=*p;*p=q->next;q->next=NULL;pairs_free(q);return 0;}p=&(*p)->next;}return ENOENT;}
uint64_t zap_create(objset_t*o,int a,int b,int c,dmu_tx_t*t){(void)a;(void)b;(void)c;(void)t;uint64_t id=o->next++;o->objects[id].type=1;return id;}
dmu_tx_t *dmu_tx_create(objset_t*o){dmu_tx_t*t=calloc(1,sizeof(*t));t->os=o;return t;}
void dmu_tx_hold_sa(dmu_tx_t*t,void*h,int g){(void)t;(void)h;(void)g;}
int sa_update(void*h,int a,void*p,size_t n,dmu_tx_t*t){(void)h;(void)a;(void)p;(void)n;(void)t;return 0;}
void dmu_tx_hold_zap(dmu_tx_t*t,uint64_t o,int a,const char*n){(void)t;(void)o;(void)a;(void)n;}
void dmu_tx_hold_write(dmu_tx_t*t,uint64_t o,uint64_t x,int n){(void)t;(void)o;(void)x;(void)n;}
void dmu_tx_hold_free(dmu_tx_t*t,uint64_t o,uint64_t x,uint64_t n){(void)t;(void)o;(void)x;(void)n;}
void dmu_tx_mark_netfree(dmu_tx_t*t){(void)t;}
int dmu_tx_assign(dmu_tx_t*t,int f){(void)f;if(t->os->fail_assign){t->os->fail_assign=0;return ENOSPC;}return 0;}
void dmu_tx_abort(dmu_tx_t*t){free(t);}
void dmu_tx_commit(dmu_tx_t*t){free(t);}
int dmu_offset_next(objset_t*o,uint64_t id,boolean_t hole,uint64_t*off){object_t*p=&o->objects[id];uint64_t original=*off;for(uint64_t i=original/p->block;i<p->blockcount;i++)if((p->allocated[i]==0)==hole){*off=MAX(original,i*p->block);return 0;}if(hole){*off=p->blockcount*p->block;return 0;}return ESRCH;}
int dmu_read_l0_bps(objset_t*o,uint64_t id,uint64_t off,uint64_t len,blkptr_t*bp,size_t*n){(void)len;assert(*n>=1);object_t*p=&o->objects[id];size_t i=off/p->block;bp->hole=i>=p->blockcount||!p->allocated[i];*n=1;return 0;}
int dmu_read(objset_t*o,uint64_t id,uint64_t x,uint64_t n,void*b,int f){(void)f;if(!o->objects[id].type)return EIO;object_t*p=&o->objects[id];memset(b,0,n);if(x<p->size)memcpy(b,p->data+x,MIN(n,p->size-x));return 0;}
void dmu_write(objset_t*o,uint64_t id,uint64_t x,uint64_t n,const void*b,dmu_tx_t*t,int f){(void)t;(void)f;object_t*p=&o->objects[id];size_t count=(x+n+p->block-1)/p->block;if(count>p->blockcount){p->allocated=realloc(p->allocated,count);memset(p->allocated+p->blockcount,0,count-p->blockcount);p->blockcount=count;}for(size_t i=x/p->block;i<count;i++)p->allocated[i]=1;if(x+n>p->size){p->data=realloc(p->data,x+n);memset(p->data+p->size,0,x+n-p->size);p->size=x+n;}memcpy(p->data+x,b,n);}
static uint64_t mock_free_length;
int dmu_free_range(objset_t*o,uint64_t id,uint64_t x,uint64_t n,dmu_tx_t*t){(void)t;mock_free_length=n;object_t*p=&o->objects[id];for(size_t i=x/p->block;i<p->blockcount && (i+1)*p->block<=x+n;i++)if(i*p->block>=x)p->allocated[i]=0;if(x<p->size)memset(p->data+x,0,MIN(n,p->size-x));return 0;}
int dmu_object_free(objset_t*o,uint64_t id,dmu_tx_t*t){(void)t;object_t*p=&o->objects[id];if(!p->type)return ENOENT;free(p->data);free(p->allocated);pairs_free(p->pairs);memset(p,0,sizeof(*p));return 0;}
uint64_t dmu_object_alloc(objset_t*o,int a,int b,int c,int d,dmu_tx_t*t){(void)c;(void)d;(void)t;uint64_t id=o->next++;o->objects[id].type=a;o->objects[id].block=b;return id;}
zap_attribute_t *zap_attribute_alloc(void){return calloc(1,sizeof(zap_attribute_t));}
void zap_attribute_free(zap_attribute_t*p){free(p);}
void zap_cursor_init(zap_cursor_t*c,objset_t*o,uint64_t id){c->os=o;c->obj=id;c->entry=o->objects[id].pairs;}
int zap_cursor_retrieve(zap_cursor_t*c,zap_attribute_t*a){if(!c->entry)return ENOENT;a->za_integer_length=8;a->za_num_integers=c->entry->count;strcpy(a->za_name,c->entry->name);return 0;}
void zap_cursor_advance(zap_cursor_t*c){c->entry=c->entry->next;}
void zap_cursor_fini(zap_cursor_t*c){(void)c;}
int nvlist_alloc(nvlist_t**n,int a,int b){(void)a;(void)b;*n=calloc(1,sizeof(**n));return 0;}
void nvlist_free(nvlist_t*n){pairs_free(n->pairs);free(n);}
int nvlist_lookup_uint64(nvlist_t*n,const char*k,uint64_t*v){pair_t*p=find(n->pairs,k);if(!p)return ENOENT;*v=p->value[0];return 0;}
int nvlist_add_uint64(nvlist_t*n,const char*k,uint64_t v){return put(&n->pairs,k,1,&v);}

int nvlist_lookup_uint64_array(nvlist_t*n,const char*k,uint64_t**v,uint_t*c){pair_t*p=find(n->pairs,k);if(!p)return ENOENT;*v=p->value;*c=(uint_t)p->count;return 0;}
void fnvlist_add_uint64(nvlist_t*n,const char*k,uint64_t v){assert(!nvlist_add_uint64(n,k,v));}
void fnvlist_add_uint64_array(nvlist_t*n,const char*k,const uint64_t*v,uint_t c){assert(!put(&n->pairs,k,c,v));}
void fnvlist_add_boolean_value(nvlist_t*n,const char*k,boolean_t v){assert(!nvlist_add_uint64(n,k,v));}

#include "zfs_chunkdedup.c"

static objset_t *new_os(void){objset_t*o=calloc(1,sizeof(*o));o->next=2;o->objects[1].type=1;return o;}
static znode_t make_file(zfsvfs_t*v,const uint8_t*b,size_t n){dmu_tx_t*t=dmu_tx_create(v->z_os);uint64_t id=dmu_object_alloc(v->z_os,2,131072,0,0,t);dmu_write(v->z_os,id,0,n,b,t,0);dmu_tx_commit(t);znode_t z={.z_zfsvfs=v,.z_id=id,.z_size=n,.z_blksz=131072};return z;}
static void plan(zfs_chunkdedup_chunk_t*c,const uint8_t*b,const size_t*l,size_t n){size_t p=0;for(size_t i=0;i<n;i++){c[i].zc_offset=p;c[i].zc_length=l[i];cd_hash(b+p,l[i],c[i].zc_hash);p+=l[i];}}
static void expect_data(znode_t*z,const uint8_t*b){uint8_t*q=malloc(z->z_size);assert(zfs_chunkdedup_read_buf(z,0,z->z_size,q)==0);assert(!memcmp(q,b,z->z_size));free(q);}
static objset_t *clone_os(objset_t*o){objset_t*c=new_os();c->next=o->next;c->ds.ds_feature_activation[0]=o->ds.ds_feature_activation[0];for(uint64_t i=1;i<o->next;i++){c->objects[i].type=o->objects[i].type;c->objects[i].size=o->objects[i].size;c->objects[i].block=o->objects[i].block;c->objects[i].blockcount=o->objects[i].blockcount;if(o->objects[i].blockcount){c->objects[i].allocated=malloc(o->objects[i].blockcount);memcpy(c->objects[i].allocated,o->objects[i].allocated,o->objects[i].blockcount);}if(o->objects[i].size){c->objects[i].data=malloc(o->objects[i].size);memcpy(c->objects[i].data,o->objects[i].data,o->objects[i].size);}for(pair_t*p=o->objects[i].pairs;p;p=p->next)put(&c->objects[i].pairs,p->name,p->count,p->value);}return c;}
int main(void){
 const size_t lengths[]={49152,32768,65536,98304};const size_t n=245760;uint8_t*b=malloc(n);uint32_t r=7;for(size_t i=0;i<n;i++){r=r*1664525+1013904223;b[i]=r>>24;}
 objset_t*o=new_os();zfsvfs_t v={.z_os=o};znode_t a=make_file(&v,b,n),d=make_file(&v,b,n);zfs_chunkdedup_chunk_t c[4];plan(c,b,lengths,4);
 assert(zfs_chunkdedup_optimize(&a,0,n,c,4)==0);assert(mock_free_length==262144);expect_data(&a,b);assert(zfs_chunkdedup_verify(&a)==0);
 assert(zfs_chunkdedup_optimize(&a,0,n,c,4)==EALREADY);
 o->fail_assign=1;assert(zfs_chunkdedup_optimize(&d,0,n,c,4)==ENOSPC);assert(!zfs_chunkdedup_active(&d));expect_data(&d,b);
 assert(zfs_chunkdedup_optimize(&d,0,n,c,4)==0);zfs_chunkdedup_stats_t s;assert(!zfs_chunkdedup_status(&v,&s));assert(s.zcs_files==2&&s.zcs_chunks==4&&s.zcs_references==10&&s.zcs_records==4);
 objset_t*snap=clone_os(o);zfsvfs_t vs={.z_os=snap};znode_t as=a;as.z_zfsvfs=&vs;
 zfs_chunkdedup_lock(&v);dmu_tx_t*t=dmu_tx_create(o);assert(!zfs_chunkdedup_hold_hydrate(&a,100000,1,t));assert(!dmu_tx_assign(t,0));assert(!zfs_chunkdedup_hydrate(&a,100000,1,t));dmu_tx_commit(t);zfs_chunkdedup_unlock(&v);expect_data(&a,b);expect_data(&as,b);
 assert(!zfs_chunkdedup_status(&v,&s));assert(s.zcs_records==3&&s.zcs_references==7);
 assert(!zfs_chunkdedup_optimize(&a,0,n,c,4));assert(!zfs_chunkdedup_status(&v,&s));assert(s.zcs_records==4&&s.zcs_references==10&&s.zcs_chunks==4);expect_data(&a,b);
 cd_roots_t roots;assert(!cd_roots_lookup(o,&roots));char key[64];cd_record_key(key,a.z_id,0);pair_t*p=find(o->objects[roots.maps].pairs,key);size_t words=p->count;uint64_t*saved=malloc(words*8);memcpy(saved,p->value,words*8);assert(!zap_remove(o,roots.maps,key,NULL));uint8_t*q=malloc(n);assert(zfs_chunkdedup_read_buf(&a,0,n,q)==EIO);assert(zfs_chunkdedup_verify(&a)==EIO);assert(!zap_add(o,roots.maps,key,8,words,saved,NULL));free(saved);free(q);
 /* A missing master/file marker or wrong refcount must fail closed. */
 cd_ref_t ref;pair_t*rp=o->objects[roots.refs].pairs;memcpy(&ref,rp->value,sizeof(ref));uint64_t prior=ref.count;ref.count++;assert(!zap_update(o,roots.refs,rp->name,8,CD_REF_WORDS,&ref,NULL));assert(zfs_chunkdedup_verify(&a)==EIO);zfs_chunkdedup_lock(&v);t=dmu_tx_create(o);assert(zfs_chunkdedup_hold_hydrate(&a,0,1,t)==EIO);dmu_tx_abort(t);zfs_chunkdedup_unlock(&v);ref.count=prior;assert(!zap_update(o,roots.refs,rp->name,8,CD_REF_WORDS,&ref,NULL));
 cd_file_key(key,a.z_id);p=find(o->objects[roots.maps].pairs,key);uint64_t saved_file[2];memcpy(saved_file,p->value,16);assert(!zap_remove(o,roots.maps,key,NULL));q=malloc(n);assert(zfs_chunkdedup_active(&a));assert(zfs_chunkdedup_read_buf(&a,0,n,q)==EIO);assert(zfs_chunkdedup_verify(&a)==EIO);assert(!zap_add(o,roots.maps,key,8,2,saved_file,NULL));free(q);
 p=find(o->objects[MASTER_NODE_OBJ].pairs,ZFS_CHUNKDEDUP_MASTER);uint64_t saved_roots[CD_ROOT_WORDS];memcpy(saved_roots,p->value,sizeof(saved_roots));assert(!zap_remove(o,MASTER_NODE_OBJ,ZFS_CHUNKDEDUP_MASTER,NULL));q=malloc(n);assert(zfs_chunkdedup_active(&a));assert(zfs_chunkdedup_read_buf(&a,0,n,q)==EIO);assert(zfs_chunkdedup_verify(&a)==EIO);assert(zfs_chunkdedup_unoptimize(&a)==EIO);assert(!zap_add(o,MASTER_NODE_OBJ,ZFS_CHUNKDEDUP_MASTER,8,CD_ROOT_WORDS,saved_roots,NULL));free(q);
 nvlist_t*token;assert(!nvlist_alloc(&token,1,0));assert(zfs_chunkdedup_test_corrupt(&a,"recipe",token,0)==EPERM);zfs_chunkdedup_test_mode=1;assert(!zfs_chunkdedup_test_corrupt(&a,"recipe",token,0));assert(zfs_chunkdedup_verify(&a)==EIO);assert(!zfs_chunkdedup_test_corrupt(&a,"recipe",token,1));assert(!zfs_chunkdedup_verify(&a));nvlist_free(token);assert(!nvlist_alloc(&token,1,0));assert(!zfs_chunkdedup_test_corrupt(&a,"refcount",token,0));assert(zfs_chunkdedup_verify(&a)==EIO);assert(!zfs_chunkdedup_test_corrupt(&a,"refcount",token,1));assert(!zfs_chunkdedup_verify(&a));nvlist_free(token);assert(!nvlist_alloc(&token,1,0));assert(!zfs_chunkdedup_test_collision(token));nvlist_free(token);zfs_chunkdedup_test_mode=0;
 /* Cap checks reject persisted oversized ZAPs before scans or allocation. */
 pair_t *actual_maps=o->objects[roots.maps].pairs;
 for(uint64_t i=0;i<=ZFS_CHUNKDEDUP_MAX_ENTRIES;i++){pair_t *extra=calloc(1,sizeof(*extra));extra->name=strdup("invalid-cap-fixture");extra->count=1;extra->value=calloc(1,8);extra->next=o->objects[roots.maps].pairs;o->objects[roots.maps].pairs=extra;}
 q=malloc(n);assert(zfs_chunkdedup_read_buf(&a,0,n,q)==E2BIG);assert(zfs_chunkdedup_verify(&a)==E2BIG);assert(zfs_chunkdedup_unoptimize(&a)==E2BIG);assert(zfs_chunkdedup_status(&v,&s)==E2BIG);zfs_chunkdedup_lock(&v);t=dmu_tx_create(o);assert(zfs_chunkdedup_hold_hydrate(&a,0,1,t)==E2BIG);dmu_tx_abort(t);zfs_chunkdedup_unlock(&v);free(q);
 while(o->objects[roots.maps].pairs!=actual_maps){pair_t *extra=o->objects[roots.maps].pairs;o->objects[roots.maps].pairs=extra->next;extra->next=NULL;pairs_free(extra);}assert(!zfs_chunkdedup_verify(&a));
 /* Force near-limit counts to exercise conservative install preflight. */
 znode_t capfile=make_file(&v,b,n);uint64_t before=o->next;count_override_object=roots.maps;count_override_value=ZFS_CHUNKDEDUP_MAX_ENTRIES-1;assert(zfs_chunkdedup_optimize(&capfile,0,n,c,4)==E2BIG);count_override_object=0;assert(o->next==before);assert(!zfs_chunkdedup_active(&capfile));expect_data(&capfile,b);
 uint8_t *cb=malloc(n);memcpy(cb,b,n);for(size_t i=0;i<n;i++)cb[i]^=0x5a;znode_t newcap=make_file(&v,cb,n);zfs_chunkdedup_chunk_t cc[4];plan(cc,cb,lengths,4);before=o->next;count_override_value=ZFS_CHUNKDEDUP_MAX_ENTRIES;count_override_object=roots.index;assert(zfs_chunkdedup_optimize(&newcap,0,n,cc,4)==E2BIG);count_override_object=roots.refs;assert(zfs_chunkdedup_optimize(&newcap,0,n,cc,4)==E2BIG);count_override_object=0;assert(o->next==before);assert(!zfs_chunkdedup_active(&newcap));expect_data(&newcap,cb);free(cb);
 assert(!zfs_chunkdedup_unoptimize(&a));assert(!zfs_chunkdedup_active(&a));expect_data(&a,b);assert(!zfs_chunkdedup_unoptimize(&d));assert(!zfs_chunkdedup_status(&v,&s));assert(s.zcs_chunks==0&&s.zcs_records==0&&s.zcs_files==0);expect_data(&as,b);
 /* Native short files commonly have a non-power-of-two, 512-aligned block. */
 znode_t small1=make_file(&v,b,10001),small2=make_file(&v,b,10001);small1.z_blksz=small2.z_blksz=10240;o->objects[small1.z_id].block=o->objects[small2.z_id].block=10240;zfs_chunkdedup_chunk_t sc;const size_t sl[]={10001};plan(&sc,b,sl,1);assert(!zfs_chunkdedup_optimize(&small1,0,10001,&sc,1));assert(mock_free_length==10240);assert(!zfs_chunkdedup_optimize(&small2,0,10001,&sc,1));expect_data(&small1,b);assert(!zfs_chunkdedup_verify(&small1));assert(!zfs_chunkdedup_status(&v,&s));assert(s.zcs_records==2&&s.zcs_chunks==1&&s.zcs_references==2);
 zfs_chunkdedup_lock(&v);t=dmu_tx_create(o);assert(!zfs_chunkdedup_hold_hydrate(&small1,9900,7,t));assert(!dmu_tx_assign(t,0));assert(!zfs_chunkdedup_hydrate(&small1,9900,7,t));dmu_tx_commit(t);zfs_chunkdedup_unlock(&v);assert(!zfs_chunkdedup_active(&small1));expect_data(&small1,b);assert(!zfs_chunkdedup_unoptimize(&small2));expect_data(&small2,b);assert(!zfs_chunkdedup_status(&v,&s));assert(s.zcs_chunks==0);
 /* Genuine native holes stay holes; mapped records remain SEEK_DATA. */
 const size_t bn=131072,sn=bn*4;uint8_t*sb=calloc(1,sn);memcpy(sb,b,bn);memcpy(sb+bn*3,b,bn);znode_t sparse=make_file(&v,sb,sn);t=dmu_tx_create(o);assert(!dmu_free_range(o,sparse.z_id,bn,bn*2,t));dmu_tx_commit(t);zfs_chunkdedup_chunk_t sparseplan[4];const size_t spl[]={bn,bn,bn,bn};plan(sparseplan,sb,spl,4);assert(!zfs_chunkdedup_optimize(&sparse,0,sn,sparseplan,4));expect_data(&sparse,sb);assert(!zfs_chunkdedup_status(&v,&s));assert(s.zcs_records==2&&s.zcs_chunks==1&&s.zcs_references==2);uint64_t off=0;assert(!zfs_chunkdedup_seek(&sparse,&off,0)&&off==0);off=0;assert(!zfs_chunkdedup_seek(&sparse,&off,1)&&off==bn);off=bn;assert(!zfs_chunkdedup_seek(&sparse,&off,0)&&off==bn*3);off=bn*3+5;assert(!zfs_chunkdedup_seek(&sparse,&off,1)&&off==sn);assert(!zfs_chunkdedup_unoptimize(&sparse));off=0;assert(!zfs_chunkdedup_seek(&sparse,&off,1)&&off==bn);off=bn;assert(!zfs_chunkdedup_seek(&sparse,&off,0)&&off==bn*3);expect_data(&sparse,sb);
 /* Huge logical sparse gaps cannot turn verify/restore into size-based loops. */
 sparse.z_size=UINT64_C(1)<<40;assert(!zfs_chunkdedup_optimize(&sparse,0,sn,sparseplan,4));assert(!zfs_chunkdedup_verify(&sparse));assert(!zfs_chunkdedup_unoptimize(&sparse));sparse.z_size=sn;expect_data(&sparse,sb);free(sb);
 puts("PASS: shares/slices, repeated optimize, ENOSPC no mutation, partial hydration, mixed reoptimize, snapshot model, missing recipe/file metadata EIO, refcount corruption rejected before GC, non-power-of-two smallfile sharing/hydration, debug gating/inject/restore, real sharing comparator collision fixture, sparse extent allocation/seek preservation, metadata caps/preflight rejection without mutation, huge sparse gap bounded restoration, reclamation");
 return 0;
}
