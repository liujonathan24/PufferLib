# exp_039 agent_e — DEF_MREAD_SHORT root cause (Cluster BH)

## Verdict: **PASS**

## Hypothesis tested

The save loop terminates with fewer bytes than the matching restore loop reads;
i.e. writer/reader byte-count asymmetry inside the per-level save stream.

Confirmed: writer wrote a pointer-size (8 bytes) for two fields where the
reader read the original array-size (1680 and 240 bytes respectively).

## Divergence (instrumented N=512, 60 s)

Writer-side trace for the file that later faulted (fd=49, path=.../SM1Bxg/.1):

```
SAVE_MCHN_BEGIN fd=49 mode=6 count=2 head=0x153cb8005c60
SAVE_MON kind=monst buflen=144
SAVE_MON kind=monst buflen=144
SAVE_MCHN_END   fd=49 mode=6 wrote_sentinel=2
DEF_BCLOSE_SIZE fd=49 size_before_close=16618
```

Reader-side trace, same file:

```
OPEN_LEVELFILE  fd=49 size=16618
REST_MCHN_BEGIN fd=49 ghostly=0
REST_MCHN iter=0 gate_buflen=96       <-- expected 144, got 96 (= sizeof obj)
REST_MON  kind=mname buflen=0
REST_MON  kind=egd   buflen=3072      <-- never written; reader is misaligned
REST_MON  kind=epri  buflen=0
REST_MON  kind=eshk  buflen=0
REST_MON  kind=emin  buflen=0
REST_MON  kind=edog  buflen=0
REST_MON  kind=corpsenm
REST_MCHN iter=1 gate_buflen=96
REST_MON  kind=mname buflen=0
REST_MON  kind=egd   buflen=3072
DEF_MREAD_SHORT pos=16618 size=16618 expected=324 got=272
```

The very first `gate_buflen` the reader pulled was 96 (sizeof struct obj),
not 144 (sizeof struct monst). The reader was already misaligned by the time
it reached `restmonchn`. The misalignment had to originate upstream — in
fields read before the monster chain inside `getlev`.

## Racing global / root cause

Two stage-7' migrations turned arrays into pointer macros:

* `vendor/nle/src/include/rm.h:623` — `#define lastseentyp ((schar (*)[ROWNO]) current_nle_ctx->s7_lastseentyp_p)`
* `vendor/nle/src/include/mkroom.h:55` — `#define doors (current_nle_ctx->s7_doors_p)`

`restore.c` was updated for these (1129 and 1141 use explicit byte counts
`COLNO * ROWNO * sizeof(schar)` and `DOORMAX * sizeof(coord)`), but
`save.c:574 / 584` (pre-fix) still wrote `sizeof lastseentyp` and
`sizeof doors`, both of which now evaluate to `sizeof(pointer) == 8`.

| Field        | Writer wrote | Reader read | Drift  |
|--------------|--------------|-------------|--------|
| lastseentyp  | 8 B          | 1680 B      | -1672  |
| doors        | 8 B          | 240 B       |  -232  |

Cumulative ~1.9 KB of reader-side over-consumption per restore, which slides
every subsequent record (timers, lights, monsters, traps, objs) out of
alignment. The reader eventually decodes a random byte sequence as a buflen
and asks for more bytes than remain in the file — the `DEF_MREAD_SHORT`
panic. Intermittent because `getlev` only runs when an agent revisits a
previously-saved level, which is rare for early-game training.

## Fix (Cluster BH)

`vendor/nle/src/src/save.c:573-596` — replace `sizeof lastseentyp` with
`COLNO * ROWNO * sizeof(schar)` and `sizeof doors` with
`DOORMAX * sizeof (coord)` to match the reader.

Commit: see git log (Cluster BH commit on branch 4.0).

## Instrumentation

`NLE_TRACE_MON=1` env-gated trace added in `save.c::savemon/savemonchn`
and `restore.c::restmon/restmonchn`. Zero overhead when unset
(one TLS load + branch). Useful for any future save-stream mismatch.

## Verification

| Run             | N    | duration | DEF_MREAD_SHORT |
|-----------------|------|----------|-----------------|
| baseline (pre)  | 64   | ~60 s    | 1               |
| baseline (pre)  | 512  | ~60 s    | 2               |
| baseline (pre)  | 1024 | ~60 s    | 2               |
| **fix** (post)  | 512  | 60 s     | **0**           |
| **fix** (post)  | 512  | 60 s     | **0** (no trace)|
| **fix** (post)  | 1024 | 60 s     | **0**           |

Training runs cleanly at N=1024 with no panic and no "Error reading level file."
