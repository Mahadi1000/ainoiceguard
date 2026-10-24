/**
 * DeepFilterNet libDF C API -- declarations for tag v0.5.6 (commit 978576a).
 *
 * Written by hand from libDF/src/capi.rs at that tag instead of generating a
 * header with cbindgen (one less build tool). Keep in sync if the pinned
 * version changes: note that later versions add a third `log_level`
 * parameter to df_create().
 *
 * Behaviour notes (from capi.rs / tract.rs, v0.5.6):
 *   - df_create() loads a DeepFilterNet tar.gz ONNX model; mono only. It
 *     panics (aborting the process) if the model cannot be loaded.
 *   - All state (STFT buffers, recurrent model state) lives in DFState and is
 *     kept across df_process_frame() calls. There is no reset: free + create.
 *   - df_process_frame() processes exactly df_get_frame_length() samples and
 *     returns the frame's local SNR (dB). No delay compensation is applied.
 *
 * Licence: libDF is MIT OR Apache-2.0.
 */

#ifndef AINOICEGUARD_DFN_CAPI_H
#define AINOICEGUARD_DFN_CAPI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DFState DFState;

DFState* df_create(const char* path, float atten_lim);
size_t df_get_frame_length(DFState* st);
void df_set_atten_lim(DFState* st, float lim_db);
void df_set_post_filter_beta(DFState* st, float beta);
float df_process_frame(DFState* st, float* input, float* output);
void df_free(DFState* model);

#ifdef __cplusplus
}
#endif

#endif  // AINOICEGUARD_DFN_CAPI_H
