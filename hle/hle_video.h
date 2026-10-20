#ifndef HLE_VIDEO_H
#define HLE_VIDEO_H
#include <fmv.h>
/** Plays the ".hbv" version of `video_fn` (HLE build replacement of fmv_play). */
void hle_fmv_play(const char *video_fn, const fmv_parms_t *parms);
#endif
