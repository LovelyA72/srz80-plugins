/* Force-included into pocketpy and into this plugin's own sources.
 *
 * Two problems are handled here:
 *
 * pocketpy marks its whole C API with PK_API, which expands to
 * __declspec(dllexport) for any Windows build and to default visibility
 * elsewhere.  The card links pocketpy statically, so those symbols would be
 * re-exported from misc_script and could collide with another plugin that
 * embeds a different pocketpy.  Including export.h first and redefining PK_API
 * keeps the runtime internal to this module.
 *
 * pocketpy's time module also calls the C11 timespec_get/TIME_UTC pair, which
 * MinGW-w64 declares only for the UCRT runtime.  The shim supplies it for the
 * default msvcrt runtime and is a no-op when the pair already exists.
 */
#ifndef SRZ80_POCKETPY_COMPAT_H
#define SRZ80_POCKETPY_COMPAT_H

#include <pocketpy/export.h>

#undef PK_API
#define PK_API

#if defined(_WIN32) && !defined(_UCRT) && !defined(__cplusplus)

#include <sys/types.h>
#include <time.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX

#ifndef TIME_UTC
#define TIME_UTC 1
#endif

static __inline int srz80_pocketpy_timespec_get(struct timespec *out, int base) {
    /* Ticks between 1601-01-01 (FILETIME epoch) and 1970-01-01 (Unix epoch). */
    const unsigned long long unix_epoch_offset = 116444736000000000ULL;
    FILETIME file_time;
    ULARGE_INTEGER ticks;
    unsigned long long unix_ticks;
    if (base != TIME_UTC) return 0;
    GetSystemTimeAsFileTime(&file_time);
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    unix_ticks = ticks.QuadPart - unix_epoch_offset;
    out->tv_sec = (time_t)(unix_ticks / 10000000ULL);
    out->tv_nsec = (long)((unix_ticks % 10000000ULL) * 100ULL);
    return base;
}

#define timespec_get srz80_pocketpy_timespec_get

#endif /* _WIN32 && !_UCRT && !__cplusplus */

#endif /* SRZ80_POCKETPY_COMPAT_H */
