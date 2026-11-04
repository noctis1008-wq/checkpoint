/* glibc stand-ins for the newlib bits scriptconsole.cpp uses */
#pragma once
#include <stdio.h>
#include <sys/types.h>
#define _READ_WRITE_BUFSIZE_TYPE size_t
typedef int (*sim_writefn)(void*, const char*, size_t);
static sim_writefn sim_console_writefn;
static ssize_t sim_cookie_write(void* c, const char* b, size_t n) { return sim_console_writefn(c, b, n); }
static inline FILE* funopen(const void* cookie, void* readfn, sim_writefn writefn, void* seekfn, void* closefn)
{
    (void)readfn; (void)seekfn; (void)closefn;
    sim_console_writefn = writefn;
    cookie_io_functions_t io = { NULL, sim_cookie_write, NULL, NULL };
    return fopencookie((void*)cookie, "w", io);
}
