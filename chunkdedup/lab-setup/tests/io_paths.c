/* SPDX-License-Identifier: MIT
 * Guest-only bounded native I/O regression helper. Never opens remote sockets.
 * All file arguments must resolve to regular files below /mnt/cdlab/.
 */
#define _WITH_GETLINE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FILE_LIMIT (64U << 20)
#define IO_BLOCK (128U << 10)
#define MUTATION_LIMIT 4096U
static const char *root = "/mnt/cdlab/";

static void die(const char *message) { perror(message); exit(1); }
static void fail(const char *message) { fprintf(stderr,"%s\n",message); exit(1); }
static int open_regular(const char *path,int flags,struct stat *st) {
    char resolved[PATH_MAX];
    if(realpath(path,resolved)==NULL)die("realpath");
    if(strncmp(resolved,root,strlen(root))!=0)fail("path must resolve below /mnt/cdlab/");
    int fd=open(resolved,flags|O_NOFOLLOW);
    if(fd<0)die("open");
    if(fstat(fd,st)!=0)die("fstat");
    if(!S_ISREG(st->st_mode)||st->st_size<0||st->st_size>FILE_LIMIT)
        fail("regular files must be no larger than 64 MiB");
    return fd;
}
static uint64_t number(const char *value) {
    char *end;errno=0;uint64_t n=strtoull(value,&end,10);
    if(errno||*end||value[0]=='-'||value[0]=='\0')fail("invalid offset");
    return n;
}
static size_t decode_hex(const char *text,uint8_t bytes[MUTATION_LIMIT]) {
    size_t n=strlen(text);
    if(n==0||n%2||n/2>MUTATION_LIMIT)fail("mutation must contain 1..4096 hex bytes");
    for(size_t i=0;i<n/2;i++) {
        char pair[3]={text[2*i],text[2*i+1],0};char *end;
        if(!((pair[0]>='0'&&pair[0]<='9')||(pair[0]>='a'&&pair[0]<='f')||(pair[0]>='A'&&pair[0]<='F'))||
           !((pair[1]>='0'&&pair[1]<='9')||(pair[1]>='a'&&pair[1]<='f')||(pair[1]>='A'&&pair[1]<='F')))
            fail("invalid hexadecimal mutation");
        unsigned long x=strtoul(pair,&end,16);if(*end)fail("invalid hexadecimal mutation");bytes[i]=(uint8_t)x;
    }
    return n/2;
}
static void pread_exact(int fd,void *buffer,size_t length,off_t offset) {
    uint8_t *bytes=buffer;
    while(length) {
        ssize_t n=pread(fd,bytes,length,offset);
        if(n<0&&errno==EINTR)continue;
        if(n<0)die("pread");if(n==0)fail("unexpected EOF");
        bytes+=n;length-=(size_t)n;offset+=n;
    }
}
static void verify(int fd,int expected,off_t size,int mapped) {
    uint8_t *actual=malloc(IO_BLOCK),*want=malloc(IO_BLOCK);if(!actual||!want)die("malloc");
    uint8_t *map=NULL;
    if(mapped&&size) {
        map=mmap(NULL,(size_t)size,PROT_READ,MAP_SHARED,fd,0);
        if(map==MAP_FAILED)die("mmap readonly");
    }
    for(off_t offset=0;offset<size;) {
        size_t take=(size_t)(size-offset)<IO_BLOCK?(size_t)(size-offset):IO_BLOCK;
        pread_exact(expected,want,take,offset);
        if(mapped)memcpy(actual,map+offset,take);else pread_exact(fd,actual,take,offset);
        if(memcmp(actual,want,take)!=0)fail("logical bytes differ from expected fixture");
        offset+=(off_t)take;
    }
    if(map&&munmap(map,(size_t)size)!=0)die("munmap readonly");
    free(actual);free(want);
}
static void mutate(int fd,off_t size,uint64_t offset,const char *hex,int mapped) {
    uint8_t bytes[MUTATION_LIMIT];size_t length=decode_hex(hex,bytes);
    if(offset>(uint64_t)size||length>(uint64_t)size-offset)fail("mutation must stay within existing file");
    if(mapped) {
        long pagesize=sysconf(_SC_PAGESIZE);if(pagesize<=0)fail("page size unavailable");
        uint64_t base=offset-(offset%(uint64_t)pagesize);
        size_t span=(size_t)(offset-base)+length;
        uint8_t *map=mmap(NULL,span,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off_t)base);
        if(map==MAP_FAILED)die("mmap writable");
        memcpy(map+offset-base,bytes,length);
        if(msync(map,span,MS_SYNC)!=0)die("msync");
        if(munmap(map,span)!=0)die("munmap writable");
    } else {
        size_t written=0;
        while(written<length) {
            ssize_t n=pwrite(fd,bytes+written,length-written,(off_t)(offset+written));
            if(n<0&&errno==EINTR)continue;
            if(n<0)die("pwrite");if(n==0)fail("pwrite made no progress");written+=(size_t)n;
        }
    }
    if(fsync(fd)!=0)die("fsync");
}
static void sendfile_verify(int fd,int expected,off_t size) {
    int sockets[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)!=0)die("socketpair");
    struct timeval timeout={5,0};
    if(setsockopt(sockets[0],SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout)!=0||
       setsockopt(sockets[1],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout)!=0)die("socket timeout");
    pid_t child=fork();if(child<0)die("fork");
    if(child==0) {
        close(sockets[1]);off_t offset=0;
        while(offset<size) {
            off_t sent=0;int error;
#ifdef __FreeBSD__
            error=sendfile(fd,sockets[0],offset,(size_t)(size-offset),NULL,&sent,0);
#elif defined(__APPLE__)
            sent=size-offset;error=sendfile(fd,sockets[0],offset,&sent,NULL,0);
#else
            (void)error;close(sockets[0]);_exit(77);
#endif
            if(sent>0)offset+=sent;
            if(error!=0&&errno==EINTR)continue;
            if(error!=0||sent==0){close(sockets[0]);_exit(1);}
        }
        close(sockets[0]);_exit(0);
    }
    close(sockets[0]);uint8_t *bytes=malloc(IO_BLOCK),*want=malloc(IO_BLOCK);
    if(!bytes||!want){kill(child,SIGKILL);die("malloc");}
    off_t offset=0;int bad=0;
    while(offset<size) {
        size_t take=(size_t)(size-offset)<IO_BLOCK?(size_t)(size-offset):IO_BLOCK;
        ssize_t n=read(sockets[1],bytes,take);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){bad=1;break;}
        pread_exact(expected,want,(size_t)n,offset);
        if(memcmp(bytes,want,(size_t)n)!=0){bad=1;break;}offset+=n;
    }
    close(sockets[1]);if(bad)kill(child,SIGKILL);
    int status;while(waitpid(child,&status,0)<0)if(errno!=EINTR)die("waitpid");
    free(bytes);free(want);
    if(bad||!WIFEXITED(status)||WEXITSTATUS(status)!=0)fail("sendfile stream failed or differs from expected fixture");
}
static int direct_verify(const char *path,int expected,off_t size) {
#ifndef O_DIRECT
    (void)path;(void)expected;(void)size;return 77;
#else
    struct stat st;int fd=open_regular(path,O_RDONLY|O_DIRECT,&st);
    if(st.st_size!=size)fail("file changed before direct read");
    void *aligned;if(posix_memalign(&aligned,4096,IO_BLOCK)!=0)fail("aligned allocation failed");
    uint8_t *want=malloc(IO_BLOCK);if(!want)die("malloc");
    for(off_t offset=0;offset<size;) {
        size_t take=(size_t)(size-offset)<IO_BLOCK?(size_t)(size-offset):IO_BLOCK;
        pread_exact(fd,aligned,take,offset);pread_exact(expected,want,take,offset);
        if(memcmp(aligned,want,take)!=0)fail("direct I/O bytes differ from expected fixture");offset+=(off_t)take;
    }
    free(aligned);free(want);if(close(fd)!=0)die("close direct");return 0;
#endif
}
static void direct_mutate(const char *path,off_t size,uint64_t offset,const char *hex) {
#ifndef O_DIRECT
    (void)path;(void)size;(void)offset;(void)hex;
    fail("direct-write requires O_DIRECT support");
#else
    uint8_t bytes[MUTATION_LIMIT];size_t length=decode_hex(hex,bytes);
    if(length!=4096||offset%4096!=0)fail("direct-write requires 4096 bytes at a 4096-aligned offset");
    if(offset>(uint64_t)size||length>(uint64_t)size-offset)fail("direct-write must stay within existing file");
    struct stat st;int fd=open_regular(path,O_WRONLY|O_DIRECT,&st);
    if(st.st_size!=size)fail("file changed before direct write");
    void *aligned;if(posix_memalign(&aligned,4096,4096)!=0)fail("aligned allocation failed");
    memcpy(aligned,bytes,length);
    ssize_t written;
    do {written=pwrite(fd,aligned,length,(off_t)offset);}while(written<0&&errno==EINTR);
    if(written<0)die("direct pwrite");
    if((size_t)written!=length)fail("direct pwrite returned a short write");
    if(fsync(fd)!=0)die("fsync direct");
    free(aligned);if(close(fd)!=0)die("close direct write");
#endif
}
int main(int argc,char **argv) {
    if(argc<3)fail("usage: io_paths verify|mmap-verify|sendfile|direct FILE EXPECTED; pwrite|mmap-write|direct-write FILE OFFSET HEX");
    int mutation=strcmp(argv[1],"pwrite")==0||strcmp(argv[1],"mmap-write")==0||strcmp(argv[1],"direct-write")==0;
    if((mutation&&argc!=5)||(!mutation&&argc!=4))fail("incorrect argument count");
    struct stat st;int fd=open_regular(argv[2],mutation?O_RDWR:O_RDONLY,&st);
    if(strcmp(argv[1],"direct-write")==0)direct_mutate(argv[2],st.st_size,number(argv[3]),argv[4]);
    else if(mutation)mutate(fd,st.st_size,number(argv[3]),argv[4],strcmp(argv[1],"mmap-write")==0);
    else {
        struct stat expected_stat;int expected=open_regular(argv[3],O_RDONLY,&expected_stat);
        if(st.st_size!=expected_stat.st_size)fail("source and expected fixture sizes differ");
        if(st.st_dev==expected_stat.st_dev&&st.st_ino==expected_stat.st_ino)fail("expected fixture must be a separate inode");
        if(strcmp(argv[1],"verify")==0)verify(fd,expected,st.st_size,0);
        else if(strcmp(argv[1],"mmap-verify")==0)verify(fd,expected,st.st_size,1);
        else if(strcmp(argv[1],"sendfile")==0)sendfile_verify(fd,expected,st.st_size);
        else if(strcmp(argv[1],"direct")==0){int result=direct_verify(argv[2],expected,st.st_size);if(result)return result;}
        else fail("unknown operation");
        if(close(expected)!=0)die("close expected");
    }
    if(close(fd)!=0)die("close source");
    printf("PASS %s %" PRId64 " bytes\n",argv[1],(int64_t)st.st_size);return 0;
}
