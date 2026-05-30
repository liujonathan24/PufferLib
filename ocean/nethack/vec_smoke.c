/* vec_smoke.c — standalone repro for the NetHack vecenv heap corruption.
 *
 * Links the PufferLib vec API + libnethack.so directly (current binding is
 * DIRECT LINKAGE, no dlopen) and drives N envs through many reset/step
 * cycles. The reset cycles are the interesting part: each reset performs
 * nle_end (munmap + unregister the per-env arena) followed by nle_start
 * (sys_early_init -> dupstr sysopt.* into the new arena). Because `sysopt`
 * is a single process-global shared by all envs, its string pointers live
 * in *some* env's per-env arena; a cross-env / post-teardown sysopt_release
 * then frees a pointer whose arena is gone -> __libc_free on arena memory.
 *
 * Build with the debug libnethack.so (compiled with -DNLE_ARENA_DEBUG) so
 * the diagnostic in alloc.c aborts at the exact offending __libc_free.
 *
 *   See build_vec_smoke.sh in repo root.
 *
 * Usage:  OMP_NUM_THREADS=8 ./vec_smoke [N_envs] [n_steps] [force_reset_every]
 *   N_envs            number of envs (default 8)
 *   n_steps           total cpu_vec_step calls (default 50000)
 *   force_reset_every if >0, force-reset every env every K steps (max churn);
 *                     0 = only reset envs that naturally terminate.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

#include "nethack.h"
#define OBS_SIZE NETHACK_OBS_SIZE
#define NUM_ATNS 1
#define ACT_SIZES {NETHACK_NUM_ACTIONS}
#define OBS_TENSOR_T ByteTensor
#define Env Nethack
#include "vecenv.h"

/* CPU-only build: stub the CUDA externs so we can link without nvcc. The
 * cpu path never calls these, but they are referenced in create_static_vec's
 * gpu branch so the linker needs definitions. */
cudaError_t cudaMalloc(void** p, size_t n)              { *p = malloc(n); return 0; }
cudaError_t cudaMemcpy(void* d, const void* s, size_t n, cudaMemcpyKind k) { (void)k; memcpy(d,s,n); return 0; }
cudaError_t cudaMemcpyAsync(void* d, const void* s, size_t n, cudaMemcpyKind k, cudaStream_t st) { (void)k;(void)st; memcpy(d,s,n); return 0; }
cudaError_t cudaMemset(void* p, int v, size_t n)        { memset(p,v,n); return 0; }
cudaError_t cudaFree(void* p)                            { free(p); return 0; }
cudaError_t cudaFreeHost(void* p)                        { free(p); return 0; }
cudaError_t cudaHostAlloc(void** p, size_t n, unsigned f){(void)f; *p = malloc(n); return 0; }
cudaError_t cudaSetDevice(int d)                         { (void)d; return 0; }
cudaError_t cudaDeviceSynchronize(void)                  { return 0; }
cudaError_t cudaStreamSynchronize(cudaStream_t s)        { (void)s; return 0; }
cudaError_t cudaStreamCreateWithFlags(cudaStream_t* s, unsigned f) { (void)f; *s = NULL; return 0; }
cudaError_t cudaStreamQuery(cudaStream_t s)              { (void)s; return 0; }
const char* cudaGetErrorString(cudaError_t e)            { (void)e; return "ok"; }

void my_init(Env* env, Dict* kwargs) {
    (void)kwargs;
    env->num_agents = 1;
    init(env);
}

void my_log(Log* log, Dict* out) {
    dict_set(out, "perf", log->perf);
    dict_set(out, "score", log->depth);
}

int main(int argc, char** argv) {
    int N            = (argc >= 2) ? atoi(argv[1]) : 8;
    long n_steps     = (argc >= 3) ? atol(argv[2]) : 50000;
    int force_every  = (argc >= 4) ? atoi(argv[3]) : 0;

    fprintf(stderr, "[vec_smoke] N=%d steps=%ld force_reset_every=%d omp_max=%d\n",
            N, n_steps, force_every, omp_get_max_threads());

    Dict* vec_kw = create_dict(8);
    Dict* env_kw = create_dict(2);
    dict_set(vec_kw, "total_agents", (double)N);
    dict_set(vec_kw, "num_buffers", 1.0);
    dict_set(vec_kw, "num_threads", 0.0);
    dict_set(vec_kw, "seed", 73.0);

    StaticVec* vec = create_static_vec(N, 1, 0, vec_kw, env_kw);
    fprintf(stderr, "[vec_smoke] vec->size=%d\n", vec->size);

    static_vec_reset(vec);          /* marks pending_reset on all envs   */
    Env* envs = (Env*) vec->envs;

    long resets = 0, terms = 0;
    for (long t = 0; t < n_steps; t++) {
        for (int i = 0; i < vec->size; i++) {
            /* cycle actions so envs actually play (and eventually die) */
            envs[i].actions[0] = (float)((t * 7 + i * 13) % NETHACK_NUM_ACTIONS);
        }
        cpu_vec_step(vec);          /* OMP-parallel c_step over all envs */

        for (int i = 0; i < vec->size; i++) {
            int term = envs[i].terminals[0] > 0.0f;
            int force = (force_every > 0 && (t % force_every) == 0);
            if (term || force) {
                if (term) terms++;
                c_reset(&envs[i]);  /* nle_end+nle_start on next c_step  */
                resets++;
            }
        }
        if ((t % 2000) == 0)
            fprintf(stderr, "[vec_smoke] t=%ld resets=%ld natural_terms=%ld\n",
                    t, resets, terms);
    }

    fprintf(stderr, "[vec_smoke] DONE t=%ld resets=%ld natural_terms=%ld (no crash)\n",
            n_steps, resets, terms);
    static_vec_close(vec);
    fprintf(stderr, "[vec_smoke] close OK\n");
    return 0;
}
