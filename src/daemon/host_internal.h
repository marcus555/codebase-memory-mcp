/*
 * host_internal.h — Private daemon-host test seams.
 */
#ifndef CBM_DAEMON_HOST_INTERNAL_H
#define CBM_DAEMON_HOST_INTERNAL_H

#include <stdbool.h>

typedef bool (*cbm_daemon_host_cleanup_release_for_test_fn)(void *context);

/* Runs the same finite retry/fail-stop driver used by daemon-claim shutdown
 * around an injected release operation. The hook is process-local and inert
 * unless a focused subprocess test calls this seam. */
void cbm_daemon_host_cleanup_release_until_complete_for_test(
    cbm_daemon_host_cleanup_release_for_test_fn release, void *context);

/* Opens the ordinary private daemon operation log, records the same forced
 * shutdown event used by production, flushes it, and terminates the process.
 * This exists only so an isolated child can verify the hard escape path. */
_Noreturn void cbm_daemon_host_force_terminate_for_test(
    const char *component);

#endif /* CBM_DAEMON_HOST_INTERNAL_H */
