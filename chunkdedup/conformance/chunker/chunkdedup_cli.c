/* Copyright (c) Microsoft Corporation. Licensed under the MIT License. */
#include "chunkdedup_chunker.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int emit(void *unused, const struct chunkdedup_chunk *chunk,
    const uint8_t *bytes) {
    (void)unused; (void)bytes;
    if (printf("%" PRIu64 "\t%" PRIu32 "\t", chunk->offset,
        chunk->length) < 0) return EIO;
    for (size_t i = 0; i < 32; i++)
        if (printf("%02x", chunk->hash[i]) < 0) return EIO;
    return putchar('\n') == EOF ? EIO : 0;
}
int main(int argc, char **argv) {
    size_t block = 262144;
    if (argc != 2 && argc != 3) {
        fprintf(stderr,"usage: %s FILE|- [read-size]\n",argv[0]); return 2;
    }
    if (argc == 3) {
        char *end;
        errno = 0;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (errno || *end || value == 0 || value > 16777216) return 2;
        block = (size_t)value;
    }
    FILE *file = strcmp(argv[1],"-") == 0 ? stdin : fopen(argv[1],"rb");
    if (!file) { perror(argv[1]); return 1; }
    uint8_t *buffer = malloc(block);
    if (!buffer) { perror("malloc"); return 1; }
    struct chunkdedup_chunker *session;
    int error = chunkdedup_chunker_create(&session,emit,NULL);
    if (error) { fprintf(stderr,"create: %s\n",strerror(error)); return 1; }
    size_t count;
    while ((count = fread(buffer,1,block,file)) > 0) {
        error = chunkdedup_chunker_push(session,buffer,count);
        if (error) break;
    }
    if (!error && ferror(file)) error = EIO;
    if (!error) error = chunkdedup_chunker_finish(session);
    if (!error && fflush(stdout) != 0) error = EIO;
    chunkdedup_chunker_destroy(session);
    free(buffer);
    if (file != stdin) fclose(file);
    if (error) fprintf(stderr,"chunking: %s\n",strerror(error));
    return error != 0;
}
