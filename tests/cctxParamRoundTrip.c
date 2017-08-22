/**
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

/*
  This program takes a file in input,
  performs a zstd round-trip test (compression - decompress)
  compares the result with original
  and generates a crash (double free) on corruption detection.
*/

/*===========================================
*   Dependencies
*==========================================*/
#include <stddef.h>     /* size_t */
#include <stdlib.h>     /* malloc, free, exit */
#include <stdio.h>      /* fprintf */
#include <sys/types.h>  /* stat */
#include <sys/stat.h>   /* stat */
#include "xxhash.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

/*===========================================
*   Macros
*==========================================*/
#define MIN(a,b)  ( (a) < (b) ? (a) : (b) )

static void crash(int errorCode){
    /* abort if AFL/libfuzzer, exit otherwise */
    #ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION /* could also use __AFL_COMPILER */
        abort();
    #else
        exit(errorCode);
    #endif
}

#define CHECK_Z(f) {                            \
    size_t const err = f;                       \
    if (ZSTD_isError(err)) {                    \
        fprintf(stderr,                         \
                "Error=> %s: %s",               \
                #f, ZSTD_getErrorName(err));    \
        crash(1);                                \
}   }

/** roundTripTest() :
*   Compresses `srcBuff` into `compressedBuff`,
*   then decompresses `compressedBuff` into `resultBuff`.
*
*   Parameters are currently set manually.
*
*   @return : result of decompression, which should be == `srcSize`
*          or an error code if either compression or decompression fails.
*   Note : `compressedBuffCapacity` should be `>= ZSTD_compressBound(srcSize)`
*          for compression to be guaranteed to work */
static size_t roundTripTest(void* resultBuff, size_t resultBuffCapacity,
                            void* compressedBuff, size_t compressedBuffCapacity,
                      const void* srcBuff, size_t srcBuffSize)
{
    ZSTD_CCtx* const cctx = ZSTD_createCCtx();
    ZSTD_CCtx_params* const cctxParams = ZSTD_createCCtxParams();
    ZSTD_inBuffer inBuffer = { srcBuff, srcBuffSize, 0 };
    ZSTD_outBuffer outBuffer = {compressedBuff, compressedBuffCapacity, 0 };

    CHECK_Z( ZSTD_CCtxParam_setParameter(cctxParams, ZSTD_p_compressionLevel, 1) );
    CHECK_Z (ZSTD_CCtxParam_setParameter(cctxParams, ZSTD_p_nbThreads, 3) );
    CHECK_Z (ZSTD_CCtxParam_setParameter(cctxParams, ZSTD_p_compressionStrategy, ZSTD_lazy) );
    CHECK_Z( ZSTD_CCtx_applyCCtxParams(cctx, cctxParams) );

    CHECK_Z (ZSTD_compress_generic(cctx, &outBuffer, &inBuffer, ZSTD_e_end) );

    ZSTD_freeCCtxParams(cctxParams);
    ZSTD_freeCCtx(cctx);

    return ZSTD_decompress(resultBuff, resultBuffCapacity, compressedBuff, outBuffer.pos);
}


static size_t checkBuffers(const void* buff1, const void* buff2, size_t buffSize)
{
    const char* ip1 = (const char*)buff1;
    const char* ip2 = (const char*)buff2;
    size_t pos;

    for (pos=0; pos<buffSize; pos++)
        if (ip1[pos]!=ip2[pos])
            break;

    return pos;
}

static void roundTripCheck(const void* srcBuff, size_t srcBuffSize)
{
    size_t const cBuffSize = ZSTD_compressBound(srcBuffSize);
    void* cBuff = malloc(cBuffSize);
    void* rBuff = malloc(cBuffSize);

    if (!cBuff || !rBuff) {
        fprintf(stderr, "not enough memory ! \n");
        exit (1);
    }

    {   size_t const result = roundTripTest(rBuff, cBuffSize, cBuff, cBuffSize, srcBuff, srcBuffSize);
        if (ZSTD_isError(result)) {
            fprintf(stderr, "roundTripTest error : %s \n", ZSTD_getErrorName(result));
            crash(1);
        }
        if (result != srcBuffSize) {
            fprintf(stderr, "Incorrect regenerated size : %u != %u\n", (unsigned)result, (unsigned)srcBuffSize);
            crash(1);
        }
        if (checkBuffers(srcBuff, rBuff, srcBuffSize) != srcBuffSize) {
            fprintf(stderr, "Silent decoding corruption !!!");
            crash(1);
        }
    }

    free(cBuff);
    free(rBuff);
}


static size_t getFileSize(const char* infilename)
{
    int r;
#if defined(_MSC_VER)
    struct _stat64 statbuf;
    r = _stat64(infilename, &statbuf);
    if (r || !(statbuf.st_mode & S_IFREG)) return 0;   /* No good... */
#else
    struct stat statbuf;
    r = stat(infilename, &statbuf);
    if (r || !S_ISREG(statbuf.st_mode)) return 0;   /* No good... */
#endif
    return (size_t)statbuf.st_size;
}


static int isDirectory(const char* infilename)
{
    int r;
#if defined(_MSC_VER)
    struct _stat64 statbuf;
    r = _stat64(infilename, &statbuf);
    if (!r && (statbuf.st_mode & _S_IFDIR)) return 1;
#else
    struct stat statbuf;
    r = stat(infilename, &statbuf);
    if (!r && S_ISDIR(statbuf.st_mode)) return 1;
#endif
    return 0;
}


/** loadFile() :
*   requirement : `buffer` size >= `fileSize` */
static void loadFile(void* buffer, const char* fileName, size_t fileSize)
{
    FILE* const f = fopen(fileName, "rb");
    if (isDirectory(fileName)) {
        fprintf(stderr, "Ignoring %s directory \n", fileName);
        exit(2);
    }
    if (f==NULL) {
        fprintf(stderr, "Impossible to open %s \n", fileName);
        exit(3);
    }
    {   size_t const readSize = fread(buffer, 1, fileSize, f);
        if (readSize != fileSize) {
            fprintf(stderr, "Error reading %s \n", fileName);
            exit(5);
    }   }
    fclose(f);
}


static void fileCheck(const char* fileName)
{
    size_t const fileSize = getFileSize(fileName);
    void* buffer = malloc(fileSize);
    if (!buffer) {
        fprintf(stderr, "not enough memory \n");
        exit(4);
    }
    loadFile(buffer, fileName, fileSize);
    roundTripCheck(buffer, fileSize);
    free (buffer);
}

int main(int argCount, const char** argv) {
    if (argCount < 2) {
        fprintf(stderr, "Error : no argument : need input file \n");
        exit(9);
    }
    fileCheck(argv[1]);
    fprintf(stderr, "no pb detected\n");
    return 0;
}
