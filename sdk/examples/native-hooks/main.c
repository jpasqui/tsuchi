// Two inline hooks with all-location checks before installation. This example
// contains no game targets or mod behavior; supply yours in targets.h and below.
#include "tsuchi/tsuchi.h"
#include "targets.h"

typedef void (*InlineHandler)(TSUCHI_InlineContext* context);
static TSUCHI_Hook delivered;
static TSUCHI_Hook model;

static void on_delivered(TSUCHI_InlineContext* context) {
    uint64_t* x = (uint64_t*)context->registers;
    (void)x; // Put your original callback's register logic here.
    ((InlineHandler)delivered.original)(context);
}

static void on_model(TSUCHI_InlineContext* context) {
    uint64_t* x = (uint64_t*)context->registers;
    (void)x; // Put your original callback's register logic here.
    ((InlineHandler)model.original)(context);
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host* host, TSUCHI_Mod* mod) {
    delivered = (TSUCHI_Hook){0};
    model = (TSUCHI_Hook){0};
    if (!TSUCHI_HAS(host, hook) || !TSUCHI_HAS(host->hook, wrap_inline) ||
        !TSUCHI_HAS(host->hook, check_code)) return TSUCHI_NOT_READY;
    if (!delivered_offset || !model_offset) return TSUCHI_INVALID;
    TSUCHI_Result result = host->hook->check_code(mod, delivered_offset, delivered_expected, sizeof delivered_expected);
    if (result != TSUCHI_OK) return result;
    result = host->hook->check_code(mod, model_offset, model_expected, sizeof model_expected);
    if (result != TSUCHI_OK) return result;
    result = host->hook->wrap_inline(mod, delivered_offset, delivered_expected[0], (void*)on_delivered, &delivered);
    if (result != TSUCHI_OK) return result;
    result = host->hook->wrap_inline(mod, model_offset, model_expected[0], (void*)on_model, &model);
    if (result != TSUCHI_OK) {
        host->hook->remove(mod, delivered.id);
        delivered = (TSUCHI_Hook){0};
    }
    return result;
}
