/*
 * test_daemon_frontend.c — Exact JSON-RPC cancellation and maintenance.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "daemon/frontend.h"
#include "daemon/ipc.h"
#include "daemon/service.h"
#include "daemon/version_cohort.h"
#include "foundation/compat.h"
#include "foundation/platform.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

enum { FRONTEND_TEST_PATH_CAP = 1024 };

typedef struct {
    char parent[FRONTEND_TEST_PATH_CAP];
    cbm_daemon_ipc_endpoint_t *endpoint;
    cbm_version_cohort_manager_t *manager;
} frontend_maintenance_fixture_t;

static bool frontend_maintenance_fixture_start(
    frontend_maintenance_fixture_t *fixture, const char *tag) {
    memset(fixture, 0, sizeof(*fixture));
    int written = snprintf(fixture->parent, sizeof(fixture->parent),
                           "%s/cbm-frontend-%s-XXXXXX", cbm_tmpdir(), tag);
    if (written <= 0 || written >= (int)sizeof(fixture->parent) ||
        !cbm_mkdtemp(fixture->parent)) {
        return false;
    }
    fixture->endpoint =
        cbm_daemon_ipc_endpoint_new("0123456789abcdef", fixture->parent);
    fixture->manager = fixture->endpoint
                           ? cbm_version_cohort_manager_new(fixture->endpoint)
                           : NULL;
    return fixture->endpoint && fixture->manager;
}

static void frontend_maintenance_fixture_finish(
    frontend_maintenance_fixture_t *fixture) {
    while (fixture->manager &&
           cbm_version_cohort_manager_free(&fixture->manager) !=
               CBM_PRIVATE_FILE_LOCK_OK) {
        cbm_usleep(1000);
    }
    cbm_daemon_ipc_endpoint_free(fixture->endpoint);
    if (fixture->parent[0]) {
        (void)th_rmtree(fixture->parent);
    }
    memset(fixture, 0, sizeof(*fixture));
}

#ifndef _WIN32
static const char FRONTEND_TEST_BUILD[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static cbm_daemon_build_identity_t frontend_test_identity(void) {
    cbm_daemon_build_identity_t identity = {
        .semantic_version = "2.4.0",
        .build_fingerprint = FRONTEND_TEST_BUILD,
        .protocol_abi = 3,
        .store_abi = 11,
        .feature_abi = 7,
    };
    return identity;
}

static void frontend_test_release_lease(
    cbm_version_cohort_lease_t **lease) {
    while (lease && *lease &&
           cbm_version_cohort_lease_release(lease) !=
               CBM_PRIVATE_FILE_LOCK_OK) {
        cbm_usleep(1000);
    }
}

static bool frontend_test_wait_byte(int fd, char expected,
                                    uint64_t deadline_ms) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }
    for (;;) {
        char value = '\0';
        ssize_t count = read(fd, &value, 1);
        if (count == 1) {
            return value == expected;
        }
        if (count == 0 ||
            (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ||
            cbm_now_ms() >= deadline_ms) {
            return false;
        }
        cbm_usleep(1000);
    }
}

static cbm_version_cohort_quiesce_result_t
frontend_test_quiesce_requested(void *context) {
    (void)context;
    return CBM_VERSION_COHORT_QUIESCE_REQUESTED;
}

static bool frontend_test_cancel_active(void *context) {
    int fd = *(int *)context;
    const char marker = 'C';
    return write(fd, &marker, 1) == 1;
}

static bool frontend_test_cancel_and_mark(void *context) {
    atomic_bool *cancelled = context;
    atomic_store_explicit(cancelled, true, memory_order_release);
    return true;
}
#endif

TEST(daemon_frontend_recognizes_exact_cancellation_notification) {
    ASSERT_TRUE(cbm_daemon_frontend_is_cancellation_notification(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
        "\"params\":{\"requestId\":7}}"));
    ASSERT_TRUE(cbm_daemon_frontend_is_cancellation_notification(
        "{ \"params\": {}, \"method\": \"notifications/cancelled\", "
        "\"jsonrpc\": \"2.0\" }"));
    PASS();
}

/* RED on the whole-session cancellation shortcut: merely recognizing the
 * notification method is not authority to close a session.  The requestId
 * must match the exact numeric/string identity currently being executed. */
TEST(daemon_frontend_correlates_cancellation_to_exact_request) {
    ASSERT_TRUE(cbm_daemon_frontend_cancellation_matches_request(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
        "\"params\":{\"requestId\":7}}",
        7, NULL));
    ASSERT_FALSE(cbm_daemon_frontend_cancellation_matches_request(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
        "\"params\":{\"requestId\":8}}",
        7, NULL));
    ASSERT_TRUE(cbm_daemon_frontend_cancellation_matches_request(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
        "\"params\":{\"requestId\":\"request-7\"}}",
        -1, "request-7"));
    ASSERT_FALSE(cbm_daemon_frontend_cancellation_matches_request(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
        "\"params\":{\"requestId\":7}}",
        -1, "7"));
    ASSERT_FALSE(cbm_daemon_frontend_cancellation_matches_request(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
        "\"params\":{}}",
        7, NULL));
    ASSERT_FALSE(cbm_daemon_frontend_cancellation_matches_request(
        "{\"jsonrpc\":\"2.0\",\"id\":9,"
        "\"method\":\"notifications/cancelled\","
        "\"params\":{\"requestId\":7}}",
        7, NULL));
    PASS();
}

TEST(daemon_frontend_ignores_cancellation_text_in_string_content) {
    ASSERT_FALSE(cbm_daemon_frontend_is_cancellation_notification(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"search_graph\",\"arguments\":{\"query\":"
        "\"notifications/cancelled\"}}}"));
    ASSERT_FALSE(cbm_daemon_frontend_is_cancellation_notification(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
        "\"params\":{\"method\":\"notifications/cancelled\"}}"));
    PASS();
}

TEST(daemon_frontend_rejects_non_notification_cancellation_shapes) {
    ASSERT_FALSE(cbm_daemon_frontend_is_cancellation_notification(
        "{\"jsonrpc\":\"2.0\",\"id\":3,"
        "\"method\":\"notifications/cancelled\"}"));
    ASSERT_FALSE(cbm_daemon_frontend_is_cancellation_notification(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled-extra\"}"));
    ASSERT_FALSE(cbm_daemon_frontend_is_cancellation_notification(
        "{\"jsonrpc\":\"2.0\",\"method\":\"prefix/notifications/cancelled\"}"));
    ASSERT_FALSE(cbm_daemon_frontend_is_cancellation_notification(
        "{\"jsonrpc\":\"2.0\",\"params\":{\"method\":"
        "\"notifications/cancelled\"}}"));
    ASSERT_FALSE(cbm_daemon_frontend_is_cancellation_notification("not-json"));
    ASSERT_FALSE(cbm_daemon_frontend_is_cancellation_notification(NULL));
    PASS();
}

#ifndef _WIN32
/* RED: an MCP frontend's main thread can remain blocked forever in stdio while
 * install/update/uninstall owns maintenance intent. The already-present
 * frontend worker must observe that native intent and terminate this stateless
 * child, releasing its cohort lease and kernel IPC ownership. A deliberately
 * invalid client pointer is safe only if the maintenance path exits before
 * ordinary EOF/session-close handling, which also pins that exact path. */
TEST(daemon_frontend_maintenance_exits_while_stdio_reader_is_blocked) {
    frontend_maintenance_fixture_t fixture;
    ASSERT_TRUE(frontend_maintenance_fixture_start(&fixture, "blocked-stdio"));
    int input_pipe[2] = {-1, -1};
    int ready_pipe[2] = {-1, -1};
    bool pipes_ready = pipe(input_pipe) == 0 && pipe(ready_pipe) == 0;
    pid_t child = pipes_ready ? fork() : -1;
    if (child == 0) {
        (void)close(input_pipe[1]);
        (void)close(ready_pipe[0]);
        cbm_daemon_ipc_endpoint_t *endpoint =
            cbm_daemon_ipc_endpoint_new("0123456789abcdef", fixture.parent);
        cbm_version_cohort_manager_t *manager =
            endpoint ? cbm_version_cohort_manager_new(endpoint) : NULL;
        cbm_version_cohort_lease_t *participant = NULL;
        cbm_daemon_conflict_t conflict;
        cbm_daemon_build_identity_t identity = frontend_test_identity();
        cbm_version_cohort_status_t admitted =
            manager ? cbm_version_cohort_acquire(
                          manager, &identity, cbm_now_ms() + 2000U,
                          &participant, &conflict)
                    : CBM_VERSION_COHORT_IO;
        FILE *input = admitted == CBM_VERSION_COHORT_OK
                          ? fdopen(input_pipe[0], "rb")
                          : NULL;
        FILE *output = input ? tmpfile() : NULL;
        const char ready = 'R';
        bool announced = output && write(ready_pipe[1], &ready, 1) == 1;
        (void)close(ready_pipe[1]);
        if (!announced) {
            _exit(70);
        }
        int result = cbm_daemon_frontend_mcp_run(
            (cbm_daemon_runtime_client_t *)(uintptr_t)1, manager, input,
            output);
        (void)result;
        _exit(71);
    }

    if (pipes_ready) {
        (void)close(input_pipe[0]);
        (void)close(ready_pipe[1]);
    }
    bool announced = child > 0 && frontend_test_wait_byte(
                                     ready_pipe[0], 'R', cbm_now_ms() + 2000U);
    cbm_version_cohort_lease_t *mutation = NULL;
    cbm_version_cohort_quiesce_result_t quiesce =
        CBM_VERSION_COHORT_QUIESCE_NOT_NEEDED;
    cbm_version_cohort_status_t status =
        announced ? cbm_version_cohort_reserve_for_mutation(
                        fixture.manager, cbm_now_ms() + 3000U,
                        frontend_test_quiesce_requested, NULL, &quiesce,
                        &mutation)
                  : CBM_VERSION_COHORT_IO;
    if (status != CBM_VERSION_COHORT_OK && child > 0) {
        (void)kill(child, SIGKILL);
    }
    int child_status = 0;
    bool waited = child > 0 && waitpid(child, &child_status, 0) == child;
    if (pipes_ready) {
        (void)close(input_pipe[1]);
        (void)close(ready_pipe[0]);
    }
    frontend_test_release_lease(&mutation);
    frontend_maintenance_fixture_finish(&fixture);

    ASSERT_TRUE(pipes_ready);
    ASSERT_TRUE(child > 0);
    ASSERT_TRUE(announced);
    ASSERT_EQ(status, CBM_VERSION_COHORT_OK);
    ASSERT_EQ(quiesce, CBM_VERSION_COHORT_QUIESCE_REQUESTED);
    ASSERT_TRUE(waited);
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 0);
    PASS();
}

/* RED: local CLI/worker work has no standing daemon session to interrupt. Its
 * command-lifetime monitor must request cooperative cancellation once, allow a
 * bounded grace, then hard-exit the isolated process so SQLite/native locks
 * roll back and the mutation barrier can prove that every lifetime participant
 * is gone. */
TEST(daemon_local_participant_monitor_cancels_then_bounds_active_operation) {
    frontend_maintenance_fixture_t fixture;
    ASSERT_TRUE(frontend_maintenance_fixture_start(&fixture, "local-monitor"));
    int ready_pipe[2] = {-1, -1};
    int cancel_pipe[2] = {-1, -1};
    bool pipes_ready = pipe(ready_pipe) == 0 && pipe(cancel_pipe) == 0;
    pid_t child = pipes_ready ? fork() : -1;
    if (child == 0) {
        (void)close(ready_pipe[0]);
        (void)close(cancel_pipe[0]);
        cbm_daemon_ipc_endpoint_t *endpoint =
            cbm_daemon_ipc_endpoint_new("0123456789abcdef", fixture.parent);
        cbm_version_cohort_manager_t *manager =
            endpoint ? cbm_version_cohort_manager_new(endpoint) : NULL;
        cbm_version_cohort_lease_t *participant = NULL;
        cbm_daemon_conflict_t conflict;
        cbm_daemon_build_identity_t identity = frontend_test_identity();
        cbm_version_cohort_status_t admitted =
            manager ? cbm_version_cohort_acquire(
                          manager, &identity, cbm_now_ms() + 2000U,
                          &participant, &conflict)
                    : CBM_VERSION_COHORT_IO;
        cbm_daemon_maintenance_monitor_t *monitor =
            admitted == CBM_VERSION_COHORT_OK
                ? cbm_daemon_maintenance_monitor_start(
                      manager, frontend_test_cancel_active, &cancel_pipe[1],
                      37, "test-local-operation")
                : NULL;
        const char ready = 'R';
        bool announced = monitor && write(ready_pipe[1], &ready, 1) == 1;
        (void)close(ready_pipe[1]);
        if (!announced) {
            _exit(72);
        }
        for (;;) {
            cbm_usleep(100000);
        }
    }

    if (pipes_ready) {
        (void)close(ready_pipe[1]);
        (void)close(cancel_pipe[1]);
    }
    bool announced = child > 0 && frontend_test_wait_byte(
                                     ready_pipe[0], 'R', cbm_now_ms() + 2000U);
    cbm_version_cohort_lease_t *mutation = NULL;
    cbm_version_cohort_quiesce_result_t quiesce =
        CBM_VERSION_COHORT_QUIESCE_NOT_NEEDED;
    cbm_version_cohort_status_t status =
        announced ? cbm_version_cohort_reserve_for_mutation(
                        fixture.manager, cbm_now_ms() + 5000U,
                        frontend_test_quiesce_requested, NULL, &quiesce,
                        &mutation)
                  : CBM_VERSION_COHORT_IO;
    bool cancelled = status == CBM_VERSION_COHORT_OK &&
                     frontend_test_wait_byte(cancel_pipe[0], 'C',
                                             cbm_now_ms() + 1000U);
    if (status != CBM_VERSION_COHORT_OK && child > 0) {
        (void)kill(child, SIGKILL);
    }
    int child_status = 0;
    bool waited = child > 0 && waitpid(child, &child_status, 0) == child;
    if (pipes_ready) {
        (void)close(ready_pipe[0]);
        (void)close(cancel_pipe[0]);
    }
    frontend_test_release_lease(&mutation);
    frontend_maintenance_fixture_finish(&fixture);

    ASSERT_TRUE(pipes_ready);
    ASSERT_TRUE(child > 0);
    ASSERT_TRUE(announced);
    ASSERT_EQ(status, CBM_VERSION_COHORT_OK);
    ASSERT_EQ(quiesce, CBM_VERSION_COHORT_QUIESCE_REQUESTED);
    ASSERT_TRUE(cancelled);
    ASSERT_TRUE(waited);
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 37);
    PASS();
}

/* RED: supervised process-tree cancellation permits one second of graceful
 * shutdown followed by one second of forced containment. The maintenance
 * observer must not _Exit the owning process before that bounded supervisor
 * window can finish and join the observer normally. */
TEST(daemon_local_participant_monitor_allows_supervisor_containment_window) {
    frontend_maintenance_fixture_t fixture;
    ASSERT_TRUE(frontend_maintenance_fixture_start(&fixture, "monitor-window"));
    int ready_pipe[2] = {-1, -1};
    bool pipe_ready = pipe(ready_pipe) == 0;
    pid_t child = pipe_ready ? fork() : -1;
    if (child == 0) {
        (void)close(ready_pipe[0]);
        cbm_daemon_ipc_endpoint_t *endpoint =
            cbm_daemon_ipc_endpoint_new("0123456789abcdef", fixture.parent);
        cbm_version_cohort_manager_t *manager =
            endpoint ? cbm_version_cohort_manager_new(endpoint) : NULL;
        cbm_version_cohort_lease_t *participant = NULL;
        cbm_daemon_conflict_t conflict;
        cbm_daemon_build_identity_t identity = frontend_test_identity();
        cbm_version_cohort_status_t admitted =
            manager ? cbm_version_cohort_acquire(
                          manager, &identity, cbm_now_ms() + 2000U,
                          &participant, &conflict)
                    : CBM_VERSION_COHORT_IO;
        atomic_bool cancelled;
        atomic_init(&cancelled, false);
        cbm_daemon_maintenance_monitor_t *monitor =
            admitted == CBM_VERSION_COHORT_OK
                ? cbm_daemon_maintenance_monitor_start(
                      manager, frontend_test_cancel_and_mark, &cancelled,
                      38, "test-supervisor-window")
                : NULL;
        const char ready = 'R';
        bool announced = monitor && write(ready_pipe[1], &ready, 1) == 1;
        (void)close(ready_pipe[1]);
        if (!announced) {
            _exit(72);
        }
        while (!atomic_load_explicit(&cancelled, memory_order_acquire)) {
            cbm_usleep(1000);
        }

        /* Model the maximum graceful + forced-settle supervisor bounds. */
        cbm_usleep(2100U * 1000U);
        bool stopped = cbm_daemon_maintenance_monitor_stop(&monitor);
        frontend_test_release_lease(&participant);
        while (manager &&
               cbm_version_cohort_manager_free(&manager) !=
                   CBM_PRIVATE_FILE_LOCK_OK) {
            cbm_usleep(1000);
        }
        cbm_daemon_ipc_endpoint_free(endpoint);
        _exit(stopped ? 0 : 73);
    }

    if (pipe_ready) {
        (void)close(ready_pipe[1]);
    }
    bool announced = child > 0 && frontend_test_wait_byte(
                                     ready_pipe[0], 'R', cbm_now_ms() + 2000U);
    cbm_version_cohort_lease_t *mutation = NULL;
    cbm_version_cohort_quiesce_result_t quiesce =
        CBM_VERSION_COHORT_QUIESCE_NOT_NEEDED;
    cbm_version_cohort_status_t status =
        announced ? cbm_version_cohort_reserve_for_mutation(
                        fixture.manager, cbm_now_ms() + 5000U,
                        frontend_test_quiesce_requested, NULL, &quiesce,
                        &mutation)
                  : CBM_VERSION_COHORT_IO;
    if (status != CBM_VERSION_COHORT_OK && child > 0) {
        (void)kill(child, SIGKILL);
    }
    int child_status = 0;
    bool waited = child > 0 && waitpid(child, &child_status, 0) == child;
    if (pipe_ready) {
        (void)close(ready_pipe[0]);
    }
    frontend_test_release_lease(&mutation);
    frontend_maintenance_fixture_finish(&fixture);

    ASSERT_TRUE(pipe_ready);
    ASSERT_TRUE(child > 0);
    ASSERT_TRUE(announced);
    ASSERT_EQ(status, CBM_VERSION_COHORT_OK);
    ASSERT_EQ(quiesce, CBM_VERSION_COHORT_QUIESCE_REQUESTED);
    ASSERT_TRUE(waited);
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 0);
    PASS();
}
#endif

TEST(daemon_local_participant_monitor_joins_before_manager_teardown) {
    frontend_maintenance_fixture_t fixture;
    ASSERT_TRUE(frontend_maintenance_fixture_start(&fixture, "monitor-join"));
    cbm_daemon_maintenance_monitor_t *monitor =
        cbm_daemon_maintenance_monitor_start(
            fixture.manager, NULL, NULL, EXIT_FAILURE, "test-idle-command");
    bool started = monitor != NULL;
    bool stopped = started && cbm_daemon_maintenance_monitor_stop(&monitor);
    bool consumed = monitor == NULL;
    frontend_maintenance_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(consumed);
    PASS();
}

SUITE(daemon_frontend) {
    RUN_TEST(daemon_frontend_recognizes_exact_cancellation_notification);
    RUN_TEST(daemon_frontend_correlates_cancellation_to_exact_request);
    RUN_TEST(daemon_frontend_ignores_cancellation_text_in_string_content);
    RUN_TEST(daemon_frontend_rejects_non_notification_cancellation_shapes);
#ifndef _WIN32
    RUN_TEST(daemon_frontend_maintenance_exits_while_stdio_reader_is_blocked);
    RUN_TEST(daemon_local_participant_monitor_cancels_then_bounds_active_operation);
    RUN_TEST(daemon_local_participant_monitor_allows_supervisor_containment_window);
#endif
    RUN_TEST(daemon_local_participant_monitor_joins_before_manager_teardown);
}
