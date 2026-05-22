/* vec_smoke.c — link the PufferLib vec API directly and exercise
 * create_static_vec / static_vec_reset / cpu_vec_step on nethack.
 * Standalone repro of the segfault that pufferl.py train hits when
 * total_agents >= 2.
 *
 * Build (from repo root):
 *   . /etc/profile.d/modules.sh; module load intel-oneapi/2024.2
 *   clang -O2 -g -Wall -Wno-unused-function -std=gnu11 \
 *     -I./vendor/nle/include -I./ocean/nethack -I./src \
 *     ocean/nethack/vec_smoke.c -o vec_smoke \
 *     -ldl -lpthread -lm -fopenmp
 *
 * Usage:
 *   NETHACK_LIBPATH=./vendor/nle/src/build/libnethack.so ./vec_smoke 2
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nethack.h"
#define OBS_SIZE NETHACK_OBS_SIZE
#define NUM_ATNS 1
#define ACT_SIZES {NETHACK_NUM_ACTIONS}
#define OBS_TENSOR_T ByteTensor
#define Env Nethack
#include "vecenv.h"

/* CPU-only build: stub the CUDA externs so we can link without nvcc. */
cudaError_t cudaMalloc(void** p, size_t n)              { *p = malloc(n); return 0; }
cudaError_t cudaMemcpy(void* d, const void* s, size_t n, cudaMemcpyKind k) { (void)k; memcpy(d,s,n); return 0; }
cudaError_t cudaMemcpyAsync(void* d, const void* s, size_t n, cudaMemcpyKind k, cudaStream_t st) { (void)k;(void)st; memcpy(d,s,n); return 0; }
cudaError_t cudaMemset(void* p, int v, size_t n)        { memset(p,v,n); return 0; }
cudaError_t cudaFree(void* p)                            { free(p); return 0; }
cudaError_t cudaFreeHost(void* p)                        { free(p); return 0; }
cudaError_t cudaHostAlloc(void** p, size_t n, unsigned f){(void)f; *p = malloc(n); return 0; }
cudaError_t cudaDeviceSynchronize(void)                  { return 0; }
cudaError_t cudaStreamCreate(cudaStream_t* s)            { *s = NULL; return 0; }
cudaError_t cudaStreamSynchronize(cudaStream_t s)        { (void)s; return 0; }
cudaError_t cudaStreamDestroy(cudaStream_t s)            { (void)s; return 0; }

void my_init(Env* env, Dict* kwargs) {
    (void)kwargs;
    env->num_agents = 1;
    init(env);
}

void my_log(Log* log, Dict* out) {
    dict_set(out, "perf", log->perf);
    dict_set(out, "score", log->score);
}

int main(int argc, char** argv) {
    int N = (argc >= 2) ? atoi(argv[1]) : 2;
    Dict* vec_kw = create_dict(8);
    Dict* env_kw = create_dict(2);
    dict_set(vec_kw, "total_agents", (double)N);
    dict_set(vec_kw, "num_buffers", 1.0);
    dict_set(vec_kw, "num_threads", 0.0);
    dict_set(vec_kw, "seed", 73.0);

    fprintf(stderr, "[vec_smoke] N=%d: create_static_vec...\n", N);
    StaticVec* vec = create_static_vec(N, 1, 0, vec_kw, env_kw);
    fprintf(stderr, "[vec_smoke] vec->size=%d\n", vec->size);

    fprintf(stderr, "[vec_smoke] static_vec_reset...\n");
    static_vec_reset(vec);
    fprintf(stderr, "[vec_smoke] reset OK\n");

    /* Drive one cpu_vec_step with random actions to see if step path works. */
    Env* envs = (Env*) vec->envs;
    for (int i = 0; i < vec->size; i++) {
        envs[i].actions[0] = (float)(i % NETHACK_NUM_ACTIONS);
    }
    fprintf(stderr, "[vec_smoke] cpu_vec_step #1...\n");
    cpu_vec_step(vec);
    fprintf(stderr, "[vec_smoke] step OK\n");

    /* Loop a bunch */
    int crashes = 0;
    for (int t = 0; t < 100; t++) {
        for (int i = 0; i < vec->size; i++) {
            envs[i].actions[0] = (float)((t + i) % NETHACK_NUM_ACTIONS);
        }
        cpu_vec_step(vec);
    }
    fprintf(stderr, "[vec_smoke] 100 steps OK\n");

    static_vec_close(vec);
    fprintf(stderr, "[vec_smoke] close OK, crashes=%d\n", crashes);
    return 0;
}
