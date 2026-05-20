#include "nethack.h"
#define OBS_SIZE NETHACK_OBS_SIZE
#define NUM_ATNS 1
#define ACT_SIZES {NETHACK_NUM_ACTIONS}
#define OBS_TENSOR_T ByteTensor

#define Env Nethack
#include "vecenv.h"

void my_init(Env* env, Dict* kwargs) {
    env->num_agents = 1;
    init(env);
}

void my_log(Log* log, Dict* out) {
    dict_set(out, "perf", log->perf);
    dict_set(out, "score", log->score);
    dict_set(out, "episode_return", log->episode_return);
    dict_set(out, "episode_length", log->episode_length);
    dict_set(out, "depth", log->depth);
    dict_set(out, "valid_moves", log->valid_moves);
    dict_set(out, "illegal_actions", log->illegal_actions);
}
