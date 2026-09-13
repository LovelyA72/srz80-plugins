#pragma once
/*
 * Host compatibility shim; upstream libasm sources remain unchanged.
 *
 * libasm's float80_hard.h selects the in-memory layout of an 80-bit long double
 * from __BYTE_ORDER, which glibc hosts receive transitively through <endian.h>
 * or <sys/param.h>.  MinGW-w64 ships neither header, and its GCC is a true
 * 80-bit long double target, so the macro has to come from GCC's own byte-order
 * builtins instead.
 */
#ifndef __BYTE_ORDER
#if defined(__BYTE_ORDER__)
#define __BYTE_ORDER __BYTE_ORDER__
#else
#error "Cannot determine the host byte order for libasm"
#endif
#endif
#ifndef __LITTLE_ENDIAN
#if defined(__ORDER_LITTLE_ENDIAN__)
#define __LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#endif
#endif
#ifndef __BIG_ENDIAN
#if defined(__ORDER_BIG_ENDIAN__)
#define __BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif
#endif
