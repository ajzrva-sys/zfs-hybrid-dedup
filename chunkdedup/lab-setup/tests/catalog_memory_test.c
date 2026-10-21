/* SPDX-License-Identifier: MIT */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "chunkdedup_hybrid.h"
typedef int boolean_t;
typedef unsigned int uint_t;
#define B_FALSE 0
#define B_TRUE 1
#define ZFS_MAX_DATASET_NAME_LEN 256
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
static uint64_t group_limit;
#define CD_CATALOG_GROUP_LIMIT group_limit
static atomic_int cd_stop;
#include "types.inc"
#include "record.inc"
static uint64_t record_calls;
static int cd_catalog_record(struct cd_catalog *c,uint64_t id,struct cd_catalog_file *f,boolean_t w)
{ record_calls++; return cd_catalog_record_impl(c,id,f,w); }
/* Unchanged checked-add and sort transports; selection/root/cost/report are actual source. */
int cd_hybrid_add(uint64_t *value,uint64_t amount)
{ if(UINT64_MAX-*value<amount) return EOVERFLOW; *value+=amount; return 0; }
static int line_compare(const void *a,const void *b)
{ return strcmp(*(char *const *)a,*(char *const *)b); }
int cd_hybrid_sort(FILE *input,FILE **output,const char *directory)
{
    (void)directory;
    assert(!fflush(input)); rewind(input);
    char line[256]; size_t count=0,capacity=1024;
    char **lines=malloc(capacity*sizeof(*lines)); assert(lines);
    while(fgets(line,sizeof(line),input)) {
        if(count==capacity) { capacity*=2; lines=realloc(lines,capacity*sizeof(*lines)); assert(lines); }
        lines[count++]=strdup(line); assert(lines[count-1]);
    }
    qsort(lines,count,sizeof(*lines),line_compare);
    *output=tmpfile(); assert(*output);
    for(size_t i=0;i<count;i++) { assert(fputs(lines[i],*output)>=0); free(lines[i]); }
    free(lines); rewind(*output); return 0;
}
int cd_hybrid_sort_parallel(FILE *input,FILE **output,const char *directory,
    unsigned threads,const atomic_int *stop)
{
    assert(threads==40);
    return stop&&atomic_load(stop)?ECANCELED:cd_hybrid_sort(input,output,directory);
}
#include "core.inc"
static void cleanup(struct cd_catalog *c)
{
    assert(c->groups==NULL);
    FILE *streams[]={c->files,c->blocks,c->chunks,c->sorted_blocks,c->sorted_chunks,c->report};
    for(size_t i=0;i<sizeof(streams)/sizeof(*streams);i++) if(streams[i]) fclose(streams[i]);
    if(c->report_temporary[0]) unlink(c->report_temporary);
}
static void setup(struct cd_catalog *c,enum cd_method method)
{
    memset(c,0,sizeof(*c)); c->count=8;c->method=method;
    c->files=tmpfile();c->blocks=tmpfile();c->chunks=tmpfile();
    assert(c->files&&c->blocks&&c->chunks);
    for(uint64_t i=0;i<c->count;i++) {
        struct cd_catalog_file f={0}; f.parent=i; f.method=method;
        f.native_base=10+i;f.chunk_base=20+i;f.uncertain=(i==5); f.chunked=(i==7);
        f.identity.st_ino=100+i;f.identity.st_size=8192;f.identity.st_mtime=12345;
        snprintf(f.dataset,sizeof(f.dataset),"fixture/dataset");
        snprintf(f.path,sizeof(f.path),"file/%" PRIu64 " quote\"\\ tab\t newline\n \xce\xb1\xff",i);
        assert(fwrite(&f,sizeof(f),1,c->files)==1);
    }
    uint8_t hash[32]={0};
    for(unsigned i=0;i<4000;i++) {
        assert(!cd_catalog_append(c->blocks,0,hash,4096,0,0,100,4096));
        assert(!cd_catalog_append(c->blocks,0,hash,4096,1,0,200,4096));
    }
    hash[0]=1;
    assert(!cd_catalog_append(c->chunks,0,hash,1024,1,0,50,0));
    assert(!cd_catalog_append(c->chunks,0,hash,1024,2,0,70,0));
    hash[0]=2;
    assert(!cd_catalog_append(c->blocks,0,hash,4096,3,0,300,4096));
    assert(!cd_catalog_append(c->blocks,0,hash,4096,4,0,400,4096));
    hash[0]=3;
    assert(!cd_catalog_append(c->chunks,0,hash,1024,5,0,150,0));
    assert(!cd_catalog_append(c->chunks,0,hash,1024,6,0,175,0));
    hash[0]=4;
    assert(!cd_catalog_append(c->chunks,0,hash,1024,7,0,999,1));
}
int main(int argc,char **argv)
{
    assert(argc==2);
    struct cd_catalog_file result[2][8]; uint64_t calls[2];
    for(int method=CD_METHOD_AUTO;method<=CD_METHOD_CHUNK;method++) {
        for(unsigned mode=0;mode<2;mode++) {
            struct cd_catalog c;setup(&c,(enum cd_method)method);
            group_limit=mode?0:256U*1024*1024;
            char path[PATH_MAX];snprintf(path,sizeof(path),"%s/%s.jsonl",argv[1],mode?"disk":"memory");
            unlink(path);assert(!cd_catalog_report_init(&c,path));record_calls=0;
            assert(!cd_catalog_select(&c,40));calls[mode]=record_calls;
            assert(c.report==NULL&&c.report_temporary[0]==0);
            for(unsigned i=0;i<8;i++) assert(!cd_catalog_record(&c,i,&result[mode][i],B_FALSE));
            cleanup(&c);
        }
        assert(calls[0]<=8*5);assert(calls[1]>calls[0]*100);
        assert(!memcmp(result[0],result[1],sizeof(result[0])));
    }
    struct cd_catalog c;setup(&c,CD_METHOD_BLOCK);group_limit=256U*1024*1024;
    char path[PATH_MAX];snprintf(path,sizeof(path),"%s/atomic.jsonl",argv[1]);
    FILE *old=fopen(path,"w");assert(old);fputs("keep old report\n",old);fclose(old);
    assert(cd_catalog_report_init(&c,path)==EEXIST);
    old=fopen(path,"r");char line[64];assert(fgets(line,sizeof(line),old));assert(!strcmp(line,"keep old report\n"));fclose(old);
    unlink(path);assert(!cd_catalog_report_init(&c,path));cd_stop=1;
    assert(cd_catalog_select(&c,40)==ECANCELED);assert(c.groups==NULL);cleanup(&c);cd_stop=0;
    assert(access(path,F_OK)!=0);
    return 0;
}
