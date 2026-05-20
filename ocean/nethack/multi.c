// Intra-process multi-env benchmark: hold M Nethack envs in one process, each
// with its own dlopen-copy of libnethack.so, and round-robin step them.
//
// Usage: ./nethack_multi NUM_ENVS STEPS_PER_ENV OUT_JSON [random|wait]
//
// Hypothesis: each env has its own globals (dlopen-per-instance), so N envs
// can run independently in one process. When one is mid-reset, others keep
// stepping. Linear scaling to ~64 envs/process expected.

#include <time.h>
#include <unistd.h>
#include <string.h>
#define NETHACK_USE_BLSTATS 1
#include "nethack.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char** argv) {
    int num_envs = (argc >= 2) ? atoi(argv[1]) : 4;
    long steps_per_env = (argc >= 3) ? atol(argv[2]) : 10000;
    const char* out_json = (argc >= 4) ? argv[3] : "/dev/null";
    int policy = (argc >= 5 && strcmp(argv[4], "wait") == 0) ? 1 : 0;

    Nethack* envs = (Nethack*)calloc(num_envs, sizeof(Nethack));
    if (!envs) { perror("calloc envs"); return 1; }

    double t_init0 = now_sec();
    for (int i = 0; i < num_envs; i++) {
        envs[i].num_agents = 1;
        envs[i].observations = (unsigned char*)calloc(NETHACK_OBS_SIZE, 1);
        envs[i].actions      = (float*)calloc(1, sizeof(float));
        envs[i].rewards      = (float*)calloc(1, sizeof(float));
        envs[i].terminals    = (float*)calloc(1, sizeof(float));
        init(&envs[i]);
        c_reset(&envs[i]);
    }
    double init_dt = now_sec() - t_init0;
    printf("init+first-reset: %d envs in %.3fs (%.1f ms/env)\n",
           num_envs, init_dt, init_dt * 1000.0 / num_envs);

    srand((unsigned)0xC0FFEE);
    long total_steps = (long)num_envs * steps_per_env;
    double t0 = now_sec();
    for (long t = 0; t < steps_per_env; t++) {
        for (int i = 0; i < num_envs; i++) {
            envs[i].actions[0] = (policy == 1) ? 18.0f : (float)(rand() % NETHACK_NUM_ACTIONS);
            c_step(&envs[i]);
        }
    }
    double dt = now_sec() - t0;

    long valid = 0;
    for (int i = 0; i < num_envs; i++) valid += envs[i].episode_valid_moves;
    // (Doesn't include valid moves from completed episodes — those were
    //  zeroed on auto-reset. Use profile counters instead.)

    printf("steps_per_env=%ld total_steps=%ld wall=%.3fs\n",
           steps_per_env, total_steps, dt);
    printf("aggregate c_steps/sec  = %.0f\n", total_steps / dt);
    printf("aggregate valid_moves/sec (last episode only, undercounts) = %.0f\n", valid / dt);

    FILE* f = (strcmp(out_json, "-") == 0) ? stdout : (strcmp(out_json, "/dev/null") == 0 ? NULL : fopen(out_json, "w"));
    if (f) {
        prof_dump_json(f, dt);
        if (f != stdout) fclose(f);
    }

    for (int i = 0; i < num_envs; i++) {
        c_close(&envs[i]);
        free(envs[i].observations);
        free(envs[i].actions);
        free(envs[i].rewards);
        free(envs[i].terminals);
    }
    free(envs);
    return 0;
}
