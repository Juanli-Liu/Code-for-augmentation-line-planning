#ifndef CADICAL_MSVC_COMPAT_HPP
#define CADICAL_MSVC_COMPAT_HPP

#ifdef _MSC_VER

#include <stdio.h>
#include <intrin.h>
#include <sys/stat.h>

#ifndef __PRETTY_FUNCTION__
#define __PRETTY_FUNCTION__ __FUNCSIG__
#endif

#ifndef __attribute__
#define __attribute__(attributes)
#endif

#ifndef __builtin_unreachable
#define __builtin_unreachable() __assume(0)
#endif

#ifndef __builtin_prefetch
#define __builtin_prefetch(address, ...) ((void) (address))
#endif

static inline unsigned cadical_msvc_count_leading_zeroes (unsigned value) {
  unsigned long index;
  _BitScanReverse (&index, value);
  return 31u - index;
}

#ifndef __builtin_clz
#define __builtin_clz(value) cadical_msvc_count_leading_zeroes(value)
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif

#ifndef S_ISFIFO
#define S_ISFIFO(mode) (((mode) & _S_IFMT) == _S_IFIFO)
#endif

#ifndef popen
#define popen _popen
#endif

#ifndef pclose
#define pclose _pclose
#endif

#endif

#endif
