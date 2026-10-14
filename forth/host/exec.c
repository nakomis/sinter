/*
 * exec.c — see exec.h.
 */
#define _DEFAULT_SOURCE      /* MAP_ANONYMOUS, sigsetjmp on glibc */
#define _DARWIN_C_SOURCE     /* MAP_ANON on macOS */

#include "exec.h"

const char *forth_run_status_name(forth_run_status status)
{
    switch (status) {
    case FORTH_RUN_OK:          return "ok";
    case FORTH_RUN_UNSUPPORTED: return "unsupported host";
    case FORTH_RUN_MAP_FAILED:  return "mmap/mprotect failed";
    case FORTH_RUN_SIGILL:      return "SIGILL";
    case FORTH_RUN_SIGSEGV:     return "SIGSEGV";
    case FORTH_RUN_SIGBUS:      return "SIGBUS";
    case FORTH_RUN_SIGFPE:      return "SIGFPE";
    case FORTH_RUN_TIMEOUT:     return "timeout";
    }
    return "unknown";
}

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))

#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static sigjmp_buf fault_jmp;

static void on_fault(int sig)
{
    siglongjmp(fault_jmp, sig);
}

int forth_host_can_execute(void)
{
    return 1;
}

forth_run_status forth_host_run(const uint8_t *code, size_t len, size_t entry,
                                uint8_t *region, unsigned timeout_s)
{
    static const int sigs[] = { SIGILL, SIGSEGV, SIGBUS, SIGFPE, SIGALRM };
    struct sigaction sa, old[sizeof sigs / sizeof sigs[0]];
    long page = sysconf(_SC_PAGESIZE);
    size_t maplen = (len + (size_t)page - 1) & ~((size_t)page - 1);
    void (*fn)(uint8_t *);
    volatile forth_run_status result = FORTH_RUN_OK;
    uint8_t *mem;
    unsigned char *entry_addr;
    int sig;

    if (len == 0 || entry >= len) {
        return FORTH_RUN_MAP_FAILED;
    }
    mem = mmap(NULL, maplen, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return FORTH_RUN_MAP_FAILED;
    }
    memcpy(mem, code, len);
    /* W^X: never writable and executable at once. */
    if (mprotect(mem, maplen, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, maplen);
        return FORTH_RUN_MAP_FAILED;
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        sigaction(sigs[i], &sa, &old[i]);
    }

    /* ISO C has no object-to-function pointer conversion; copy the bits. */
    entry_addr = mem + entry;
    memcpy(&fn, &entry_addr, sizeof fn);

    sig = sigsetjmp(fault_jmp, 1);
    if (sig == 0) {
        alarm(timeout_s);
        fn(region);
        alarm(0);
    } else {
        alarm(0);
        result = sig == SIGILL  ? FORTH_RUN_SIGILL
               : sig == SIGSEGV ? FORTH_RUN_SIGSEGV
               : sig == SIGBUS  ? FORTH_RUN_SIGBUS
               : sig == SIGFPE  ? FORTH_RUN_SIGFPE
                                : FORTH_RUN_TIMEOUT;
    }

    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        sigaction(sigs[i], &old[i], NULL);
    }
    munmap(mem, maplen);
    return result;
}

#else /* not an x86-64 POSIX host */

int forth_host_can_execute(void)
{
    return 0;
}

forth_run_status forth_host_run(const uint8_t *code, size_t len, size_t entry,
                                uint8_t *region, unsigned timeout_s)
{
    (void)code;
    (void)len;
    (void)entry;
    (void)region;
    (void)timeout_s;
    return FORTH_RUN_UNSUPPORTED;
}

#endif
