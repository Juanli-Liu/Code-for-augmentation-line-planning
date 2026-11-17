#ifndef CADICAL_MSVC_UNISTD_H
#define CADICAL_MSVC_UNISTD_H

// Minimal POSIX compatibility layer used when CaDiCaL is compiled with MSVC.
// The upstream sources include <unistd.h>; MSVC exposes the required file and
// process functions through <io.h> and <process.h> instead.

#ifdef _WIN32

#include <io.h>
#include <process.h>

#ifndef R_OK
#define R_OK 4
#endif

#ifndef W_OK
#define W_OK 2
#endif

#define access _access
#define fileno _fileno
#define getpid _getpid

#else

#include_next <unistd.h>

#endif

#endif
