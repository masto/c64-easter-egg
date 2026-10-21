/*
 * gpusearch: fastsearch on the GPU (Metal).  Same arguments and output.
 *
 * usage: gpusearch [--assign] [--scale T] [--offset S] WORD [FIRST [LAST]]
 *
 * The kernel (search.metal) is embedded as source and compiled at startup.
 * The seed range is split into batches, all encoded into one command buffer.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rndfast.h"
#include "search_metal.h"   /* generated: search_metal[], search_metal_len */

typedef struct {
    uint32_t first, count, per_thread, b_lo, b_span, scale, assign, capacity;
} Params;

static uint32_t GRID = 1u << 22;    /* threads per dispatch; $GPU_GRID_LOG2 overrides */
#define BATCH       (1u << 30)      /* seeds per dispatch */
#define CAPACITY    (1u << 22)      /* results overall */

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--assign] [--scale T] [--offset S] WORD [FIRST [LAST]]\n", prog);
    exit(2);
}

static uint64_t parse_number(const char *s, uint64_t max, const char *prog)
{
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v > max)
        usage(prog);
    return v;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static id<MTLComputePipelineState> pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *name)
{
    NSError *err = nil;
    id<MTLComputePipelineState> pso =
        [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&err];
    if (!pso) {
        fprintf(stderr, "pipeline %s failed: %s\n", name.UTF8String, err.localizedDescription.UTF8String);
        exit(2);
    }
    return pso;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char *prog = argv[0];
        uint64_t scale = 22, offset = 64;
        uint32_t assign = 0;

        int i = 1;
        for (; i < argc && argv[i][0] == '-'; i++) {
            if (strcmp(argv[i], "--assign") == 0)
                assign = 1;
            else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
                scale = parse_number(argv[++i], 255, prog);
            else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc)
                offset = parse_number(argv[++i], 255, prog);
            else
                usage(prog);
        }
        if (i >= argc || argc - i > 3 || scale == 0)
            usage(prog);

        const char *word = argv[i];
        size_t len = strlen(word);
        if (len == 0 || len >= 255)
            usage(prog);
        char upper[256];
        uint32_t target[256];
        for (size_t k = 0; k <= len; k++) {
            char ch = word[k];
            upper[k] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
            if (k < len) {
                int v = (unsigned char)upper[k] - (int)offset;
                if (v <= 0 || v > (int)scale) {   /* v == scale needs RND = 1.0, which happens */
                    fprintf(stderr, "%s: '%c' can never be produced\n", prog, ch);
                    return 1;
                }
                target[k] = (uint32_t)v;
            }
        }
        target[len] = 0;
        uint64_t first = argc - i > 1 ? parse_number(argv[i + 1], 0xFFFFFFFFu, prog) : 1;
        uint64_t last  = argc - i > 2 ? parse_number(argv[i + 2], 0xFFFFFFFFu, prog) : 99999999;
        if (first == 0)
            first = 1;
        ffilter filt = fast_filter_init((uint32_t)scale, target[0]);

        double t_setup = now();
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 2; }
        NSError *err = nil;
        NSString *src = [[NSString alloc] initWithBytes:search_metal length:search_metal_len
                                               encoding:NSUTF8StringEncoding];
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:[MTLCompileOptions new] error:&err];
        if (!lib) { fprintf(stderr, "shader compile failed: %s\n", err.localizedDescription.UTF8String); return 2; }
        id<MTLComputePipelineState> pso = pipeline(dev, lib, @"search");
        id<MTLCommandQueue> queue = [dev newCommandQueue];

        MTLResourceOptions shared = MTLResourceStorageModeShared;
        id<MTLBuffer> targetBuf = [dev newBufferWithBytes:target length:sizeof target options:shared];
        id<MTLBuffer> foundBuf  = [dev newBufferWithLength:4 options:shared];
        id<MTLBuffer> resultBuf = [dev newBufferWithLength:(size_t)CAPACITY * 4 options:shared];
        memset(foundBuf.contents, 0, 4);
        if (getenv("GPU_GRID_LOG2"))
            GRID = 1u << atoi(getenv("GPU_GRID_LOG2"));

        double t_run = now();
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        for (uint64_t lo = first; lo <= last; lo += BATCH) {
            uint64_t n = last - lo + 1 < BATCH ? last - lo + 1 : BATCH;
            uint32_t grid = n < GRID ? (uint32_t)n : GRID;
            Params p = { (uint32_t)lo, (uint32_t)n, (uint32_t)((n + grid - 1) / grid),
                         filt.b_lo, filt.b_span, (uint32_t)scale, assign, CAPACITY };
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBytes:&p length:sizeof p atIndex:0];
            [enc setBuffer:targetBuf offset:0 atIndex:1];
            [enc setBuffer:foundBuf offset:0 atIndex:2];
            [enc setBuffer:resultBuf offset:0 atIndex:3];
            [enc dispatchThreads:MTLSizeMake(grid, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(pso.maxTotalThreadsPerThreadgroup, 1, 1)];
            [enc endEncoding];
        }
        [cmd commit];
        [cmd waitUntilCompleted];
        if (cmd.error) { fprintf(stderr, "GPU error: %s\n", cmd.error.localizedDescription.UTF8String); return 2; }
        double t_end = now();

        uint32_t nfound = *(uint32_t *)foundBuf.contents;
        if (nfound > CAPACITY) { fprintf(stderr, "too many results\n"); return 2; }
        uint32_t *found = resultBuf.contents;
        qsort(found, nfound, sizeof *found, cmp_u32);
        for (uint32_t k = 0; k < nfound; k++)
            printf("%u %s\n", found[k], upper);

        double seeds = (double)(last - first + 1);
        fprintf(stderr, "%.0f seeds in %.3f s: %.2f Gseeds/s on %s (setup %.3f s)\n",
                seeds, t_end - t_run, seeds / (t_end - t_run) / 1e9, dev.name.UTF8String, t_run - t_setup);
    }
    return 0;
}
