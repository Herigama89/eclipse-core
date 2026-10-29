#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EclipseAI EclipseAI;

/* Creates one mono AI voice-isolation engine running at the host sample rate.
   Returns NULL if it could not be created (the plugin then falls back to the classic engine). */
EclipseAI* eclipse_ai_create (double sampleRate, int maxBlockSize);
void       eclipse_ai_free (EclipseAI*);

/* Total latency in host samples (constant). */
int        eclipse_ai_latency (const EclipseAI*);

/* Maximum noise reduction in dB (0 = off, ~100 = unlimited). */
void       eclipse_ai_set_reduction_db (EclipseAI*, float db);

/* Extra suppression between words (0 = off, max 0.05). */
void       eclipse_ai_set_post_filter (EclipseAI*, float beta);

/* Process n samples. Returns 0 on success, -1 if the engine failed (output = delayed input). */
int        eclipse_ai_process (EclipseAI*, const float* in, float* out, int n);

#ifdef __cplusplus
}
#endif
