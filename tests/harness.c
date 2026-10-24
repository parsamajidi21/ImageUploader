/*
 * tests/harness.c
 *
 * Drives the image reader / backup writer in src/stream.c without a user interface.
 *
 *   harness dec <image> <out>          decode an image (any supported format) to a raw file
 *   harness enc <raw> <backup> <comp>  create a backup like the Read button does (comp 1 = zstd)
 *
 * Exit status is 0 on success, 1 when the stream code reports an error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "lang.h"
#include "stream.h"

#ifdef WINVER
wchar_t **lang = NULL;
#else
char **lang = NULL;
extern char *dict[NUMLANGS][NUMTEXTS + 1];
#endif
char *main_errorMessage = NULL;

void main_addToCombobox(char *option) { (void)option; }
void main_getErrorMessage(void) { }
void main_onProgress(void *data) { (void)data; }

int main(int argc, char **argv)
{
    static stream_t ctx;
    static char buf[1 << 20];
    FILE *f;
    int n, r;

#ifndef WINVER
    lang = &dict[0][1];
#endif
    verbose = getenv("VERBOSE") ? 2 : 0;
    if(argc < 4) {
        fprintf(stderr, "usage: harness dec <image> <out> | enc <raw> <backup> <comp>\n");
        return 2;
    }
    if(!strcmp(argv[1], "dec")) {
        if((r = stream_open(&ctx, argv[2], 0))) { printf("stream_open=%d\n", r); return 1; }
        if(!(f = fopen(argv[3], "wb"))) { stream_close(&ctx); return 2; }
        while((n = stream_read(&ctx)) > 0)
            fwrite(ctx.buffer, 1, n, f);
        fclose(f);
        stream_close(&ctx);
        if(n < 0) { printf("stream_read=%d\n", n); return 1; }
        return 0;
    }
    if(!strcmp(argv[1], "enc") && argc > 4) {
        if(!(f = fopen(argv[2], "rb"))) return 2;
        fseek(f, 0, SEEK_END);
        if(stream_create(&ctx, argv[3], atoi(argv[4]), (uint64_t)ftell(f))) { printf("stream_create failed\n"); return 1; }
        fseek(f, 0, SEEK_SET);
        /* the Read button hands over buffer_size chunks */
        while((n = (int)fread(buf, 1, buffer_size < (int)sizeof(buf) ? buffer_size : (int)sizeof(buf), f)) > 0)
            if(!stream_write(&ctx, buf, n)) { printf("stream_write failed\n"); fclose(f); return 1; }
        fclose(f);
        stream_close(&ctx);
        return 0;
    }
    return 2;
}
