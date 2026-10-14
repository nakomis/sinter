/*
 * exec.h — host-side execution harness for compiled images.
 *
 * Stands in for the Phenom II: maps an image as executable memory (write,
 * then flip to read+execute, never both), fakes the scratch/result region
 * with an ordinary buffer, and calls the entry point exactly as the kernel
 * poll loop will. Faults and runaway programs are caught and reported
 * rather than taking the caller down.
 *
 * Only x86-64 POSIX hosts (Linux, Intel macOS) can execute images; on
 * anything else forth_host_can_execute() returns 0 and forth_host_run()
 * reports FORTH_RUN_UNSUPPORTED. Test-and-tooling code only: none of this
 * is meant for the ESP32.
 */
#ifndef SINTER_FORTH_HOST_EXEC_H
#define SINTER_FORTH_HOST_EXEC_H

#include <stddef.h>
#include <stdint.h>

typedef enum forth_run_status {
    FORTH_RUN_OK = 0,
    FORTH_RUN_UNSUPPORTED,   /* host cannot execute x86-64 images */
    FORTH_RUN_MAP_FAILED,    /* mmap/mprotect failed */
    FORTH_RUN_SIGILL,        /* e.g. ran into the UD2 guard tail */
    FORTH_RUN_SIGSEGV,
    FORTH_RUN_SIGBUS,
    FORTH_RUN_SIGFPE,
    FORTH_RUN_TIMEOUT        /* still running after the time limit */
} forth_run_status;

int forth_host_can_execute(void);

/* Execute `code` (len bytes, entry at offset `entry`) with `region`
 * (FORTH_REGION_SIZE bytes) as the scratch/result region. `timeout_s` of
 * 0 means no limit. */
forth_run_status forth_host_run(const uint8_t *code, size_t len, size_t entry,
                                uint8_t *region, unsigned timeout_s);

const char *forth_run_status_name(forth_run_status status);

#endif /* SINTER_FORTH_HOST_EXEC_H */
