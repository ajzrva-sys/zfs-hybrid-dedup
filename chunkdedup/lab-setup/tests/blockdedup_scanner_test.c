/* SPDX-License-Identifier: MIT */
#include "blockdedup_adapter.h"
#ifndef BLOCKDEDUP_SOURCE
#error Supply the absolute production scanner path in BLOCKDEDUP_SOURCE
#endif
#define BLOCKDEDUP_TEST_HEADER "blockdedup_adapter.h"
#include BLOCKDEDUP_SOURCE

static void make_file(const char *name,uint64_t block,uint64_t size,unsigned seed,int sparse)
{
    assert(mock_count<40 && size/block<40);
    mock_file *f=&mock_files[mock_count];
    assert(snprintf(f->path,sizeof(f->path),"%s/%s",mock_handle.mount,name)<(int)sizeof(f->path));
    f->block=block;f->size=size;f->sparse=sparse;f->eligible=1;
    int fd=open(f->path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);
    uint8_t data[4096];
    for(uint64_t p=0;p<size;) {
        size_t n=MIN(sizeof(data),size-p);
        for(size_t i=0;i<n;i++){seed=1664525U*seed+1013904223U;data[i]=seed>>24;}
        assert(pwrite(fd,data,n,p)==(ssize_t)n);p+=n;
    }
    struct stat st;assert(!fstat(fd,&st));f->inode=st.st_ino;close(fd);
    for(size_t i=0;i<40;i++) f->physical[i]=1+mock_count*40+i;
    mock_count++;
}
static void clear_files(void)
{
    for(size_t i=0;i<mock_count;i++) assert(!unlink(mock_files[i].path));
    memset(mock_files,0,sizeof(mock_files));mock_count=mock_reads=mock_shares=0;
}
static struct bd_catalog analyze(void)
{
    struct bd_catalog c;assert(!bd_init(&c,mock_handle.mount,B_FALSE));bd_stop=0;
    for(size_t i=0;i<mock_count;i++) assert(!bd_analyze(&c,"cdwf-local/test",mock_files[i].path,0));
    return c;
}
static int cli(int argc,char **argv)
{
#ifdef __APPLE__
    optreset=1;
#endif
    return zfs_do_blockdedup(argc,argv);
}
int main(void)
{
    char temporary[]="/tmp/zfs-blockdedup-test.XXXXXX";
    char *dir=mkdtemp(temporary);assert(dir);strcpy(mock_handle.mount,dir);
    make_file("a",131072,262267,81,0);make_file("b",131072,262267,81,0);
    make_file("unique",131072,262267,82,0);
    struct bd_catalog c=analyze();assert(c.count==3 && c.block_count==9 && mock_reads==9);
    assert(!bd_apply(&c) && c.candidates==3 && c.shared==3 && mock_shares==3);
    assert(mock_files[0].physical[0]==mock_files[1].physical[0]);
    assert(mock_files[2].physical[0]!=mock_files[0].physical[0]);bd_fini(&c);
    c=analyze();assert(!bd_apply(&c)&&c.shared==0&&c.already==3);bd_fini(&c);clear_files();
    puts("PASS exact duplicates, unique blocks, short tails, already shared");

    make_file("large-record",131072,262144,83,0);make_file("small-record",16384,262144,83,0);
    make_file("sparse",131072,262144,84,1);
    c=analyze();assert(c.block_count==19);assert(!bd_apply(&c)&&c.candidates==0);bd_fini(&c);
    c=analyze();boolean_t found;struct stat st;assert(!stat(mock_files[0].path,&st));
    assert(!bd_seen(&c,&st,B_FALSE,&found)&&found);
    assert(!bd_analyze(&c,"cdwf-local/test",mock_files[0].path,0)&&c.count==3&&c.skipped==1);
    bd_fini(&c);clear_files();puts("PASS incompatible geometry, sparse skip, hardlink identity exclusion");

    make_file("a",131072,262144,85,0);make_file("b",131072,262144,85,0);
    int errors[]={ESTALE,EBUSY,EILSEQ};
    for(size_t i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        c=analyze();mock_share_error=errors[i];assert(!bd_apply(&c));
        assert((i<2&&c.deferred==1)||(i==2&&c.skipped==1));bd_fini(&c);
        for(size_t j=0;j<mock_count;j++)for(size_t k=0;k<40;k++)mock_files[j].physical[k]=1+j*40+k;
    }
    c=analyze();c.dry_run=B_TRUE;unsigned before=mock_shares;
    assert(!bd_apply(&c)&&c.candidates==2&&mock_shares==before);bd_fini(&c);
    mock_files[1].eligible=0;c=analyze();assert(c.count==1&&c.skipped==1);bd_fini(&c);
    clear_files();puts("PASS stale and busy deferral, byte mismatch rejection, dry run and eligibility");

    char *bad[]={"blockdedup","optimize","--min-age","-1",dir,NULL};assert(cli(5,bad)==2);
    char *bad_interval[]={"blockdedup","optimize","--interval","1",dir,NULL};assert(cli(5,bad_interval)==2);
    make_file("a",131072,262144,86,0);make_file("b",131072,262144,86,0);
    char *watch[]={"blockdedup","watch","--once","--min-age","0",dir,NULL};assert(cli(6,watch)==0);
    assert(mock_files[0].physical[0]==mock_files[1].physical[0]);clear_files();
    assert(!rmdir(dir));puts("PASS CLI contracts and one-pass background watcher");
    puts("PASS production C scanner adapter; kernel behavior remains a separate native check");
    return 0;
}
