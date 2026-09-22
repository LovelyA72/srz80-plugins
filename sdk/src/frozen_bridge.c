#include "frozen_bridge.h"

#include <string.h>

#include <frozen.h>

struct Bridge {
    SrzFrozenVisit visit;
    void *context;
};

_Static_assert((int)SRZ_FROZEN_INVALID == (int)JSON_TYPE_INVALID, "token type mismatch");
_Static_assert((int)SRZ_FROZEN_ARRAY_END == (int)JSON_TYPE_ARRAY_END, "token type mismatch");
_Static_assert(SRZ_FROZEN_STRING_INVALID == JSON_STRING_INVALID, "error code mismatch");
_Static_assert(SRZ_FROZEN_STRING_INCOMPLETE == JSON_STRING_INCOMPLETE, "error code mismatch");
_Static_assert(SRZ_FROZEN_DEPTH_LIMIT == JSON_DEPTH_LIMIT, "error code mismatch");

static void visit_token(void *context, const char *name, size_t name_size, const char *path,
                        const struct json_token *token) {
    struct Bridge *bridge = context;
    (void)path;
    bridge->visit(bridge->context, name, name_size, token ? token->ptr : NULL,
                  token ? token->len : 0,
                  token ? (enum SrzFrozenTokenType)token->type : SRZ_FROZEN_INVALID);
}

int srz_frozen_walk(const char *input, int input_size, int depth_limit,
                    SrzFrozenVisit visit, void *context) {
    struct Bridge bridge = {visit, context};
    struct frozen_args args;
    INIT_FROZEN_ARGS(&args);
    args.callback = visit_token;
    args.callback_data = &bridge;
    args.limit = depth_limit;
    return json_walk_args(input, input_size, &args);
}
