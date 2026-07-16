/*
 * application_internal.h — Private daemon application test seams.
 */
#ifndef CBM_DAEMON_APPLICATION_INTERNAL_H
#define CBM_DAEMON_APPLICATION_INTERNAL_H

#include "daemon/application.h"

#include <stdbool.h>

/* Read-only test diagnostic for a live session. The caller must serialize
 * this check with request, cancellation, and close callbacks. */
bool cbm_daemon_application_session_retains_store_for_test(
    const cbm_daemon_runtime_application_session_t *session);

#endif /* CBM_DAEMON_APPLICATION_INTERNAL_H */
