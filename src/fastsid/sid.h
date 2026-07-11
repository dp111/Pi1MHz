/* Minimal sid.h — engine hooks only (from VICE 3.1, trimmed). */
#ifndef VICE_SID_ENGINE_H
#define VICE_SID_ENGINE_H

#include "types.h"
#include "sound.h"

struct sound_s;
struct sid_snapshot_state_s;

struct sid_engine_s {
    struct sound_s *(*open)(BYTE *sidstate);
    int (*init)(struct sound_s *psid, int speed, int cycles_per_sec, int factor);
    void (*close)(struct sound_s *psid);
    BYTE (*read)(struct sound_s *psid, WORD addr);
    void (*store)(struct sound_s *psid, WORD addr, BYTE val);
    void (*reset)(struct sound_s *psid, CLOCK cpu_clk);
    int (*calculate_samples)(struct sound_s *psid, SWORD *pbuf, int nr,
                             int interleave, int *delta_t);
    void (*prevent_clk_overflow)(struct sound_s *psid, CLOCK sub);
    char *(*dump_state)(struct sound_s *psid);
    void (*state_read)(struct sound_s *psid,
                       struct sid_snapshot_state_s *sid_state);
    void (*state_write)(struct sound_s *psid,
                        struct sid_snapshot_state_s *sid_state);
};
typedef struct sid_engine_s sid_engine_t;

#endif
