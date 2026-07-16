/*
 * host.c — Lifecycle owner for the mandatory per-account CBM daemon.
 */
#include "daemon/host.h"
#include "daemon/host_internal.h"

#include "daemon/application.h"
#include "daemon/runtime.h"
#include "daemon/project_lock.h"
#include "daemon/version_cohort.h"
#include "cli/cli.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/diagnostics.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/platform.h"
#include "mcp/index_supervisor.h"
#include "store/store.h"
#include "ui/config.h"
#include "ui/embedded_assets.h"
#include "ui/http_server.h"
#include "watcher/watcher.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HOST_MAX_CLIENTS = 64,
    HOST_LEASE_TIMEOUT_MS = 30000,
    HOST_REQUEST_TIMEOUT_MS = 5000,
    HOST_RUNTIME_SHUTDOWN_MS = 10000,
    HOST_APPLICATION_SHUTDOWN_MS = 10000,
    HOST_COORDINATION_CLEANUP_MS = 500,
    HOST_INITIAL_CLIENT_TIMEOUT_MS = 10000,
    HOST_WAIT_TICK_MS = 100,
    HOST_WATCH_INTERVAL_MS = 5000,
    HOST_CONFLICT_LOG_CAP = 1024 * 1024,
    HOST_OPERATION_LOG_CAP = 5 * 1024 * 1024,
    HOST_PATH_CAP = 4096,
#ifdef _WIN32
    /* LockFileEx has no non-owning F_GETLK equivalent. A lifetime probe must
     * momentarily take and release the private file range. The child can also
     * briefly lose the legacy-mutex handoff to another waiting starter, so it
     * retries both bounded observation/compatibility windows. */
    HOST_WINDOWS_LIFETIME_ACQUIRE_MS = 250,
#endif
};

typedef struct {
    cbm_daemon_application_t *application;
    cbm_watcher_t *watcher;
    cbm_store_t *watch_store;
    cbm_config_t *runtime_config;
    cbm_project_lock_manager_t *project_locks;
    cbm_http_server_t *http;
    cbm_thread_t watcher_thread;
    cbm_thread_t http_thread;
    bool watcher_started;
    bool http_started;
    bool http_config_enabled;
    int http_config_port;
} host_state_t;

static FILE *g_host_log_file = NULL;
static cbm_mutex_t g_host_log_mutex;
static bool g_host_log_mutex_initialized = false;

static _Noreturn void host_force_terminate(const char *component);

static void host_log_sink(const char *line) {
    cbm_ui_log_append(line);
    if (!g_host_log_file || !g_host_log_mutex_initialized) {
        return;
    }
    cbm_mutex_lock(&g_host_log_mutex);
    (void)fprintf(g_host_log_file, "%s\n", line ? line : "");
    (void)fflush(g_host_log_file);
    cbm_mutex_unlock(&g_host_log_mutex);
}

static bool host_log_open(char conflict_log_out[HOST_PATH_CAP]) {
    const char *cache = cbm_resolve_cache_dir();
    if (!cache || !cache[0] || !conflict_log_out) {
        return false;
    }
    char logs[HOST_PATH_CAP];
    int written = snprintf(logs, sizeof(logs), "%s/logs", cache);
    if (written <= 0 || written >= (int)sizeof(logs)) {
        return false;
    }
    written = snprintf(conflict_log_out, HOST_PATH_CAP,
                       "%s/daemon-conflicts.ndjson", logs);
    if (written <= 0 || written >= HOST_PATH_CAP) {
        return false;
    }
    g_host_log_file = cbm_daemon_ipc_private_log_open(
        logs, "cbm-daemon.log", HOST_OPERATION_LOG_CAP);
    if (!g_host_log_file) {
        conflict_log_out[0] = '\0';
        return false;
    }
    cbm_mutex_init(&g_host_log_mutex);
    g_host_log_mutex_initialized = true;
    cbm_log_set_sink(host_log_sink);
    return true;
}

static void host_log_close(void) {
    cbm_log_set_sink(NULL);
    if (g_host_log_mutex_initialized) {
        cbm_mutex_lock(&g_host_log_mutex);
    }
    FILE *file = g_host_log_file;
    g_host_log_file = NULL;
    if (file) {
        (void)fclose(file);
    }
    if (g_host_log_mutex_initialized) {
        cbm_mutex_unlock(&g_host_log_mutex);
        cbm_mutex_destroy(&g_host_log_mutex);
        g_host_log_mutex_initialized = false;
    }
}

static uint64_t host_deadline_after(uint32_t timeout_ms) {
    uint64_t now = cbm_now_ms();
    return now > UINT64_MAX - timeout_ms ? UINT64_MAX : now + timeout_ms;
}

static _Noreturn void host_cleanup_force_terminate(const char *component) {
    /* Early startup failures can precede the ordinary operation-log setup.
     * Make one best-effort attempt to establish that same owner-only durable
     * sink before the process-level escape releases every native claim. */
    if (!g_host_log_file) {
        char conflict_log[HOST_PATH_CAP];
        cbm_ui_log_init();
        (void)host_log_open(conflict_log);
    }
    host_force_terminate(component);
}

static void host_cleanup_release_until_complete(
    cbm_daemon_host_cleanup_release_for_test_fn release, void *context,
    const char *component) {
    if (!release) {
        return;
    }
    uint64_t deadline = host_deadline_after(HOST_COORDINATION_CLEANUP_MS);
    while (!release(context)) {
        if (cbm_now_ms() >= deadline) {
            host_cleanup_force_terminate(component);
        }
        cbm_usleep(1000);
    }
}

void cbm_daemon_host_cleanup_release_until_complete_for_test(
    cbm_daemon_host_cleanup_release_for_test_fn release, void *context) {
    host_cleanup_release_until_complete(release, context,
                                        "coordination_cleanup");
}

static bool host_cohort_lease_release_once(void *context) {
    cbm_version_cohort_lease_t **lease = context;
    if (!lease || !*lease) {
        return true;
    }
    (void)cbm_version_cohort_lease_release(lease);
    return *lease == NULL;
}

static bool host_cohort_manager_free_once(void *context) {
    cbm_version_cohort_manager_t **manager = context;
    if (!manager || !*manager) {
        return true;
    }
    (void)cbm_version_cohort_manager_free(manager);
    return *manager == NULL;
}

static void host_cohort_close(cbm_version_cohort_lease_t **lease,
                              cbm_version_cohort_manager_t **manager) {
    host_cleanup_release_until_complete(host_cohort_lease_release_once, lease,
                                        "cohort_lease_cleanup");
    host_cleanup_release_until_complete(host_cohort_manager_free_once, manager,
                                        "cohort_manager_cleanup");
}

static bool host_daemon_claim_release_once(void *context) {
    cbm_version_cohort_daemon_claim_t **claim = context;
    if (!claim || !*claim) {
        return true;
    }
    (void)cbm_version_cohort_daemon_claim_release(claim);
    return *claim == NULL;
}

static void host_daemon_claim_close(
    cbm_version_cohort_daemon_claim_t **claim) {
    host_cleanup_release_until_complete(host_daemon_claim_release_once,
                                        claim, "daemon_claim_cleanup");
}

static bool host_participant_guard_release_once(void *context) {
    cbm_daemon_ipc_participant_guard_t **guard = context;
    if (!guard || !*guard) {
        return true;
    }
    (void)cbm_daemon_ipc_participant_guard_release(guard);
    return *guard == NULL;
}

static void host_participant_guard_close(
    cbm_daemon_ipc_participant_guard_t **guard) {
    host_cleanup_release_until_complete(host_participant_guard_release_once,
                                        guard, "participant_guard_cleanup");
}

static int host_lifetime_reservation_acquire(
    const cbm_daemon_ipc_endpoint_t *endpoint,
    cbm_daemon_ipc_lifetime_reservation_t **reservation_out) {
#ifdef _WIN32
    uint64_t now = cbm_now_ms();
    uint64_t deadline =
        now > UINT64_MAX - HOST_WINDOWS_LIFETIME_ACQUIRE_MS
            ? UINT64_MAX
            : now + HOST_WINDOWS_LIFETIME_ACQUIRE_MS;
    do {
        int status = cbm_daemon_ipc_lifetime_reservation_try_acquire(
            endpoint, reservation_out);
        if (status != 0) {
            return status;
        }
        cbm_usleep(1000);
    } while (cbm_now_ms() < deadline);
    return 0;
#else
    return cbm_daemon_ipc_lifetime_reservation_try_acquire(
        endpoint, reservation_out);
#endif
}

static void *host_watcher_thread(void *opaque) {
    cbm_watcher_run(opaque, HOST_WATCH_INTERVAL_MS);
    return NULL;
}

static void *host_http_thread(void *opaque) {
    cbm_http_server_run(opaque);
    return NULL;
}

static int host_watcher_index(const char *project_name, const char *root_path,
                              void *opaque) {
    host_state_t *host = opaque;
    return host && host->application
               ? cbm_daemon_application_watcher_index(project_name, root_path,
                                                       host->application)
               : -1;
}

static int host_ui_index(void *opaque, const char *root_path,
                         const char *project_name) {
    host_state_t *host = opaque;
    return host && host->application
               ? cbm_daemon_application_index(host->application,
                                              project_name ? project_name : "",
                                              root_path)
               : -1;
}

static bool host_ui_mutation_begin(void *opaque, const char *project) {
    host_state_t *host = opaque;
    return host && host->application &&
           cbm_daemon_application_project_mutation_try_begin(host->application,
                                                             project);
}

static void host_ui_mutation_end(void *opaque, const char *project) {
    host_state_t *host = opaque;
    if (host && host->application) {
        cbm_daemon_application_project_mutation_end(host->application, project);
    }
}

static void host_http_stop_join_free(host_state_t *host) {
    if (host->http) {
        cbm_http_server_stop(host->http);
    }
    if (host->http_started) {
        (void)cbm_thread_join(&host->http_thread);
        host->http_started = false;
    }
    cbm_http_server_free(host->http);
    host->http = NULL;
}

static void host_http_reconcile(host_state_t *host) {
    cbm_ui_config_t desired;
    cbm_ui_config_load(&desired);
    if (desired.ui_enabled == host->http_config_enabled &&
        desired.ui_port == host->http_config_port) {
        return;
    }
    host_http_stop_join_free(host);
    host->http_config_enabled = desired.ui_enabled;
    host->http_config_port = desired.ui_port;
    if (!desired.ui_enabled) {
        return;
    }
    if (CBM_EMBEDDED_FILE_COUNT == 0) {
        cbm_log_warn("ui.no_assets", "hint",
                     "rebuild with: make -f Makefile.cbm cbm-with-ui");
        return;
    }
    host->http = cbm_http_server_new(desired.ui_port);
    if (!host->http) {
        return;
    }
    cbm_http_server_set_watcher(host->http, host->watcher);
    cbm_http_server_set_index_executor(host->http, host_ui_index, host);
    cbm_http_server_set_project_mutation_guard(
        host->http, host_ui_mutation_begin, host_ui_mutation_end, host);
    if (cbm_thread_create(&host->http_thread, 0, host_http_thread,
                          host->http) == 0) {
        host->http_started = true;
    } else {
        cbm_http_server_free(host->http);
        host->http = NULL;
    }
}

static void host_background_stop(host_state_t *host) {
    if (host->http) {
        cbm_http_server_stop(host->http);
    }
    if (host->watcher) {
        cbm_watcher_stop(host->watcher);
    }
}

static void host_background_join(host_state_t *host) {
    if (host->http_started) {
        (void)cbm_thread_join(&host->http_thread);
        host->http_started = false;
    }
    if (host->watcher_started) {
        (void)cbm_thread_join(&host->watcher_thread);
        host->watcher_started = false;
    }
}

static bool host_project_lock_manager_free_once(void *context) {
    cbm_project_lock_manager_t **manager = context;
    if (!manager || !*manager) {
        return true;
    }
    (void)cbm_project_lock_manager_free(manager);
    return *manager == NULL;
}

static void host_state_free(host_state_t *host) {
    host_http_stop_join_free(host);
    cbm_daemon_application_free(host->application);
    host->application = NULL;
    cbm_watcher_free(host->watcher);
    host->watcher = NULL;
    cbm_store_close(host->watch_store);
    host->watch_store = NULL;
    cbm_config_close(host->runtime_config);
    host->runtime_config = NULL;
    if (host->project_locks) {
        host_cleanup_release_until_complete(
            host_project_lock_manager_free_once, &host->project_locks,
            "project_lock_manager_cleanup");
    }
}

static bool host_state_prepare(host_state_t *host,
                               const cbm_daemon_ipc_endpoint_t *endpoint) {
    size_t aggregate_memory_budget_bytes = cbm_mem_budget();
    const char *cache = cbm_resolve_cache_dir();
    host->runtime_config = cache ? cbm_config_open(cache) : NULL;
    host->watch_store = cbm_store_open_memory();
    host->project_locks = cbm_project_lock_manager_new(endpoint);
    host->watcher = cbm_watcher_new(host->watch_store, host_watcher_index, host);
    cbm_daemon_application_config_t application_config = {
        .watcher = host->watcher,
        .config = host->runtime_config,
        .aggregate_memory_budget_bytes = aggregate_memory_budget_bytes,
        .project_locks = host->project_locks,
    };
    host->application = cbm_daemon_application_new(&application_config);
    if (!host->watch_store || !host->watcher || !host->project_locks ||
        !host->application) {
        return false;
    }
    return true;
}

static bool host_background_start(host_state_t *host) {
    if (cbm_thread_create(&host->watcher_thread, 0, host_watcher_thread,
                          host->watcher) != 0) {
        return false;
    }
    host->watcher_started = true;

    /* Force the first reconciliation even when defaults are false/9749. */
    host->http_config_port = -1;
    host_http_reconcile(host);
    return true;
}

static bool host_wait_for_lifetime(cbm_daemon_runtime_service_t *service,
                                   atomic_int *stop_requested,
                                   host_state_t *host) {
    uint64_t initial_deadline = cbm_now_ms() + HOST_INITIAL_CLIENT_TIMEOUT_MS;
    bool admitted = false;
    uint64_t stopping_deadline = 0;
    for (;;) {
        cbm_daemon_runtime_service_state_t state =
            cbm_daemon_runtime_service_state(service);
        if (state == CBM_DAEMON_RUNTIME_SERVICE_EXITED) {
            return true;
        }
        if (state == CBM_DAEMON_RUNTIME_SERVICE_STOPPING) {
            if (stopping_deadline == 0) {
                stopping_deadline =
                    cbm_now_ms() + HOST_RUNTIME_SHUTDOWN_MS;
            }
            if (cbm_now_ms() >= stopping_deadline) {
                return false;
            }
            (void)cbm_daemon_runtime_service_wait_exited(
                service, HOST_WAIT_TICK_MS);
            continue;
        }
        if (cbm_daemon_runtime_service_active_clients(service) > 0) {
            admitted = true;
        }
        bool stop = stop_requested && atomic_load(stop_requested);
        if (stop || (!admitted && cbm_now_ms() >= initial_deadline)) {
            return cbm_daemon_runtime_service_stop(
                service, HOST_RUNTIME_SHUTDOWN_MS);
        }
        host_http_reconcile(host);
        (void)cbm_daemon_runtime_service_wait_exited(service,
                                                     HOST_WAIT_TICK_MS);
    }
}

static bool host_application_shutdown(host_state_t *host) {
    if (cbm_daemon_application_shutdown(host->application,
                                        HOST_APPLICATION_SHUTDOWN_MS)) {
        return true;
    }
    cbm_log_error("daemon.shutdown_timeout", "component", "operations");
    return false;
}

static bool host_runtime_stop_free(cbm_daemon_runtime_service_t *service) {
    if (cbm_daemon_runtime_service_state(service) !=
        CBM_DAEMON_RUNTIME_SERVICE_EXITED) {
        if (!cbm_daemon_runtime_service_stop(
                service, HOST_RUNTIME_SHUTDOWN_MS)) {
            cbm_log_error("daemon.shutdown_timeout", "component", "runtime");
            return false;
        }
    }
    if (!cbm_daemon_runtime_service_free(service)) {
        cbm_log_error("daemon.cleanup_timeout", "component", "runtime");
        return false;
    }
    return true;
}

static _Noreturn void host_force_terminate(const char *component) {
    /* The operation-log sink flushes every record before returning. Do not run
     * ordinary teardown after the cooperative deadline: a callback thread may
     * still own application storage. Process termination releases native
     * locks, and supervised worker groups observe their daemon parent's death. */
    cbm_log_error("daemon.forced_shutdown", "component", component);
#ifdef _WIN32
    (void)TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
    abort();
#else
    _exit(EXIT_FAILURE);
#endif
}

_Noreturn void cbm_daemon_host_force_terminate_for_test(
    const char *component) {
    char conflict_log[HOST_PATH_CAP];
    cbm_ui_log_init();
    if (host_log_open(conflict_log)) {
        host_force_terminate(component ? component : "test");
    }
#ifdef _WIN32
    (void)TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
    abort();
#else
    _exit(EXIT_FAILURE);
#endif
}

int cbm_daemon_host_run(const cbm_daemon_host_config_t *config) {
    if (!config || !config->endpoint || !config->executable_path ||
        !config->identity.semantic_version ||
        !config->identity.build_fingerprint) {
        return -1;
    }
    cbm_version_cohort_manager_t *cohort_manager =
        cbm_version_cohort_manager_new(config->endpoint);
    cbm_version_cohort_lease_t *cohort_lease = NULL;
    cbm_daemon_conflict_t cohort_conflict;
    uint64_t now_ms = cbm_now_ms();
    uint64_t cohort_deadline =
        now_ms > UINT64_MAX - HOST_INITIAL_CLIENT_TIMEOUT_MS
            ? UINT64_MAX
            : now_ms + HOST_INITIAL_CLIENT_TIMEOUT_MS;
    cbm_version_cohort_status_t cohort_status =
        cohort_manager
            ? cbm_version_cohort_acquire(
                  cohort_manager, &config->identity, cohort_deadline,
                  &cohort_lease, &cohort_conflict)
            : CBM_VERSION_COHORT_IO;
    if (cohort_status != CBM_VERSION_COHORT_OK) {
        char message[CBM_DAEMON_CONFLICT_MESSAGE_SIZE];
        bool formatted = cohort_status == CBM_VERSION_COHORT_CONFLICT &&
                         cbm_daemon_conflict_format(
                             &cohort_conflict, message, sizeof(message));
        if (cohort_status == CBM_VERSION_COHORT_CONFLICT) {
            (void)cbm_version_cohort_log_conflict(&cohort_conflict);
        }
        (void)fprintf(stderr, "codebase-memory: %s\n",
                      formatted ? message
                                : "daemon exact-build admission failed");
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_daemon_ipc_participant_guard_t *participant_guard = NULL;
    if (cbm_daemon_ipc_participant_guard_try_join(
            config->endpoint, &participant_guard) != 1) {
        (void)fprintf(stderr,
                      "codebase-memory: daemon participant admission failed\n");
        host_participant_guard_close(&participant_guard);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_daemon_ipc_lifetime_reservation_t *lifetime_reservation = NULL;
    if (host_lifetime_reservation_acquire(
            config->endpoint, &lifetime_reservation) != 1) {
        host_participant_guard_close(&participant_guard);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_version_cohort_daemon_claim_t *daemon_claim = NULL;
    if (cbm_version_cohort_daemon_claim_acquire(
            cohort_manager, &daemon_claim) != CBM_VERSION_COHORT_OK) {
        cbm_daemon_ipc_lifetime_reservation_release(lifetime_reservation);
        host_daemon_claim_close(&daemon_claim);
        host_participant_guard_close(&participant_guard);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_mem_init(cbm_mem_ram_fraction_for_total(cbm_system_info().total_ram));
    cbm_ui_log_init();
    char conflict_log[HOST_PATH_CAP];
    if (!host_log_open(conflict_log)) {
        (void)fprintf(stderr,
                      "codebase-memory: daemon log path is not private or safe\n");
        cbm_daemon_ipc_lifetime_reservation_release(lifetime_reservation);
        host_daemon_claim_close(&daemon_claim);
        host_participant_guard_close(&participant_guard);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_http_server_set_binary_path(config->executable_path);
    cbm_index_supervisor_mark_host();

    host_state_t host = {0};
    if (!host_state_prepare(&host, config->endpoint)) {
        cbm_log_error("daemon.start_failed", "component", "application");
        host_state_free(&host);
        host_log_close();
        cbm_daemon_ipc_lifetime_reservation_release(lifetime_reservation);
        host_daemon_claim_close(&daemon_claim);
        host_participant_guard_close(&participant_guard);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(host.application);
    cbm_daemon_runtime_service_config_t runtime_config = {
        .endpoint = config->endpoint,
        .identity = config->identity,
        .conflict_log_path = conflict_log,
        .conflict_log_cap_bytes = HOST_CONFLICT_LOG_CAP,
        .max_clients = HOST_MAX_CLIENTS,
        .lease_timeout_ms = HOST_LEASE_TIMEOUT_MS,
        .request_timeout_ms = HOST_REQUEST_TIMEOUT_MS,
        .shutdown_timeout_ms = HOST_RUNTIME_SHUTDOWN_MS,
        .application = callbacks,
    };
    cbm_daemon_runtime_service_t *service =
        cbm_daemon_runtime_service_start_reserved(
            &runtime_config, &lifetime_reservation);
    cbm_daemon_ipc_lifetime_reservation_release(lifetime_reservation);
    lifetime_reservation = NULL;
    if (!service) {
        cbm_log_error("daemon.start_failed", "component", "runtime");
        host_state_free(&host);
        host_log_close();
        host_daemon_claim_close(&daemon_claim);
        host_participant_guard_close(&participant_guard);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    if (!host_background_start(&host)) {
        cbm_log_error("daemon.start_failed", "component", "background");
        if (!host_application_shutdown(&host)) {
            host_force_terminate("operations");
        }
        host_background_stop(&host);
        host_background_join(&host);
        if (!host_runtime_stop_free(service)) {
            host_force_terminate("runtime");
        }
        host_state_free(&host);
        host_log_close();
        host_daemon_claim_close(&daemon_claim);
        host_participant_guard_close(&participant_guard);
        host_cohort_close(&cohort_lease, &cohort_manager);
        return -1;
    }
    cbm_diag_start();

    cbm_log_info("daemon.start", "version", config->identity.semantic_version);
    if (!host_wait_for_lifetime(service, config->stop_requested, &host)) {
        host_force_terminate("runtime");
    }

    /* Prevent any new UI/watcher operation, then cancel/reap every physical
     * job before those background loops and the daemon process disappear. */
    if (!host_application_shutdown(&host)) {
        host_force_terminate("operations");
    }
    host_background_stop(&host);
    host_background_join(&host);
    if (!host_runtime_stop_free(service)) {
        host_force_terminate("runtime");
    }
    host_state_free(&host);
    cbm_diag_stop();
    cbm_log_info("daemon.stop");
    host_log_close();
    /* Keep the exact-build lifetime lease through listener/lifetime teardown,
     * application destruction, diagnostics shutdown, the durable stop record,
     * daemon-claim release, and participant release. An activation transaction
     * may treat exclusive acquisition as proof that coordinated cleanup for
     * this generation has completed. */
    host_daemon_claim_close(&daemon_claim);
    host_participant_guard_close(&participant_guard);
    host_cohort_close(&cohort_lease, &cohort_manager);
    return 0;
}
