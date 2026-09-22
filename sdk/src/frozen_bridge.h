#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum SrzFrozenTokenType {
    SRZ_FROZEN_INVALID,
    SRZ_FROZEN_STRING,
    SRZ_FROZEN_NUMBER,
    SRZ_FROZEN_TRUE,
    SRZ_FROZEN_FALSE,
    SRZ_FROZEN_NULL,
    SRZ_FROZEN_OBJECT_START,
    SRZ_FROZEN_OBJECT_END,
    SRZ_FROZEN_ARRAY_START,
    SRZ_FROZEN_ARRAY_END,
};

enum {
    SRZ_FROZEN_STRING_INVALID = -1,
    SRZ_FROZEN_STRING_INCOMPLETE = -2,
    SRZ_FROZEN_DEPTH_LIMIT = -3,
};

typedef void (*SrzFrozenVisit)(void *context, const char *name, size_t name_size,
                               const char *value, int value_size,
                               enum SrzFrozenTokenType type);

int srz_frozen_walk(const char *input, int input_size, int depth_limit,
                    SrzFrozenVisit visit, void *context);

#ifdef __cplusplus
}
#endif
