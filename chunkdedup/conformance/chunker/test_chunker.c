/* MIT license. Boundary/callback tests of the public portable C API. */
#include "chunkdedup_chunker.h"
#include "rabin64_table.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct capture { struct chunkdedup_chunk chunks[128]; size_t count; int fail; };
static int record(void *argument,const struct chunkdedup_chunk *chunk,const uint8_t *bytes) {
    struct capture *capture=argument;
    assert(bytes!=NULL);
    if(capture->fail)return ECANCELED;
    assert(capture->count<128);
    capture->chunks[capture->count++]=*chunk;
    return 0;
}
static uint64_t append(uint64_t hash,uint8_t b) {
 return (hash<<8)^b^g_arrPolynomialsTD[hash>>56];
}
int main(int argc,char **argv) {
    struct chunkdedup_chunker *session;
    struct capture capture={0};
    assert(chunkdedup_chunker_create(NULL,record,&capture)==EINVAL);
    assert(chunkdedup_chunker_create(&session,NULL,&capture)==EINVAL);
    assert(chunkdedup_chunker_create(&session,record,&capture)==0);
    assert(chunkdedup_chunker_push(session,NULL,1)==EINVAL);
    assert(chunkdedup_chunker_push(session,NULL,0)==0);
    assert(chunkdedup_chunker_finish(session)==0);
    assert(chunkdedup_chunker_finish(session)==0);
    assert(chunkdedup_chunker_push(session,"x",1)==EINVAL);
    assert(capture.count==0);
    chunkdedup_chunker_destroy(session);
    chunkdedup_chunker_destroy(NULL);
    uint8_t *bytes=malloc(CHUNKDEDUP_PUSH_SIZE*2+17);
    assert(bytes);
    memset(bytes,0xff,CHUNKDEDUP_PUSH_SIZE*2+17);
    capture.fail=1;
    assert(chunkdedup_chunker_create(&session,record,&capture)==0);
    assert(chunkdedup_chunker_push(session,bytes,CHUNKDEDUP_PUSH_SIZE)==ECANCELED);
    assert(chunkdedup_chunker_push(session,bytes,1)==ECANCELED);
    assert(chunkdedup_chunker_finish(session)==ECANCELED);
    chunkdedup_chunker_destroy(session);
    struct chunkdedup_chunk chunks[34];size_t count;
    assert(chunkdedup_regression_chunks(bytes,CHUNKDEDUP_PUSH_SIZE+1,chunks,34,&count)==EINVAL);
    assert(chunkdedup_regression_chunks(bytes,CHUNKDEDUP_PUSH_SIZE,chunks,0,&count)==EOVERFLOW);
    /* Produce isolated partial Rabin matches at minimum: each window's last
     * two bytes can select all low16 hash bits. Repeated FF afterward avoids
     * introducing another candidate. Verify all four regression strengths. */
    const uint16_t targets[4]={0xd555,0x9555,0x3555,0x0555};
    for(size_t i=0;i<4;i++) {
        memset(bytes,0xff,CHUNKDEDUP_CHUNK_MAX);
        uint64_t hash=0;
        for(size_t j=CHUNKDEDUP_CHUNK_MIN-16;j<CHUNKDEDUP_CHUNK_MIN-2;j++)hash=append(hash,bytes[j]);
        hash=append(append(hash,0),0);
        uint16_t ending=(uint16_t)(hash^targets[i]);
        bytes[CHUNKDEDUP_CHUNK_MIN-2]=(uint8_t)(ending>>8);
        bytes[CHUNKDEDUP_CHUNK_MIN-1]=(uint8_t)ending;
        assert(chunkdedup_regression_chunks(bytes,CHUNKDEDUP_CHUNK_MAX,chunks,34,&count)==0);
        assert(count==2);
        assert(chunks[0].length==CHUNKDEDUP_CHUNK_MIN);
        assert(chunks[0].cut_type==7+i);
        printf("regression_%zu: offset0 length%u type%u\n",i+1,chunks[0].length,chunks[0].cut_type);
        if(argc>1) {
            char path[4096];
            assert(snprintf(path,sizeof path,"%s/regression-%zu.bin",argv[1],i+1)<(int)sizeof path);
            FILE *file=fopen(path,"wb");assert(file);
            assert(fwrite(bytes,1,CHUNKDEDUP_CHUNK_MAX,file)==CHUNKDEDUP_CHUNK_MAX);
            assert(fclose(file)==0);
        }
    }
    free(bytes);
    puts("API lifecycle, cancellation, bounds and four regression types PASS");
    return 0;
}
