#ifndef P4_MODDING_HTTPS_H
#define P4_MODDING_HTTPS_H
#include "result.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental HTTPS GET transfers. Public calls are game-thread only;
 * DNS, TLS, HTTP and storage run on framework workers. No mod/UI callback
 * runs there. Poll snapshots and read completed memory responses instead.
 * Descriptors are copied. release invalidates the handle immediately but
 * retains worker storage until cancellation has drained. */
#define P4M_HTTPS_ABI 1u
#define P4M_SERVICE_HTTPS "p4m.https"
#define P4M_HTTPS_MAX_REQUESTS 8u
#define P4M_HTTPS_URL_BYTES 2048u
#define P4M_HTTPS_PATH_BYTES 192u
#define P4M_HTTPS_MAX_MEMORY (4u<<20)
#define P4M_HTTPS_MAX_FILE (512ull<<20)
typedef uint32_t P4M_Transfer;
enum { P4M_HTTPS_MEMORY=1u, P4M_HTTPS_FILE=2u };
enum { P4M_HTTPS_ASYNC=1u, P4M_HTTPS_VERIFIED_TLS=2u,
       P4M_HTTPS_MEMORY_RESULTS=4u, P4M_HTTPS_FILE_RESULTS=8u };
enum { P4M_HTTPS_QUEUED=1u, P4M_HTTPS_RUNNING=2u,
       P4M_HTTPS_SUCCEEDED=3u, P4M_HTTPS_FAILED=4u, P4M_HTTPS_CANCELLED=5u };
enum { P4M_HTTPS_ERROR_NONE=0u, P4M_HTTPS_ERROR_NETWORK=1u,
       P4M_HTTPS_ERROR_TLS=2u, P4M_HTTPS_ERROR_HTTP=3u,
       P4M_HTTPS_ERROR_TIMEOUT=4u, P4M_HTTPS_ERROR_LIMIT=5u,
       P4M_HTTPS_ERROR_STORAGE=6u, P4M_HTTPS_ERROR_PROTOCOL=7u };
typedef struct P4M_HttpsRequest {
    uint32_t size, kind;
    const char *url; /* https only, no credentials or fragment; ASCII/percent encoded */
    const char *path; /* FILE: relative lowercase cache key under this owner;
                      * [a-z0-9._-/], no hidden/empty/dot-ending segments.
                      * Existing complete keys are never replaced. MEMORY: null */
    uint64_t max_bytes; /* mandatory; never buffer an archive in RAM */
    uint32_t connect_timeout_ms, timeout_ms; /* 1..60000 / 1..3600000 */
} P4M_HttpsRequest;
typedef struct P4M_HttpsStatus {
    uint32_t size, state, error, http_status;
    int32_t transport_error;
    uint32_t reserved;
    uint64_t received, total; /* total 0 means unknown; received is stored body bytes */
} P4M_HttpsStatus;
typedef struct P4M_HttpsApi {
    uint32_t size, abi;
    P4M_Result (*capabilities)(uint32_t *flags);
    P4M_Result (*get)(const char *owner_id, const P4M_HttpsRequest *, P4M_Transfer *out);
    P4M_Result (*status)(P4M_Transfer, P4M_HttpsStatus *out);
    /* Only successful MEMORY results. No network or storage access on this call. */
    P4M_Result (*read)(P4M_Transfer, uint64_t offset, void *buffer, uint32_t capacity, uint32_t *read);
    P4M_Result (*cancel)(P4M_Transfer); /* queued/running; completion may already have won */
    P4M_Result (*release)(P4M_Transfer); /* cancels if necessary, invalidates handle */
} P4M_HttpsApi;
const P4M_HttpsApi *p4m_get_https_api(uint32_t abi);
#ifdef __cplusplus
}
#endif
#endif
