/*
 * test_daemon_application.c — RED contract for daemon-owned frontend state.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "daemon/application.h"
#include "daemon/application_internal.h"
#include "daemon/ipc.h"
#include "daemon/project_lock.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/platform.h"
#include "mcp/mcp.h"
#include "pipeline/pipeline.h"
#include "store/store.h"
#include "ui/config.h"
#include "watcher/watcher.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { APP_TEST_TIMEOUT_MS = 2000, APP_TEST_PATH_CAP = 1024 };

typedef struct {
    char runtime_parent[APP_TEST_PATH_CAP];
    cbm_daemon_ipc_endpoint_t *endpoint;
    cbm_project_lock_manager_t *external_locks;
    cbm_project_lock_manager_t *daemon_locks;
    cbm_project_lock_manager_t *worker_locks;
    cbm_project_lock_manager_t *probe_locks;
} app_project_lock_fixture_t;

static bool app_project_lock_fixture_cleanup(app_project_lock_fixture_t *fixture) {
    if (!fixture) {
        return false;
    }
    bool clean = true;
    if (fixture->daemon_locks &&
        cbm_project_lock_manager_free(&fixture->daemon_locks) !=
            CBM_PRIVATE_FILE_LOCK_OK) {
        clean = false;
    }
    if (fixture->external_locks &&
        cbm_project_lock_manager_free(&fixture->external_locks) !=
            CBM_PRIVATE_FILE_LOCK_OK) {
        clean = false;
    }
    if (fixture->worker_locks &&
        cbm_project_lock_manager_free(&fixture->worker_locks) !=
            CBM_PRIVATE_FILE_LOCK_OK) {
        clean = false;
    }
    if (fixture->probe_locks &&
        cbm_project_lock_manager_free(&fixture->probe_locks) !=
            CBM_PRIVATE_FILE_LOCK_OK) {
        clean = false;
    }
    if (!fixture->daemon_locks && !fixture->external_locks &&
        !fixture->worker_locks && !fixture->probe_locks) {
        cbm_daemon_ipc_endpoint_free(fixture->endpoint);
        fixture->endpoint = NULL;
        if (fixture->runtime_parent[0] && th_rmtree(fixture->runtime_parent) != 0) {
            clean = false;
        }
    } else {
        clean = false;
    }
    return clean;
}

static bool app_project_lock_fixture_init(app_project_lock_fixture_t *fixture) {
    if (!fixture) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    (void)snprintf(fixture->runtime_parent, sizeof(fixture->runtime_parent),
                   "%s/cbm-app-project-lock-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(fixture->runtime_parent)) {
        return false;
    }
    fixture->endpoint =
        cbm_daemon_ipc_endpoint_new("fedcba9876543210", fixture->runtime_parent);
    if (fixture->endpoint) {
        fixture->external_locks = cbm_project_lock_manager_new(fixture->endpoint);
        fixture->daemon_locks = cbm_project_lock_manager_new(fixture->endpoint);
        fixture->worker_locks = cbm_project_lock_manager_new(fixture->endpoint);
        fixture->probe_locks = cbm_project_lock_manager_new(fixture->endpoint);
    }
    if (fixture->endpoint && fixture->external_locks && fixture->daemon_locks &&
        fixture->worker_locks && fixture->probe_locks) {
        return true;
    }
    (void)app_project_lock_fixture_cleanup(fixture);
    return false;
}

static bool app_project_lock_release(cbm_project_lock_lease_t **lease) {
    if (!lease) {
        return false;
    }
    uint64_t deadline = cbm_now_ms() + APP_TEST_TIMEOUT_MS;
    while (*lease && cbm_now_ms() < deadline) {
        (void)cbm_project_lock_lease_release(lease);
        if (*lease) {
            cbm_usleep(1000);
        }
    }
    return *lease == NULL;
}

static cbm_daemon_runtime_application_session_t *app_test_open(
    const cbm_daemon_runtime_application_callbacks_t *callbacks, cbm_daemon_client_id_t client_id) {
    return callbacks->session_open(callbacks->context, client_id, 1234);
}

static atomic_uint_fast64_t g_app_test_request_token = ATOMIC_VAR_INIT(1);

static cbm_daemon_runtime_application_status_t app_test_request_tagged(
    const cbm_daemon_runtime_application_callbacks_t *callbacks,
    cbm_daemon_runtime_application_session_t *session,
    cbm_daemon_runtime_application_token_t request_token,
    const uint8_t *request, uint32_t request_length, uint8_t **response_out,
    uint32_t *response_length_out) {
    return callbacks->request(callbacks->context, session, request_token,
                              request, request_length, response_out,
                              response_length_out);
}

static cbm_daemon_runtime_application_status_t app_test_request(
    const cbm_daemon_runtime_application_callbacks_t *callbacks,
    cbm_daemon_runtime_application_session_t *session, const uint8_t *request,
    uint32_t request_length, uint8_t **response_out, uint32_t *response_length_out) {
    cbm_daemon_runtime_application_token_t request_token =
        atomic_fetch_add_explicit(&g_app_test_request_token, 1,
                                  memory_order_relaxed);
    return app_test_request_tagged(callbacks, session, request_token, request,
                                   request_length, response_out,
                                   response_length_out);
}

static bool app_test_context_request_options(
    const char *root, const char *allowed,
    cbm_mcp_tool_profile_t tool_profile, const char *hook_event,
    const char *hook_dialect, uint8_t **request_out, uint32_t *length_out) {
    size_t root_length = strlen(root);
    size_t allowed_length = allowed ? strlen(allowed) : 0;
    size_t event_length = hook_event ? strlen(hook_event) : 0;
    size_t dialect_length = hook_dialect ? strlen(hook_dialect) : 0;
    size_t total = 19U + root_length + allowed_length + event_length +
                   dialect_length;
    if (root_length == 0 || root_length > UINT32_MAX ||
        allowed_length > UINT32_MAX || event_length > UINT32_MAX ||
        dialect_length > UINT32_MAX || total > UINT32_MAX) {
        return false;
    }
    uint8_t *request = calloc(1, total);
    if (!request) {
        return false;
    }
    request[0] = CBM_DAEMON_APPLICATION_REQUEST_SET_CONTEXT;
    request[1] = (uint8_t)(root_length >> 24);
    request[2] = (uint8_t)(root_length >> 16);
    request[3] = (uint8_t)(root_length >> 8);
    request[4] = (uint8_t)root_length;
    request[5] = allowed ? 1U : 0U;
    request[6] = (uint8_t)(allowed_length >> 24);
    request[7] = (uint8_t)(allowed_length >> 16);
    request[8] = (uint8_t)(allowed_length >> 8);
    request[9] = (uint8_t)allowed_length;
    request[10] = (uint8_t)tool_profile;
    request[11] = (uint8_t)(event_length >> 24);
    request[12] = (uint8_t)(event_length >> 16);
    request[13] = (uint8_t)(event_length >> 8);
    request[14] = (uint8_t)event_length;
    request[15] = (uint8_t)(dialect_length >> 24);
    request[16] = (uint8_t)(dialect_length >> 16);
    request[17] = (uint8_t)(dialect_length >> 8);
    request[18] = (uint8_t)dialect_length;
    memcpy(request + 19, root, root_length);
    if (allowed_length > 0) {
        memcpy(request + 19 + root_length, allowed, allowed_length);
    }
    if (event_length > 0) {
        memcpy(request + 19 + root_length + allowed_length, hook_event,
               event_length);
    }
    if (dialect_length > 0) {
        memcpy(request + 19 + root_length + allowed_length + event_length,
               hook_dialect, dialect_length);
    }
    *request_out = request;
    *length_out = (uint32_t)total;
    return true;
}

static bool app_test_context_request(const char *root, const char *allowed,
                                     uint8_t **request_out,
                                     uint32_t *length_out) {
    return app_test_context_request_options(
        root, allowed, CBM_MCP_TOOL_PROFILE_ALL, NULL, NULL, request_out,
        length_out);
}

static bool app_test_text_request(cbm_daemon_application_request_kind_t kind, const char *text,
                                  uint8_t **request_out, uint32_t *length_out) {
    size_t text_length = strlen(text);
    if (text_length == 0 || text_length >= UINT32_MAX) {
        return false;
    }
    uint8_t *request = malloc(text_length + 1U);
    if (!request) {
        return false;
    }
    request[0] = (uint8_t)kind;
    memcpy(request + 1, text, text_length);
    *request_out = request;
    *length_out = (uint32_t)text_length + 1U;
    return true;
}

static bool app_test_tool_request(const char *tool, const char *args, uint8_t **request_out,
                                  uint32_t *length_out) {
    size_t tool_length = strlen(tool);
    size_t args_length = strlen(args);
    size_t total = 5U + tool_length + args_length;
    if (tool_length == 0 || args_length == 0 || total > UINT32_MAX) {
        return false;
    }
    uint8_t *request = malloc(total);
    if (!request) {
        return false;
    }
    request[0] = CBM_DAEMON_APPLICATION_REQUEST_TOOL;
    request[1] = (uint8_t)(tool_length >> 24);
    request[2] = (uint8_t)(tool_length >> 16);
    request[3] = (uint8_t)(tool_length >> 8);
    request[4] = (uint8_t)tool_length;
    memcpy(request + 5, tool, tool_length);
    memcpy(request + 5 + tool_length, args, args_length);
    *request_out = request;
    *length_out = (uint32_t)total;
    return true;
}

static cbm_daemon_runtime_application_status_t app_test_ui_config_request(
    const cbm_daemon_runtime_application_callbacks_t *callbacks,
    cbm_daemon_runtime_application_session_t *session, uint8_t mask,
    uint8_t enabled, uint32_t port, uint32_t request_length) {
    uint8_t request[7] = {
        CBM_DAEMON_APPLICATION_REQUEST_SET_UI_CONFIG,
        mask,
        enabled,
        (uint8_t)(port >> 24),
        (uint8_t)(port >> 16),
        (uint8_t)(port >> 8),
        (uint8_t)port,
    };
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    cbm_daemon_runtime_application_status_t status =
        app_test_request(callbacks, session, request, request_length,
                         &response, &response_length);
    if (response || response_length != 0) {
        free(response);
        return CBM_DAEMON_RUNTIME_APPLICATION_HANDLER_ERROR;
    }
    return status;
}

TEST(daemon_application_new_session_does_not_retain_initial_store) {
    cbm_daemon_application_t *application = cbm_daemon_application_new(NULL);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session = app_test_open(&callbacks, 300);

    bool retains_store =
        session && cbm_daemon_application_session_retains_store_for_test(session);

    if (session) {
        callbacks.session_close(callbacks.context, session);
    }
    bool stopped =
        application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);

    ASSERT_NOT_NULL(application);
    ASSERT_NOT_NULL(session);
    ASSERT_FALSE(retains_store);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_request_cancel_is_scoped_to_exact_token) {
    char root[APP_TEST_PATH_CAP];
    (void)snprintf(root, sizeof(root),
                   "%s/cbm-app-request-cancel-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    cbm_daemon_application_t *application =
        cbm_daemon_application_new(NULL);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session =
        app_test_open(&callbacks, 301);
    uint8_t *request = NULL;
    uint32_t request_length = 0;
    bool request_created = root_ok &&
                           app_test_context_request(
                               root, root, &request, &request_length);
    cbm_daemon_runtime_application_token_t cancelled_token = UINT64_C(7001);
    cbm_daemon_runtime_application_token_t next_token = UINT64_C(7002);
    uint8_t *cancelled_response = NULL;
    uint32_t cancelled_response_length = 0;
    cbm_daemon_runtime_application_status_t cancelled_status =
        CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    uint8_t *next_response = NULL;
    uint32_t next_response_length = 0;
    cbm_daemon_runtime_application_status_t next_status =
        CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;

    if (session && request_created) {
        callbacks.request_cancel(callbacks.context, session,
                                 cancelled_token);
        cancelled_status = app_test_request_tagged(
            &callbacks, session, cancelled_token, request, request_length,
            &cancelled_response, &cancelled_response_length);
        /* A late duplicate for the completed token must not poison the next
         * unique request on this still-live session. */
        callbacks.request_cancel(callbacks.context, session,
                                 cancelled_token);
        next_status = app_test_request_tagged(
            &callbacks, session, next_token, request, request_length,
            &next_response, &next_response_length);
    }

    free(request);
    free(cancelled_response);
    free(next_response);
    if (session) {
        callbacks.session_close(callbacks.context, session);
    }
    bool stopped = application &&
                   cbm_daemon_application_shutdown(
                       application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    if (root_ok) {
        (void)cbm_rmdir(root);
    }

    ASSERT_TRUE(root_ok);
    ASSERT_NOT_NULL(application);
    ASSERT_NOT_NULL(session);
    ASSERT_TRUE(request_created);
    ASSERT_EQ(cancelled_status, CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED);
    ASSERT_EQ(cancelled_response_length, 0);
    ASSERT_EQ(next_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_EQ(next_response_length, 0);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_ui_config_updates_are_masked_and_serialized) {
    char cache[APP_TEST_PATH_CAP];
    (void)snprintf(cache, sizeof(cache), "%s/cbm-app-ui-config-XXXXXX",
                   cbm_tmpdir());
    bool cache_ok = cbm_mkdtemp(cache) != NULL;
    char *old_cache = getenv("CBM_CACHE_DIR")
                          ? strdup(getenv("CBM_CACHE_DIR"))
                          : NULL;
    bool env_ok = cache_ok && cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0;
    cbm_ui_config_t initial = {.ui_enabled = false, .ui_port = 9749};
    bool initial_saved = env_ok && cbm_ui_config_save(&initial);

    cbm_daemon_application_t *application = cbm_daemon_application_new(NULL);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *enabled_session =
        app_test_open(&callbacks, 301);
    cbm_daemon_runtime_application_session_t *port_session =
        app_test_open(&callbacks, 302);
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *context_response = NULL;
    uint32_t context_response_length = 0;
    bool context_encoded = cache_ok && app_test_context_request(
                                               cache, cache, &context,
                                               &context_length);
    bool contexts_set = context_encoded && enabled_session && port_session &&
                        app_test_request(
                            &callbacks, enabled_session, context,
                            context_length, &context_response,
                            &context_response_length) ==
                            CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(context_response);
    context_response = NULL;
    context_response_length = 0;
    contexts_set = contexts_set &&
                   app_test_request(
                       &callbacks, port_session, context, context_length,
                       &context_response, &context_response_length) ==
                       CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(context_response);

    /* Each request deliberately carries a stale value for the unmasked field.
     * Whole-struct replacement would lose one update; masked daemon-side
     * read-modify-write must preserve both regardless of request order. */
    cbm_daemon_runtime_application_status_t enabled_status =
        app_test_ui_config_request(
            &callbacks, enabled_session,
            CBM_DAEMON_APPLICATION_UI_CONFIG_ENABLED, 1, 0, 7);
    cbm_daemon_runtime_application_status_t port_status =
        app_test_ui_config_request(
            &callbacks, port_session,
            CBM_DAEMON_APPLICATION_UI_CONFIG_PORT, 0, 18432, 7);

    cbm_ui_config_t updated = {0};
    cbm_ui_config_load(&updated);

    if (enabled_session) {
        callbacks.session_close(callbacks.context, enabled_session);
    }
    if (port_session) {
        callbacks.session_close(callbacks.context, port_session);
    }
    bool stopped = application &&
                   cbm_daemon_application_shutdown(application,
                                                   APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    free(context);
    if (old_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", old_cache, 1);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(old_cache);
    (void)th_rmtree(cache);

    ASSERT_TRUE(cache_ok);
    ASSERT_TRUE(env_ok);
    ASSERT_TRUE(initial_saved);
    ASSERT_NOT_NULL(application);
    ASSERT_NOT_NULL(enabled_session);
    ASSERT_NOT_NULL(port_session);
    ASSERT_TRUE(contexts_set);
    ASSERT_EQ(enabled_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_EQ(port_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(updated.ui_enabled);
    ASSERT_EQ(updated.ui_port, 18432);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_ui_config_rejects_noncanonical_frames) {
    char cache[APP_TEST_PATH_CAP];
    (void)snprintf(cache, sizeof(cache), "%s/cbm-app-ui-frame-XXXXXX",
                   cbm_tmpdir());
    bool cache_ok = cbm_mkdtemp(cache) != NULL;
    char *old_cache = getenv("CBM_CACHE_DIR")
                          ? strdup(getenv("CBM_CACHE_DIR"))
                          : NULL;
    bool env_ok = cache_ok && cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0;
    cbm_ui_config_t initial = {.ui_enabled = false, .ui_port = 9749};
    bool initial_saved = env_ok && cbm_ui_config_save(&initial);

    cbm_daemon_application_t *application = cbm_daemon_application_new(NULL);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session =
        app_test_open(&callbacks, 303);
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *context_response = NULL;
    uint32_t context_response_length = 0;
    bool context_encoded = cache_ok && app_test_context_request(
                                               cache, cache, &context,
                                               &context_length);
    bool context_set = context_encoded && session &&
                       app_test_request(
                           &callbacks, session, context, context_length,
                           &context_response, &context_response_length) ==
                           CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(context_response);

    cbm_daemon_runtime_application_status_t short_frame =
        app_test_ui_config_request(
            &callbacks, session,
            CBM_DAEMON_APPLICATION_UI_CONFIG_ENABLED, 1, 0, 6);
    cbm_daemon_runtime_application_status_t empty_mask =
        app_test_ui_config_request(&callbacks, session, 0, 0, 0, 7);
    cbm_daemon_runtime_application_status_t unknown_mask =
        app_test_ui_config_request(&callbacks, session, 0x80, 0, 0, 7);
    cbm_daemon_runtime_application_status_t invalid_boolean =
        app_test_ui_config_request(
            &callbacks, session,
            CBM_DAEMON_APPLICATION_UI_CONFIG_ENABLED, 2, 0, 7);
    cbm_daemon_runtime_application_status_t invalid_port =
        app_test_ui_config_request(
            &callbacks, session,
            CBM_DAEMON_APPLICATION_UI_CONFIG_PORT, 0, 65536, 7);
    cbm_daemon_runtime_application_status_t nonzero_unused =
        app_test_ui_config_request(
            &callbacks, session,
            CBM_DAEMON_APPLICATION_UI_CONFIG_ENABLED, 1, 9749, 7);

    cbm_ui_config_t unchanged = {0};
    cbm_ui_config_load(&unchanged);
    if (session) {
        callbacks.session_close(callbacks.context, session);
    }
    bool stopped = application &&
                   cbm_daemon_application_shutdown(application,
                                                   APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    free(context);
    if (old_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", old_cache, 1);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(old_cache);
    (void)th_rmtree(cache);

    ASSERT_TRUE(cache_ok);
    ASSERT_TRUE(env_ok);
    ASSERT_TRUE(initial_saved);
    ASSERT_NOT_NULL(application);
    ASSERT_NOT_NULL(session);
    ASSERT_TRUE(context_set);
    ASSERT_EQ(short_frame, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_EQ(empty_mask, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_EQ(unknown_mask, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_EQ(invalid_boolean, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_EQ(invalid_port, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_EQ(nonzero_unused, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_FALSE(unchanged.ui_enabled);
    ASSERT_EQ(unchanged.ui_port, 9749);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_requires_immutable_explicit_context) {
    cbm_daemon_application_t *application = cbm_daemon_application_new(NULL);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session = app_test_open(&callbacks, 1);
    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-context-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    uint8_t *mcp = NULL;
    uint32_t mcp_length = 0;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    bool mcp_ok = app_test_text_request(CBM_DAEMON_APPLICATION_REQUEST_MCP,
                                        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}", &mcp,
                                        &mcp_length);
    cbm_daemon_runtime_application_status_t before =
        app_test_request(&callbacks, session, mcp, mcp_length, &response, &response_length);
    free(response);
    response = NULL;
    bool context_ok = root_ok && app_test_context_request(root, root, &context, &context_length);
    cbm_daemon_runtime_application_status_t first =
        context_ok ? app_test_request(&callbacks, session, context, context_length, &response,
                                      &response_length)
                   : CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    free(response);
    response = NULL;
    cbm_daemon_runtime_application_status_t repeated =
        context_ok ? app_test_request(&callbacks, session, context, context_length, &response,
                                      &response_length)
                   : CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    free(response);
    response = NULL;
    cbm_daemon_runtime_application_status_t after =
        app_test_request(&callbacks, session, mcp, mcp_length, &response, &response_length);
    bool ping_response =
        response && response_length > 0 && strstr((const char *)response, "\"result\":{}") != NULL;

    callbacks.session_close(callbacks.context, session);
    (void)cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    free(mcp);
    free(context);
    free(response);
    (void)cbm_rmdir(root);

    ASSERT_TRUE(application != NULL);
    ASSERT_TRUE(session != NULL);
    ASSERT_TRUE(mcp_ok);
    ASSERT_EQ(before, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_EQ(first, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_EQ(repeated, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_EQ(after, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(ping_response);
    PASS();
}

TEST(daemon_application_mcp_notification_has_no_response) {
    cbm_daemon_application_t *application = cbm_daemon_application_new(NULL);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session = app_test_open(&callbacks, 2);
    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-notify-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *notification = NULL;
    uint32_t notification_length = 0;
    uint8_t *response = (uint8_t *)(uintptr_t)1;
    uint32_t response_length = UINT32_MAX;
    bool encoded =
        root_ok && app_test_context_request(root, NULL, &context, &context_length) &&
        app_test_text_request(CBM_DAEMON_APPLICATION_REQUEST_MCP,
                              "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                              &notification, &notification_length);
    cbm_daemon_runtime_application_status_t context_status =
        encoded ? app_test_request(&callbacks, session, context, context_length, &response,
                                   &response_length)
                : CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    free(response);
    response = (uint8_t *)(uintptr_t)1;
    response_length = UINT32_MAX;
    cbm_daemon_runtime_application_status_t notification_status =
        encoded ? app_test_request(&callbacks, session, notification, notification_length,
                                   &response, &response_length)
                : CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;

    callbacks.session_close(callbacks.context, session);
    cbm_daemon_application_free(application);
    free(context);
    free(notification);
    (void)cbm_rmdir(root);

    ASSERT_TRUE(encoded);
    ASSERT_EQ(context_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_EQ(notification_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(response == NULL);
    ASSERT_EQ(response_length, 0);
    PASS();
}

static int app_test_index_noop(const char *project_name, const char *root_path, void *context) {
    (void)project_name;
    (void)root_path;
    (void)context;
    return 0;
}

static bool app_test_create_empty_file(const char *path) {
    FILE *file = path ? cbm_fopen(path, "wb") : NULL;
    return file && fclose(file) == 0;
}

/* Rebase guard: restricted MCP profiles are a property of one authenticated
 * daemon session. Before profile propagation, the daemon silently created a
 * full-surface MCP server, subscribed that session to the shared watcher, and
 * still accepted raw UI mutations. */
TEST(daemon_application_restricted_profile_owns_no_background_surfaces) {
    const char *old_cache = getenv("CBM_CACHE_DIR");
    bool had_cache = old_cache != NULL;
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;
    char root[APP_TEST_PATH_CAP];
    char cache[APP_TEST_PATH_CAP];
    (void)snprintf(root, sizeof(root),
                   "%s/cbm-app-profile-root-XXXXXX", cbm_tmpdir());
    (void)snprintf(cache, sizeof(cache),
                   "%s/cbm-app-profile-cache-XXXXXX", cbm_tmpdir());
    bool dirs_ok = cbm_mkdtemp(root) != NULL && cbm_mkdtemp(cache) != NULL;
    bool env_ok = dirs_ok && (!had_cache || saved_cache) &&
                  cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0;
    cbm_ui_config_t initial_ui = {.ui_enabled = false, .ui_port = 9749};
    bool ui_ready = env_ok && cbm_ui_config_save(&initial_ui);
    char *project = dirs_ok ? cbm_project_name_from_path(root) : NULL;
    char db_path[APP_TEST_PATH_CAP] = {0};
    if (project) {
        (void)snprintf(db_path, sizeof(db_path), "%s/%s.db", cache,
                       project);
    }
    bool db_ok = project && app_test_create_empty_file(db_path);
    cbm_store_t *store = cbm_store_open_memory();
    cbm_watcher_t *watcher =
        cbm_watcher_new(store, app_test_index_noop, NULL);
    cbm_daemon_application_config_t config = {.watcher = watcher};
    cbm_daemon_application_t *application =
        cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session =
        app_test_open(&callbacks, 304);

    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *initialize = NULL;
    uint32_t initialize_length = 0;
    uint8_t *index_call = NULL;
    uint32_t index_call_length = 0;
    bool encoded = db_ok && session &&
                   app_test_context_request_options(
                       root, root, CBM_MCP_TOOL_PROFILE_SCOUT, NULL, NULL,
                       &context, &context_length) &&
                   app_test_text_request(
                       CBM_DAEMON_APPLICATION_REQUEST_MCP,
                       "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}",
                       &initialize, &initialize_length) &&
                   app_test_text_request(
                       CBM_DAEMON_APPLICATION_REQUEST_MCP,
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                       "\"params\":{\"name\":\"index_repository\",\"arguments\":{}}}",
                       &index_call, &index_call_length);
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    cbm_daemon_runtime_application_status_t context_status =
        encoded ? app_test_request(&callbacks, session, context,
                                   context_length, &response,
                                   &response_length)
                : CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    free(response);
    response = NULL;
    cbm_daemon_runtime_application_status_t initialize_status =
        context_status == CBM_DAEMON_RUNTIME_APPLICATION_OK
            ? app_test_request(&callbacks, session, initialize,
                               initialize_length, &response,
                               &response_length)
            : CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    bool scout_surface =
        response && strstr((const char *)response, "scout tool profile") &&
        !strstr((const char *)response, "index_repository");
    free(response);
    response = NULL;
    cbm_daemon_runtime_application_status_t index_status =
        initialize_status == CBM_DAEMON_RUNTIME_APPLICATION_OK
            ? app_test_request(&callbacks, session, index_call,
                               index_call_length, &response,
                               &response_length)
            : CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    bool index_refused =
        response && strstr((const char *)response,
                           "not available in the scout tool profile");
    free(response);
    response = NULL;
    cbm_daemon_runtime_application_status_t ui_status =
        app_test_ui_config_request(
            &callbacks, session,
            CBM_DAEMON_APPLICATION_UI_CONFIG_ENABLED, 1, 0, 7);
    int watch_count = watcher ? cbm_watcher_watch_count(watcher) : -1;
    size_t active_jobs = application
                             ? cbm_daemon_application_active_jobs(application)
                             : SIZE_MAX;
    cbm_ui_config_t final_ui = {0};
    cbm_ui_config_load(&final_ui);

    if (session) {
        callbacks.session_close(callbacks.context, session);
    }
    bool stopped = application && cbm_daemon_application_shutdown(
                                      application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    if (watcher) {
        cbm_watcher_stop(watcher);
        cbm_watcher_free(watcher);
    }
    cbm_store_close(store);
    free(context);
    free(initialize);
    free(index_call);
    free(project);
    (void)cbm_unlink(db_path);
    (void)cbm_rmdir(root);
    if (had_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(saved_cache);
    (void)th_rmtree(cache);

    ASSERT_TRUE(dirs_ok);
    ASSERT_TRUE(env_ok);
    ASSERT_TRUE(ui_ready);
    ASSERT_TRUE(db_ok);
    ASSERT_TRUE(encoded);
    ASSERT_EQ(context_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_EQ(initialize_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(scout_surface);
    ASSERT_EQ(index_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(index_refused);
    ASSERT_EQ(ui_status, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_EQ(watch_count, 0);
    ASSERT_EQ(active_jobs, 0);
    ASSERT_FALSE(final_ui.ui_enabled);
    ASSERT_TRUE(stopped);
    PASS();
}

/* The thin hook process parses CLI metadata, but the daemon owns the MCP
 * session and therefore must retain that metadata with the session context.
 * Copilot deliberately omits the event from stdin, making this non-vacuous. */
TEST(daemon_application_hook_context_preserves_event_and_dialect) {
    char root[APP_TEST_PATH_CAP];
    (void)snprintf(root, sizeof(root),
                   "%s/cbm-app-hook-context-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    cbm_daemon_application_t *application =
        cbm_daemon_application_new(NULL);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session =
        app_test_open(&callbacks, 305);
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char input[APP_TEST_PATH_CAP + 32];
    (void)snprintf(input, sizeof(input), "{\"cwd\":\"%s\"}", root);
    uint8_t *hook = NULL;
    uint32_t hook_length = 0;
    bool encoded = root_ok && session &&
                   app_test_context_request_options(
                       root, root, CBM_MCP_TOOL_PROFILE_ALL, "SessionStart",
                       "copilot", &context, &context_length) &&
                   app_test_text_request(
                       CBM_DAEMON_APPLICATION_REQUEST_HOOK_AUGMENT, input,
                       &hook, &hook_length);
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    cbm_daemon_runtime_application_status_t context_status =
        encoded ? app_test_request(&callbacks, session, context,
                                   context_length, &response,
                                   &response_length)
                : CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    free(response);
    response = NULL;
    cbm_daemon_runtime_application_status_t hook_status =
        context_status == CBM_DAEMON_RUNTIME_APPLICATION_OK
            ? app_test_request(&callbacks, session, hook, hook_length,
                               &response, &response_length)
            : CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    bool copilot_shape =
        response && strstr((const char *)response, "additionalContext") &&
        strstr((const char *)response, "Session context") &&
        !strstr((const char *)response, "hookSpecificOutput");

    free(response);
    free(context);
    free(hook);
    if (session) {
        callbacks.session_close(callbacks.context, session);
    }
    bool stopped = application && cbm_daemon_application_shutdown(
                                      application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    if (root_ok) {
        (void)cbm_rmdir(root);
    }

    ASSERT_TRUE(root_ok);
    ASSERT_TRUE(encoded);
    ASSERT_EQ(context_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_EQ(hook_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(copilot_shape);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_reference_counts_one_shared_watch) {
    const char *old_cache = getenv("CBM_CACHE_DIR");
    bool had_cache = old_cache != NULL;
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;
    bool saved_cache_ok = !had_cache || saved_cache;
    char root[APP_TEST_PATH_CAP];
    char cache[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-watch-root-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-app-watch-cache-XXXXXX", cbm_tmpdir());
    bool dirs_ok = cbm_mkdtemp(root) != NULL && cbm_mkdtemp(cache) != NULL;
    bool env_ok = saved_cache_ok && cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0;
    char *project = dirs_ok ? cbm_project_name_from_path(root) : NULL;
    char db_path[APP_TEST_PATH_CAP] = {0};
    FILE *db = NULL;
    if (project) {
        snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
        db = cbm_fopen(db_path, "wb");
    }
    bool db_ok = db && fclose(db) == 0;
    cbm_store_t *store = cbm_store_open_memory();
    cbm_watcher_t *watcher = cbm_watcher_new(store, app_test_index_noop, NULL);
    cbm_daemon_application_config_t config = {.watcher = watcher, .config = NULL};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *first_session = app_test_open(&callbacks, 10);
    cbm_daemon_runtime_application_session_t *second_session = app_test_open(&callbacks, 11);
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *ping = NULL;
    uint32_t ping_length = 0;
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    bool encoded = dirs_ok && db_ok &&
                   app_test_context_request(root, root, &context, &context_length) &&
                   app_test_text_request(CBM_DAEMON_APPLICATION_REQUEST_MCP,
                                         "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}",
                                         &ping, &ping_length);
    bool requests_ok = encoded;
    cbm_daemon_runtime_application_session_t *sessions[2] = {first_session, second_session};
    for (size_t i = 0; requests_ok && i < 2; i++) {
        requests_ok = app_test_request(&callbacks, sessions[i], context, context_length, &response,
                                       &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK &&
                      app_test_request(&callbacks, sessions[i], ping, ping_length, &response,
                                       &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(response);
        response = NULL;
    }
    int shared_count = cbm_watcher_watch_count(watcher);
    callbacks.session_cancel(callbacks.context, first_session);
    int after_first_cancel = cbm_watcher_watch_count(watcher);
    callbacks.session_cancel(callbacks.context, second_session);
    int after_final_cancel = cbm_watcher_watch_count(watcher);
    callbacks.session_close(callbacks.context, first_session);
    int after_first_close = cbm_watcher_watch_count(watcher);
    callbacks.session_close(callbacks.context, second_session);
    int after_final_close = cbm_watcher_watch_count(watcher);

    cbm_daemon_application_free(application);
    cbm_watcher_stop(watcher);
    cbm_watcher_free(watcher);
    cbm_store_close(store);
    free(context);
    free(ping);
    free(response);
    free(project);
    (void)cbm_unlink(db_path);
    (void)cbm_rmdir(root);
    (void)cbm_rmdir(cache);
    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
    } else if (!had_cache) {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(saved_cache);

    ASSERT_TRUE(env_ok);
    ASSERT_TRUE(requests_ok);
    ASSERT_EQ(shared_count, 1);
    ASSERT_EQ(after_first_cancel, 1);
    ASSERT_EQ(after_final_cancel, 0);
    ASSERT_EQ(after_first_close, 0);
    ASSERT_EQ(after_final_close, 0);
    PASS();
}

TEST(daemon_application_free_releases_live_watch_once) {
    const char *old_cache = getenv("CBM_CACHE_DIR");
    bool had_cache = old_cache != NULL;
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;
    const char *old_grace = getenv("CBM_WATCHER_PRUNE_GRACE_S");
    bool had_grace = old_grace != NULL;
    char *saved_grace = old_grace ? cbm_strdup(old_grace) : NULL;
    char root[APP_TEST_PATH_CAP];
    char survivor_root[APP_TEST_PATH_CAP];
    char cache[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-free-watch-root-XXXXXX", cbm_tmpdir());
    snprintf(survivor_root, sizeof(survivor_root),
             "%s/cbm-app-free-survivor-root-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-app-free-watch-cache-XXXXXX", cbm_tmpdir());
    bool dirs_ok = cbm_mkdtemp(root) != NULL && cbm_mkdtemp(survivor_root) != NULL &&
                   cbm_mkdtemp(cache) != NULL;
    bool env_ok = (!had_cache || saved_cache) && (!had_grace || saved_grace) &&
                  cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0 &&
                  cbm_setenv("CBM_WATCHER_PRUNE_GRACE_S", "0", 1) == 0;
    char *project = dirs_ok ? cbm_project_name_from_path(root) : NULL;
    char *survivor_project = dirs_ok ? cbm_project_name_from_path(survivor_root) : NULL;
    char db_path[APP_TEST_PATH_CAP] = {0};
    char survivor_db_path[APP_TEST_PATH_CAP] = {0};
    if (project) {
        snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    }
    if (survivor_project) {
        snprintf(survivor_db_path, sizeof(survivor_db_path), "%s/%s.db", cache,
                 survivor_project);
    }
    bool db_ok = app_test_create_empty_file(db_path) &&
                 app_test_create_empty_file(survivor_db_path);
    cbm_store_t *store = cbm_store_open_memory();
    cbm_watcher_t *watcher = cbm_watcher_new(store, app_test_index_noop, NULL);
    cbm_daemon_application_config_t config = {.watcher = watcher};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session = app_test_open(&callbacks, 12);
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *ping = NULL;
    uint32_t ping_length = 0;
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    bool encoded = dirs_ok && env_ok && db_ok && session &&
                   app_test_context_request(root, root, &context, &context_length) &&
                   app_test_text_request(CBM_DAEMON_APPLICATION_REQUEST_MCP,
                                         "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\"}",
                                         &ping, &ping_length);
    bool requested = encoded &&
                     app_test_request(&callbacks, session, context, context_length, &response,
                                      &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(response);
    response = NULL;
    requested = requested &&
                app_test_request(&callbacks, session, ping, ping_length, &response,
                                 &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
    bool watch_live = requested && cbm_watcher_watch_count(watcher) == 1;

    /* Keep one watcher entry outside the application's subscription table.
     * It survives application teardown and then enters the destructive prune
     * path. If application_free leaves its borrowed callbacks installed, the
     * third poll calls through the freed application context (caught by ASan). */
    if (watch_live) {
        cbm_watcher_watch(watcher, survivor_project, survivor_root);
    }
    bool survivor_live = watch_live && cbm_watcher_watch_count(watcher) == 2;
    bool survivor_removed = survivor_live && cbm_rmdir(survivor_root) == 0;

    /* Deliberately leave the session open: application teardown owns both
     * the logical session and its one physical watch. ASan catches a second
     * release of the detached watch node. */
    cbm_daemon_application_free(application);
    bool survivor_still_watched = cbm_watcher_watch_count(watcher) == 1;
    for (int miss = 0; survivor_removed && miss < 3; miss++) {
        cbm_watcher_touch(watcher, survivor_project);
        (void)cbm_watcher_poll_once(watcher);
    }
    bool post_free_poll_safe = survivor_removed && cbm_watcher_watch_count(watcher) == 0 &&
                               !cbm_file_exists(survivor_db_path);
    cbm_watcher_stop(watcher);
    cbm_watcher_free(watcher);
    cbm_store_close(store);
    free(context);
    free(ping);
    free(response);
    free(project);
    free(survivor_project);
    (void)cbm_unlink(db_path);
    (void)cbm_unlink(survivor_db_path);
    (void)cbm_rmdir(root);
    (void)cbm_rmdir(survivor_root);
    (void)cbm_rmdir(cache);
    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
    } else if (!had_cache) {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    if (saved_grace) {
        (void)cbm_setenv("CBM_WATCHER_PRUNE_GRACE_S", saved_grace, 1);
    } else if (!had_grace) {
        (void)cbm_unsetenv("CBM_WATCHER_PRUNE_GRACE_S");
    }
    free(saved_cache);
    free(saved_grace);

    ASSERT_TRUE(encoded);
    ASSERT_TRUE(requested);
    ASSERT_TRUE(watch_live);
    ASSERT_TRUE(survivor_live);
    ASSERT_TRUE(survivor_still_watched);
    ASSERT_TRUE(post_free_poll_safe);
    PASS();
}

TEST(daemon_application_prune_clears_logical_watch_for_reregistration) {
    const char *old_cache = getenv("CBM_CACHE_DIR");
    bool had_cache = old_cache != NULL;
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;
    const char *old_grace = getenv("CBM_WATCHER_PRUNE_GRACE_S");
    bool had_grace = old_grace != NULL;
    char *saved_grace = old_grace ? cbm_strdup(old_grace) : NULL;
    char root[APP_TEST_PATH_CAP];
    char cache[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-prune-watch-root-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-app-prune-watch-cache-XXXXXX", cbm_tmpdir());
    bool dirs_ok = cbm_mkdtemp(root) != NULL && cbm_mkdtemp(cache) != NULL;
    bool env_ok = (!had_cache || saved_cache) && (!had_grace || saved_grace) &&
                  cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0 &&
                  cbm_setenv("CBM_WATCHER_PRUNE_GRACE_S", "0", 1) == 0;
    char *project = dirs_ok ? cbm_project_name_from_path(root) : NULL;
    char db_path[APP_TEST_PATH_CAP] = {0};
    char wal_path[APP_TEST_PATH_CAP] = {0};
    char shm_path[APP_TEST_PATH_CAP] = {0};
    if (project) {
        snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
        snprintf(wal_path, sizeof(wal_path), "%s/%s.db-wal", cache, project);
        snprintf(shm_path, sizeof(shm_path), "%s/%s.db-shm", cache, project);
    }
    bool files_ok = app_test_create_empty_file(db_path) &&
                    app_test_create_empty_file(wal_path) &&
                    app_test_create_empty_file(shm_path);
    cbm_store_t *store = cbm_store_open_memory();
    cbm_watcher_t *watcher = cbm_watcher_new(store, app_test_index_noop, NULL);
    cbm_daemon_application_config_t config = {.watcher = watcher};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *first = app_test_open(&callbacks, 13);
    cbm_daemon_runtime_application_session_t *second = NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *ping = NULL;
    uint32_t ping_length = 0;
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    bool encoded = dirs_ok && env_ok && files_ok && store && watcher && application && first &&
                   app_test_context_request(root, root, &context, &context_length) &&
                   app_test_text_request(CBM_DAEMON_APPLICATION_REQUEST_MCP,
                                         "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}",
                                         &ping, &ping_length);
    bool first_registered =
        encoded && app_test_request(&callbacks, first, context, context_length, &response,
                                    &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(response);
    response = NULL;
    first_registered =
        first_registered && app_test_request(&callbacks, first, ping, ping_length, &response,
                                             &response_length) ==
                                CBM_DAEMON_RUNTIME_APPLICATION_OK &&
        cbm_watcher_watch_count(watcher) == 1;
    free(response);
    response = NULL;

    bool root_removed = first_registered && cbm_rmdir(root) == 0;
    for (int miss = 0; root_removed && miss < 3; miss++) {
        cbm_watcher_touch(watcher, project);
        (void)cbm_watcher_poll_once(watcher);
    }
    bool pruned = root_removed && cbm_watcher_watch_count(watcher) == 0 &&
                  !cbm_file_exists(db_path) && !cbm_file_exists(wal_path) &&
                  !cbm_file_exists(shm_path);

    bool restored = pruned && cbm_mkdir_p(root, 0755) &&
                    app_test_create_empty_file(db_path);
    if (restored) {
        second = app_test_open(&callbacks, 14);
    }
    bool second_registered =
        second && app_test_request(&callbacks, second, context, context_length, &response,
                                   &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(response);
    response = NULL;
    second_registered =
        second_registered && app_test_request(&callbacks, second, ping, ping_length, &response,
                                              &response_length) ==
                                 CBM_DAEMON_RUNTIME_APPLICATION_OK &&
        cbm_watcher_watch_count(watcher) == 1;
    free(response);
    response = NULL;

    if (first) {
        callbacks.session_close(callbacks.context, first);
        first = NULL;
    }
    bool first_close_kept_reregistered_watch =
        second_registered && cbm_watcher_watch_count(watcher) == 1;
    if (second) {
        callbacks.session_close(callbacks.context, second);
        second = NULL;
    }
    bool final_close_released_watch = cbm_watcher_watch_count(watcher) == 0;

    cbm_daemon_application_free(application);
    cbm_watcher_stop(watcher);
    cbm_watcher_free(watcher);
    cbm_store_close(store);
    free(context);
    free(ping);
    free(response);
    free(project);
    (void)cbm_unlink(db_path);
    (void)cbm_unlink(wal_path);
    (void)cbm_unlink(shm_path);
    (void)cbm_rmdir(root);
    (void)cbm_rmdir(cache);
    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
    } else if (!had_cache) {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    if (saved_grace) {
        (void)cbm_setenv("CBM_WATCHER_PRUNE_GRACE_S", saved_grace, 1);
    } else if (!had_grace) {
        (void)cbm_unsetenv("CBM_WATCHER_PRUNE_GRACE_S");
    }
    free(saved_cache);
    free(saved_grace);

    ASSERT_TRUE(encoded);
    ASSERT_TRUE(first_registered);
    ASSERT_TRUE(pruned);
    ASSERT_TRUE(restored);
    ASSERT_TRUE(second_registered);
    ASSERT_TRUE(first_close_kept_reregistered_watch);
    ASSERT_TRUE(final_close_released_watch);
    PASS();
}

enum { APP_FAKE_MAX_ATTEMPTS = 16 };

typedef struct {
    atomic_int starts;
    atomic_int cancels;
    atomic_int destroys;
    atomic_bool allow_completion;
    atomic_bool unsafe_clean;
    atomic_bool scripted;
    atomic_int hold_destroy_attempt;
    atomic_bool release_destroy;
    atomic_int project_lock_attempts;
    atomic_int project_lock_acquisitions;
    cbm_project_lock_manager_t *project_locks;
    bool project_lock_busy_is_error;
    cbm_proc_outcome_t outcomes[APP_FAKE_MAX_ATTEMPTS];
    const char *responses[APP_FAKE_MAX_ATTEMPTS];
    const char *marker_payloads[APP_FAKE_MAX_ATTEMPTS];
    char marker_paths[APP_FAKE_MAX_ATTEMPTS][APP_TEST_PATH_CAP];
    char quarantine_paths[APP_FAKE_MAX_ATTEMPTS][APP_TEST_PATH_CAP];
    char quarantine_seen[APP_FAKE_MAX_ATTEMPTS][APP_TEST_PATH_CAP];
    size_t memory_budgets[APP_FAKE_MAX_ATTEMPTS];
} app_fake_worker_context_t;

typedef struct {
    app_fake_worker_context_t *context;
    int attempt;
    atomic_bool cancelled;
    char *project_key;
    cbm_project_lock_lease_t *project_lock_lease;
    cbm_index_worker_result_t result;
} app_fake_worker_t;

static void app_fake_worker_context_init(app_fake_worker_context_t *context) {
    memset(context, 0, sizeof(*context));
    atomic_init(&context->starts, 0);
    atomic_init(&context->cancels, 0);
    atomic_init(&context->destroys, 0);
    atomic_init(&context->allow_completion, false);
    atomic_init(&context->unsafe_clean, false);
    atomic_init(&context->scripted, false);
    atomic_init(&context->hold_destroy_attempt, -1);
    atomic_init(&context->release_destroy, false);
    atomic_init(&context->project_lock_attempts, 0);
    atomic_init(&context->project_lock_acquisitions, 0);
}

static void app_fake_worker_read_file(const char *path, char *out, size_t out_size) {
    if (!path || !path[0] || !out || out_size == 0) {
        return;
    }
    FILE *file = cbm_fopen(path, "rb");
    if (!file) {
        return;
    }
    size_t read = fread(out, 1, out_size - 1U, file);
    out[read] = '\0';
    (void)fclose(file);
}

static int app_fake_worker_start(void *opaque, const char *args_json,
                                 size_t memory_budget_bytes, const char *marker_file,
                                 const char *quarantine_file,
                                 cbm_daemon_application_worker_t *worker_out) {
    app_fake_worker_context_t *context = opaque;
    app_fake_worker_t *worker = calloc(1, sizeof(*worker));
    if (!worker) {
        return -1;
    }
    if (context->project_locks) {
        char *repo_path = cbm_mcp_get_string_arg(args_json, "repo_path");
        char *name_override = cbm_mcp_get_string_arg(args_json, "name");
        worker->project_key = cbm_project_name_from_path(
            name_override && name_override[0] ? name_override : repo_path);
        free(name_override);
        free(repo_path);
        if (!worker->project_key) {
            free(worker);
            return -1;
        }
    }
    worker->context = context;
    worker->attempt = atomic_fetch_add(&context->starts, 1);
    atomic_init(&worker->cancelled, false);
    worker->result.exit_code = -1;
    if (worker->attempt < APP_FAKE_MAX_ATTEMPTS) {
        context->memory_budgets[worker->attempt] = memory_budget_bytes;
        if (marker_file) {
            (void)snprintf(context->marker_paths[worker->attempt], APP_TEST_PATH_CAP, "%s",
                           marker_file);
        }
        if (quarantine_file) {
            (void)snprintf(context->quarantine_paths[worker->attempt], APP_TEST_PATH_CAP, "%s",
                           quarantine_file);
            app_fake_worker_read_file(quarantine_file, context->quarantine_seen[worker->attempt],
                                      sizeof(context->quarantine_seen[worker->attempt]));
        }
        const char *payload = context->marker_payloads[worker->attempt];
        if (marker_file && payload) {
            FILE *marker = cbm_fopen(marker_file, "ab");
            if (marker) {
                (void)fputs(payload, marker);
                (void)fclose(marker);
            }
        }
    }
    *worker_out = worker;
    return 0;
}

static cbm_index_worker_poll_t app_fake_worker_poll(void *opaque,
                                                    cbm_daemon_application_worker_t handle,
                                                    const cbm_index_worker_result_t **result_out) {
    (void)opaque;
    app_fake_worker_t *worker = handle;
    *result_out = NULL;
    if (atomic_load(&worker->cancelled)) {
        worker->result.outcome = CBM_PROC_KILLED;
        worker->result.cancellation_requested = true;
        worker->result.tree_quiesced = true;
        *result_out = &worker->result;
        return CBM_INDEX_WORKER_POLL_TERMINAL;
    }
    if (worker->context->project_locks && !worker->project_lock_lease) {
        cbm_project_lock_lease_t *lease = NULL;
        atomic_fetch_add(&worker->context->project_lock_attempts, 1);
        cbm_private_file_lock_status_t status = cbm_project_lock_try_acquire(
            worker->context->project_locks, worker->project_key, &lease);
        if (status == CBM_PRIVATE_FILE_LOCK_OK && lease) {
            worker->project_lock_lease = lease;
            atomic_fetch_add(&worker->context->project_lock_acquisitions, 1);
        } else {
            (void)app_project_lock_release(&lease);
            if (status == CBM_PRIVATE_FILE_LOCK_BUSY &&
                !worker->context->project_lock_busy_is_error) {
                return CBM_INDEX_WORKER_POLL_RUNNING;
            }
            return CBM_INDEX_WORKER_POLL_ERROR;
        }
    }
    if (atomic_load(&worker->context->scripted)) {
        cbm_proc_outcome_t outcome = worker->attempt < APP_FAKE_MAX_ATTEMPTS
                                         ? worker->context->outcomes[worker->attempt]
                                         : CBM_PROC_SPAWN_FAILED;
        worker->result.outcome = outcome;
        worker->result.exit_code = outcome == CBM_PROC_CLEAN ? 0 : -1;
        worker->result.tree_quiesced = true;
        const char *response =
            worker->attempt < APP_FAKE_MAX_ATTEMPTS &&
                    worker->context->responses[worker->attempt]
                ? worker->context->responses[worker->attempt]
                : "{\"content\":[{\"type\":\"text\",\"text\":\"{"
                  "\\\"status\\\":\\\"indexed\\\"}\"}]}";
        worker->result.response =
            outcome == CBM_PROC_CLEAN ? cbm_strdup(response) : NULL;
        *result_out = &worker->result;
        return CBM_INDEX_WORKER_POLL_TERMINAL;
    }
    if (atomic_load(&worker->context->allow_completion)) {
        worker->result.outcome = CBM_PROC_CLEAN;
        worker->result.exit_code = 0;
        worker->result.tree_quiesced = !atomic_load(&worker->context->unsafe_clean);
        worker->result.supervision_failed = atomic_load(&worker->context->unsafe_clean);
        worker->result.response = cbm_strdup(
            "{\"content\":[{\"type\":\"text\",\"text\":\"{\\\"status\\\":\\\"indexed\\\"}\"}]}");
        *result_out = &worker->result;
        return CBM_INDEX_WORKER_POLL_TERMINAL;
    }
    return CBM_INDEX_WORKER_POLL_RUNNING;
}

static bool app_fake_worker_cancel(void *opaque, cbm_daemon_application_worker_t handle) {
    app_fake_worker_context_t *context = opaque;
    app_fake_worker_t *worker = handle;
    bool first = !atomic_exchange(&worker->cancelled, true);
    if (first) {
        atomic_fetch_add(&context->cancels, 1);
    }
    return true;
}

static const char *app_fake_worker_log_path(void *opaque, cbm_daemon_application_worker_t handle) {
    (void)opaque;
    (void)handle;
    return "/tmp/cbm-fake-worker.log";
}

static void app_fake_worker_destroy(void *opaque, cbm_daemon_application_worker_t handle) {
    app_fake_worker_context_t *context = opaque;
    app_fake_worker_t *worker = handle;
    atomic_fetch_add(&context->destroys, 1);
    while (worker->attempt == atomic_load(&context->hold_destroy_attempt) &&
           !atomic_load(&context->release_destroy)) {
        cbm_usleep(1000);
    }
    (void)app_project_lock_release(&worker->project_lock_lease);
    free(worker->project_key);
    free(handle);
}

typedef struct {
    cbm_daemon_runtime_application_callbacks_t callbacks;
    cbm_daemon_runtime_application_session_t *session;
    cbm_daemon_runtime_application_token_t request_token;
    const uint8_t *request;
    uint32_t request_length;
    cbm_daemon_runtime_application_status_t status;
    uint8_t *response;
    uint32_t response_length;
    atomic_bool done;
} app_request_thread_t;

static void *app_request_thread(void *opaque) {
    app_request_thread_t *request = opaque;
    request->status = request->request_token ==
                              CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID
                          ? app_test_request(
                                &request->callbacks, request->session,
                                request->request, request->request_length,
                                &request->response, &request->response_length)
                          : app_test_request_tagged(
                                &request->callbacks, request->session,
                                request->request_token, request->request,
                                request->request_length, &request->response,
                                &request->response_length);
    atomic_store(&request->done, true);
    return NULL;
}

static bool app_wait_for_subscribers(cbm_daemon_application_t *application, const char *project,
                                     size_t expected) {
    uint64_t deadline = cbm_now_ms() + APP_TEST_TIMEOUT_MS;
    while (cbm_now_ms() < deadline) {
        if (cbm_daemon_application_job_subscribers(application, project) == expected) {
            return true;
        }
        cbm_usleep(1000);
    }
    return false;
}

static bool app_wait_for_atomic_int(atomic_int *value, int expected) {
    uint64_t deadline = cbm_now_ms() + APP_TEST_TIMEOUT_MS;
    while (cbm_now_ms() < deadline) {
        if (atomic_load(value) == expected) {
            return true;
        }
        cbm_usleep(1000);
    }
    return false;
}

static bool app_wait_for_atomic_int_at_least(atomic_int *value, int minimum) {
    uint64_t deadline = cbm_now_ms() + APP_TEST_TIMEOUT_MS;
    while (cbm_now_ms() < deadline) {
        if (atomic_load(value) >= minimum) {
            return true;
        }
        cbm_usleep(1000);
    }
    return false;
}

static bool app_wait_for_atomic_bool(atomic_bool *value, bool expected) {
    uint64_t deadline = cbm_now_ms() + APP_TEST_TIMEOUT_MS;
    while (cbm_now_ms() < deadline) {
        if (atomic_load(value) == expected) {
            return true;
        }
        cbm_usleep(1000);
    }
    return false;
}

static bool app_wait_for_active_jobs(cbm_daemon_application_t *application, size_t expected) {
    uint64_t deadline = cbm_now_ms() + APP_TEST_TIMEOUT_MS;
    while (cbm_now_ms() < deadline) {
        if (cbm_daemon_application_active_jobs(application) == expected) {
            return true;
        }
        cbm_usleep(1000);
    }
    return false;
}

static bool app_wait_for_terminal_job_with_subscribers(
    cbm_daemon_application_t *application, const char *project, size_t minimum_subscribers) {
    uint64_t deadline = cbm_now_ms() + APP_TEST_TIMEOUT_MS;
    while (cbm_now_ms() < deadline) {
        if (cbm_daemon_application_active_jobs(application) == 0 &&
            cbm_daemon_application_job_subscribers(application, project) >=
                minimum_subscribers) {
            return true;
        }
    }
    return false;
}

typedef struct {
    cbm_daemon_application_t *application;
    const char *project;
    const char *root;
    int result;
} app_index_thread_t;

static void *app_index_thread(void *opaque) {
    app_index_thread_t *request = opaque;
    request->result =
        cbm_daemon_application_index(request->application, request->project, request->root);
    return NULL;
}

typedef struct {
    bool had_cache;
    char *saved_cache;
    char root[APP_TEST_PATH_CAP];
    char cache[APP_TEST_PATH_CAP];
    char db_path[APP_TEST_PATH_CAP];
    char *project;
    app_fake_worker_context_t fake;
    cbm_store_t *store;
    cbm_watcher_t *watcher;
    cbm_daemon_application_t *application;
    cbm_daemon_runtime_application_callbacks_t callbacks;
    cbm_daemon_runtime_application_session_t *session;
} app_watch_race_fixture_t;

static bool app_watch_race_fixture_init(app_watch_race_fixture_t *fixture,
                                        cbm_daemon_client_id_t client_id) {
    memset(fixture, 0, sizeof(*fixture));
    app_fake_worker_context_init(&fixture->fake);
    const char *old_cache = getenv("CBM_CACHE_DIR");
    fixture->had_cache = old_cache != NULL;
    fixture->saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;
    bool saved_cache_ok = !fixture->had_cache || fixture->saved_cache;
    (void)snprintf(fixture->root, sizeof(fixture->root),
                   "%s/cbm-app-watch-race-root-XXXXXX", cbm_tmpdir());
    (void)snprintf(fixture->cache, sizeof(fixture->cache),
                   "%s/cbm-app-watch-race-cache-XXXXXX", cbm_tmpdir());
    bool dirs_ok = cbm_mkdtemp(fixture->root) != NULL &&
                   cbm_mkdtemp(fixture->cache) != NULL;
    bool env_ok = saved_cache_ok &&
                  cbm_setenv("CBM_CACHE_DIR", fixture->cache, 1) == 0;
    fixture->project = dirs_ok ? cbm_project_name_from_path(fixture->root) : NULL;
    if (fixture->project) {
        (void)snprintf(fixture->db_path, sizeof(fixture->db_path), "%s/%s.db",
                       fixture->cache, fixture->project);
    }
    bool db_ok = fixture->project &&
                 app_test_create_empty_file(fixture->db_path);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fixture->fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    fixture->store = cbm_store_open_memory();
    fixture->watcher =
        cbm_watcher_new(fixture->store, app_test_index_noop, NULL);
    cbm_daemon_application_config_t config = {
        .watcher = fixture->watcher,
        .worker_ops = &worker_ops,
    };
    fixture->application = cbm_daemon_application_new(&config);
    fixture->callbacks =
        cbm_daemon_application_runtime_callbacks(fixture->application);
    if (fixture->application) {
        fixture->session = app_test_open(&fixture->callbacks, client_id);
    }

    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *ping = NULL;
    uint32_t ping_length = 0;
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    bool encoded = env_ok && dirs_ok && db_ok && fixture->store &&
                   fixture->watcher && fixture->application && fixture->session &&
                   app_test_context_request(fixture->root, fixture->root,
                                            &context, &context_length) &&
                   app_test_text_request(
                       CBM_DAEMON_APPLICATION_REQUEST_MCP,
                       "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"ping\"}",
                       &ping, &ping_length);
    bool initialized = encoded &&
                       app_test_request(&fixture->callbacks, fixture->session,
                                        context, context_length, &response,
                                        &response_length) ==
                           CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(response);
    response = NULL;
    response_length = 0;
    initialized = initialized &&
                  app_test_request(&fixture->callbacks, fixture->session, ping,
                                   ping_length, &response, &response_length) ==
                      CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(response);
    free(context);
    free(ping);
    return initialized && cbm_watcher_watch_count(fixture->watcher) == 1;
}

static bool app_watch_race_fixture_finish(app_watch_race_fixture_t *fixture) {
    if (fixture->session) {
        fixture->callbacks.session_close(fixture->callbacks.context,
                                         fixture->session);
        fixture->session = NULL;
    }
    bool stopped = !fixture->application ||
                   cbm_daemon_application_shutdown(fixture->application,
                                                   APP_TEST_TIMEOUT_MS);
    if (stopped) {
        cbm_daemon_application_free(fixture->application);
        fixture->application = NULL;
        cbm_watcher_stop(fixture->watcher);
        cbm_watcher_free(fixture->watcher);
        fixture->watcher = NULL;
        cbm_store_close(fixture->store);
        fixture->store = NULL;
    }
    free(fixture->project);
    fixture->project = NULL;
    (void)th_rmtree(fixture->root);
    (void)th_rmtree(fixture->cache);
    bool restored = fixture->saved_cache
                        ? cbm_setenv("CBM_CACHE_DIR", fixture->saved_cache, 1) == 0
                        : (!fixture->had_cache &&
                           cbm_unsetenv("CBM_CACHE_DIR") == 0);
    free(fixture->saved_cache);
    fixture->saved_cache = NULL;
    return stopped && restored;
}

typedef struct {
    cbm_daemon_application_t *application;
    const char *project;
    const char *root;
    bool pause_before_subscribe;
    atomic_bool ready;
    atomic_bool proceed;
    atomic_bool done;
    int result;
} app_watcher_index_thread_t;

static void *app_watcher_index_thread(void *opaque) {
    app_watcher_index_thread_t *request = opaque;
    if (request->pause_before_subscribe) {
        atomic_store(&request->ready, true);
        while (!atomic_load(&request->proceed)) {
            cbm_usleep(1000);
        }
    }
    request->result = cbm_daemon_application_watcher_index(
        request->project, request->root, request->application);
    atomic_store(&request->done, true);
    return NULL;
}

TEST(daemon_application_coalesces_identical_index_requests) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {
        .worker_ops = &worker_ops,
        .physical_job_limit = 1,
    };
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *sessions[2] = {app_test_open(&callbacks, 21),
                                                             app_test_open(&callbacks, 22)};
    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-coalesce-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    char *project = root_ok ? cbm_project_name_from_path(root) : NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char args[APP_TEST_PATH_CAP + 32];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    bool encoded = root_ok && project &&
                   app_test_context_request(root, root, &context, &context_length) &&
                   app_test_tool_request("index_repository", args, &tool, &tool_length);
    bool contexts_ok = encoded;
    uint8_t *empty = NULL;
    uint32_t empty_length = 0;
    for (size_t i = 0; contexts_ok && i < 2; i++) {
        contexts_ok = app_test_request(&callbacks, sessions[i], context, context_length, &empty,
                                       &empty_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(empty);
        empty = NULL;
    }
    app_request_thread_t requests[2] = {
        {.callbacks = callbacks,
         .session = sessions[0],
         .request = tool,
         .request_length = tool_length},
        {.callbacks = callbacks,
         .session = sessions[1],
         .request = tool,
         .request_length = tool_length},
    };
    cbm_thread_t threads[2];
    bool thread_started[2] = {false, false};
    if (contexts_ok) {
        thread_started[0] =
            cbm_thread_create(&threads[0], 0, app_request_thread, &requests[0]) == 0;
    }
    bool first_joined = thread_started[0] && app_wait_for_subscribers(application, project, 1);
    if (first_joined) {
        thread_started[1] =
            cbm_thread_create(&threads[1], 0, app_request_thread, &requests[1]) == 0;
    }
    bool both_joined = thread_started[1] && app_wait_for_subscribers(application, project, 2);
    atomic_store(&fake.allow_completion, true);
    for (size_t i = 0; i < 2; i++) {
        if (thread_started[i]) {
            (void)cbm_thread_join(&threads[i]);
        }
    }
    for (size_t i = 0; i < 2; i++) {
        callbacks.session_close(callbacks.context, sessions[i]);
    }
    (void)cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);

    ASSERT_TRUE(contexts_ok);
    ASSERT_TRUE(first_joined);
    ASSERT_TRUE(both_joined);
    ASSERT_EQ(atomic_load(&fake.starts), 1);
    ASSERT_EQ(atomic_load(&fake.cancels), 0);
    ASSERT_EQ(atomic_load(&fake.destroys), 1);
    ASSERT_EQ(requests[0].status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_EQ(requests[1].status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(requests[0].response && strstr((char *)requests[0].response, "indexed"));
    ASSERT_TRUE(requests[1].response && strstr((char *)requests[1].response, "indexed"));

    free(requests[0].response);
    free(requests[1].response);
    free(context);
    free(tool);
    free(project);
    (void)cbm_rmdir(root);
    PASS();
}

TEST(daemon_application_fresh_request_does_not_reuse_terminal_subscribed_job) {
    enum { PRIOR_SUBSCRIBERS = 16 };
    static const char stale_response[] =
        "{\"content\":[{\"type\":\"text\",\"text\":\"stale-generation\"}]}";
    static const char fresh_response[] =
        "{\"content\":[{\"type\":\"text\",\"text\":\"fresh-generation\"}]}";

    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    atomic_store(&fake.scripted, true);
    atomic_store(&fake.hold_destroy_attempt, 0);
    fake.outcomes[0] = CBM_PROC_CLEAN;
    fake.outcomes[1] = CBM_PROC_CLEAN;
    fake.responses[0] = stale_response;
    fake.responses[1] = fresh_response;
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *sessions[PRIOR_SUBSCRIBERS + 1] = {0};
    bool sessions_ok = application != NULL;
    for (size_t i = 0; sessions_ok && i < PRIOR_SUBSCRIBERS + 1U; i++) {
        sessions[i] = app_test_open(&callbacks, (cbm_daemon_client_id_t)(51U + i));
        sessions_ok = sessions[i] != NULL;
    }

    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-terminal-fresh-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    char *project = root_ok ? cbm_project_name_from_path(root) : NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char args[APP_TEST_PATH_CAP + 32];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    bool setup = sessions_ok && root_ok && project &&
                 app_test_context_request(root, root, &context, &context_length) &&
                 app_test_tool_request("index_repository", args, &tool, &tool_length);
    for (size_t i = 0; setup && i < PRIOR_SUBSCRIBERS + 1U; i++) {
        uint8_t *empty = NULL;
        uint32_t empty_length = 0;
        setup = app_test_request(&callbacks, sessions[i], context, context_length, &empty,
                                 &empty_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(empty);
    }

    app_request_thread_t prior[PRIOR_SUBSCRIBERS];
    cbm_thread_t threads[PRIOR_SUBSCRIBERS];
    bool started[PRIOR_SUBSCRIBERS] = {0};
    memset(prior, 0, sizeof(prior));
    bool all_subscribed = setup;
    for (size_t i = 0; all_subscribed && i < PRIOR_SUBSCRIBERS; i++) {
        prior[i] = (app_request_thread_t){
            .callbacks = callbacks,
            .session = sessions[i],
            .request = tool,
            .request_length = tool_length,
        };
        atomic_init(&prior[i].done, false);
        started[i] = cbm_thread_create(&threads[i], 0, app_request_thread, &prior[i]) == 0;
        all_subscribed = started[i] &&
                         app_wait_for_subscribers(application, project, i + 1U);
        if (all_subscribed) {
            cbm_usleep(1000);
        }
    }
    bool first_worker_ready_to_publish =
        all_subscribed && app_wait_for_atomic_int(&fake.destroys, 1);
    atomic_store(&fake.release_destroy, true);
    bool terminal_with_prior_subscribers =
        first_worker_ready_to_publish && app_wait_for_terminal_job_with_subscribers(
                                             application, project, PRIOR_SUBSCRIBERS / 2U);

    uint8_t *fresh = NULL;
    uint32_t fresh_length = 0;
    cbm_daemon_runtime_application_status_t fresh_status =
        terminal_with_prior_subscribers
            ? app_test_request(&callbacks, sessions[PRIOR_SUBSCRIBERS], tool, tool_length, &fresh,
                               &fresh_length)
            : CBM_DAEMON_RUNTIME_APPLICATION_REJECTED;
    for (size_t i = 0; i < PRIOR_SUBSCRIBERS; i++) {
        if (started[i]) {
            (void)cbm_thread_join(&threads[i]);
        }
    }
    for (size_t i = 0; i < PRIOR_SUBSCRIBERS + 1U; i++) {
        if (sessions[i]) {
            callbacks.session_close(callbacks.context, sessions[i]);
        }
    }
    bool stopped =
        application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);

    ASSERT_TRUE(setup);
    ASSERT_TRUE(all_subscribed);
    ASSERT_TRUE(first_worker_ready_to_publish);
    ASSERT_TRUE(terminal_with_prior_subscribers);
    ASSERT_EQ(fresh_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_EQ(atomic_load(&fake.starts), 2);
    ASSERT_EQ(atomic_load(&fake.destroys), 2);
    ASSERT_TRUE(fresh && strstr((char *)fresh, "fresh-generation"));
    ASSERT_TRUE(!fresh || !strstr((char *)fresh, "stale-generation"));
    ASSERT_TRUE(!fresh || !strstr((char *)fresh, "different options"));
    ASSERT_TRUE(stopped);
    for (size_t i = 0; i < PRIOR_SUBSCRIBERS; i++) {
        ASSERT_EQ(prior[i].status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
        ASSERT_TRUE(prior[i].response && strstr((char *)prior[i].response, "stale-generation"));
        free(prior[i].response);
    }

    free(fresh);
    free(context);
    free(tool);
    free(project);
    (void)cbm_rmdir(root);
    PASS();
}

TEST(daemon_application_request_cancel_detaches_only_one_coalesced_subscriber) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application =
        cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *sessions[2] = {
        app_test_open(&callbacks, 3011), app_test_open(&callbacks, 3012)};
    char root[APP_TEST_PATH_CAP];
    (void)snprintf(root, sizeof(root),
                   "%s/cbm-app-request-cancel-coalesce-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    char *project = root_ok ? cbm_project_name_from_path(root) : NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char args[APP_TEST_PATH_CAP + 32];
    (void)snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    bool setup = application && sessions[0] && sessions[1] && root_ok &&
                 project &&
                 app_test_context_request(root, root, &context,
                                          &context_length) &&
                 app_test_tool_request("index_repository", args, &tool,
                                       &tool_length);
    uint8_t *empty = NULL;
    uint32_t empty_length = 0;
    for (size_t i = 0; setup && i < 2; i++) {
        setup = app_test_request(&callbacks, sessions[i], context,
                                 context_length, &empty, &empty_length) ==
                CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(empty);
        empty = NULL;
        empty_length = 0;
    }

    app_request_thread_t requests[2] = {
        {.callbacks = callbacks,
         .session = sessions[0],
         .request_token = UINT64_C(8101),
         .request = tool,
         .request_length = tool_length},
        {.callbacks = callbacks,
         .session = sessions[1],
         .request_token = UINT64_C(8102),
         .request = tool,
         .request_length = tool_length},
    };
    atomic_init(&requests[0].done, false);
    atomic_init(&requests[1].done, false);
    cbm_thread_t threads[2];
    bool started[2] = {false, false};
    if (setup) {
        started[0] =
            cbm_thread_create(&threads[0], 0, app_request_thread,
                              &requests[0]) == 0;
    }
    bool first_joined = started[0] &&
                        app_wait_for_subscribers(application, project, 1);
    if (first_joined) {
        started[1] =
            cbm_thread_create(&threads[1], 0, app_request_thread,
                              &requests[1]) == 0;
    }
    bool both_joined = started[1] &&
                       app_wait_for_subscribers(application, project, 2);
    if (both_joined) {
        callbacks.request_cancel(callbacks.context, sessions[0],
                                 requests[0].request_token);
    }
    bool one_left = both_joined &&
                    app_wait_for_subscribers(application, project, 1);
    bool cancelled_returned =
        one_left && app_wait_for_atomic_bool(&requests[0].done, true);
    int cancels_after_first = atomic_load(&fake.cancels);

    /* Always release the fake worker so a failed RED assertion cannot strand
     * either joinable request thread. */
    atomic_store(&fake.allow_completion, true);
    bool thread_joined[2] = {false, false};
    for (size_t i = 0; i < 2; i++) {
        if (started[i]) {
            thread_joined[i] = cbm_thread_join(&threads[i]) == 0;
        }
    }
    for (size_t i = 0; i < 2; i++) {
        if (sessions[i]) {
            callbacks.session_close(callbacks.context, sessions[i]);
        }
    }
    bool stopped = application && cbm_daemon_application_shutdown(
                                      application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    bool second_indexed =
        requests[1].response &&
        strstr((char *)requests[1].response, "indexed") != NULL;
    int final_cancels = atomic_load(&fake.cancels);
    int final_starts = atomic_load(&fake.starts);
    int final_destroys = atomic_load(&fake.destroys);
    free(requests[0].response);
    free(requests[1].response);
    free(context);
    free(tool);
    free(project);
    (void)cbm_rmdir(root);

    ASSERT_TRUE(setup);
    ASSERT_TRUE(first_joined);
    ASSERT_TRUE(both_joined);
    ASSERT_TRUE(one_left);
    ASSERT_TRUE(cancelled_returned);
    ASSERT_EQ(cancels_after_first, 0);
    ASSERT_TRUE(thread_joined[0]);
    ASSERT_TRUE(thread_joined[1]);
    ASSERT_EQ(requests[0].status, CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED);
    ASSERT_NULL(requests[0].response);
    ASSERT_EQ(requests[0].response_length, 0);
    ASSERT_EQ(requests[1].status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(second_indexed);
    ASSERT_EQ(final_cancels, 0);
    ASSERT_EQ(final_starts, 1);
    ASSERT_EQ(final_destroys, 1);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_cancels_physical_job_only_after_final_session) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *sessions[2] = {app_test_open(&callbacks, 31),
                                                             app_test_open(&callbacks, 32)};
    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-cancel-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    char *project = root_ok ? cbm_project_name_from_path(root) : NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char args[APP_TEST_PATH_CAP + 32];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    bool setup = root_ok && project &&
                 app_test_context_request(root, root, &context, &context_length) &&
                 app_test_tool_request("index_repository", args, &tool, &tool_length);
    uint8_t *empty = NULL;
    uint32_t empty_length = 0;
    for (size_t i = 0; setup && i < 2; i++) {
        setup = app_test_request(&callbacks, sessions[i], context, context_length, &empty,
                                 &empty_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(empty);
        empty = NULL;
    }
    app_request_thread_t requests[2] = {
        {.callbacks = callbacks,
         .session = sessions[0],
         .request = tool,
         .request_length = tool_length},
        {.callbacks = callbacks,
         .session = sessions[1],
         .request = tool,
         .request_length = tool_length},
    };
    cbm_thread_t threads[2];
    bool started[2] = {false, false};
    for (size_t i = 0; setup && i < 2; i++) {
        started[i] = cbm_thread_create(&threads[i], 0, app_request_thread, &requests[i]) == 0;
    }
    bool subscribed = started[0] && started[1] && app_wait_for_subscribers(application, project, 2);
    if (subscribed) {
        callbacks.session_cancel(callbacks.context, sessions[0]);
    }
    bool one_left = subscribed && app_wait_for_subscribers(application, project, 1);
    int cancels_after_first = atomic_load(&fake.cancels);
    if (one_left) {
        callbacks.session_cancel(callbacks.context, sessions[1]);
    }
    for (size_t i = 0; i < 2; i++) {
        if (started[i]) {
            (void)cbm_thread_join(&threads[i]);
        }
        callbacks.session_close(callbacks.context, sessions[i]);
    }
    (void)cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);

    ASSERT_TRUE(setup);
    ASSERT_TRUE(subscribed);
    ASSERT_TRUE(one_left);
    ASSERT_EQ(atomic_load(&fake.starts), 1);
    ASSERT_EQ(cancels_after_first, 0);
    ASSERT_EQ(atomic_load(&fake.cancels), 1);
    ASSERT_EQ(atomic_load(&fake.destroys), 1);
    ASSERT_EQ(requests[0].status, CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED);
    ASSERT_NULL(requests[0].response);
    ASSERT_EQ(requests[1].status, CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED);
    ASSERT_NULL(requests[1].response);

    free(requests[0].response);
    free(requests[1].response);
    free(context);
    free(tool);
    free(project);
    (void)cbm_rmdir(root);
    PASS();
}

TEST(daemon_application_disconnect_before_request_callback_is_sticky) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    atomic_store(&fake.scripted, true);
    fake.outcomes[0] = CBM_PROC_CLEAN;
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *cancelled_session =
        app_test_open(&callbacks, 37);
    cbm_daemon_runtime_application_session_t *healthy_session =
        app_test_open(&callbacks, 38);
    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-pre-callback-cancel-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char args[APP_TEST_PATH_CAP + 32];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    bool setup = application && cancelled_session && healthy_session && root_ok &&
                 app_test_context_request(root, root, &context, &context_length) &&
                 app_test_tool_request("index_repository", args, &tool, &tool_length);
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    for (size_t i = 0; setup && i < 2; i++) {
        cbm_daemon_runtime_application_session_t *session =
            i == 0 ? cancelled_session : healthy_session;
        setup = app_test_request(&callbacks, session, context, context_length, &response,
                                 &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(response);
        response = NULL;
        response_length = 0;
    }

    if (setup) {
        /* Runtime disconnect may win immediately after it creates the request
         * thread but before that thread enters the application callback. */
        callbacks.session_cancel(callbacks.context, cancelled_session);
    }
    cbm_daemon_runtime_application_status_t cancelled_status =
        setup ? app_test_request(&callbacks, cancelled_session, tool, tool_length, &response,
                                 &response_length)
              : CBM_DAEMON_RUNTIME_APPLICATION_HANDLER_ERROR;
    uint8_t *cancelled_response = response;
    uint32_t cancelled_response_length = response_length;
    response = NULL;
    response_length = 0;
    int starts_after_cancelled_callback = atomic_load(&fake.starts);

    cbm_daemon_runtime_application_status_t healthy_status =
        setup ? app_test_request(&callbacks, healthy_session, tool, tool_length, &response,
                                 &response_length)
              : CBM_DAEMON_RUNTIME_APPLICATION_HANDLER_ERROR;

    if (cancelled_session) {
        callbacks.session_close(callbacks.context, cancelled_session);
    }
    if (healthy_session) {
        callbacks.session_close(callbacks.context, healthy_session);
    }
    bool stopped = application &&
                   cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);

    ASSERT_TRUE(setup);
    ASSERT_EQ(cancelled_status, CBM_DAEMON_RUNTIME_APPLICATION_REJECTED);
    ASSERT_NULL(cancelled_response);
    ASSERT_EQ(cancelled_response_length, 0);
    ASSERT_EQ(starts_after_cancelled_callback, 0);
    ASSERT_EQ(healthy_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(response && strstr((char *)response, "indexed"));
    ASSERT_EQ(atomic_load(&fake.starts), 1);
    ASSERT_EQ(atomic_load(&fake.cancels), 0);
    ASSERT_EQ(atomic_load(&fake.destroys), 1);
    ASSERT_TRUE(stopped);

    free(cancelled_response);
    free(response);
    free(context);
    free(tool);
    (void)cbm_rmdir(root);
    PASS();
}

TEST(daemon_application_request_cancel_preserves_persistent_watch_and_session) {
    app_watch_race_fixture_t fixture;
    bool fixture_ready = app_watch_race_fixture_init(&fixture, 3021);
    char args[APP_TEST_PATH_CAP + 32];
    (void)snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}",
                   fixture.root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    uint8_t *ping = NULL;
    uint32_t ping_length = 0;
    bool encoded =
        fixture_ready &&
        app_test_tool_request("index_repository", args, &tool,
                              &tool_length) &&
        app_test_text_request(
            CBM_DAEMON_APPLICATION_REQUEST_MCP,
            "{\"jsonrpc\":\"2.0\",\"id\":3021,\"method\":\"ping\"}",
            &ping, &ping_length);
    app_request_thread_t request = {
        .callbacks = fixture.callbacks,
        .session = fixture.session,
        .request_token = UINT64_C(8201),
        .request = tool,
        .request_length = tool_length,
        .status = CBM_DAEMON_RUNTIME_APPLICATION_HANDLER_ERROR,
    };
    atomic_init(&request.done, false);
    cbm_thread_t request_thread;
    bool thread_started =
        encoded && cbm_thread_create(&request_thread, 0, app_request_thread,
                                     &request) == 0;
    bool subscribed =
        thread_started &&
        app_wait_for_subscribers(fixture.application, fixture.project, 1);
    if (subscribed) {
        fixture.callbacks.request_cancel(
            fixture.callbacks.context, fixture.session,
            request.request_token);
    }
    bool request_returned =
        subscribed && app_wait_for_atomic_bool(&request.done, true);
    if (!request_returned) {
        /* Keep cleanup bounded if the RED path fails to detach the request. */
        atomic_store(&fixture.fake.allow_completion, true);
    }
    bool thread_joined =
        thread_started && cbm_thread_join(&request_thread) == 0;
    int watch_count_after_cancel =
        fixture.watcher ? cbm_watcher_watch_count(fixture.watcher) : -1;

    uint8_t *ping_response = NULL;
    uint32_t ping_response_length = 0;
    cbm_daemon_runtime_application_status_t ping_status =
        CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;
    if (thread_joined && encoded) {
        ping_status = app_test_request_tagged(
            &fixture.callbacks, fixture.session, UINT64_C(8202), ping,
            ping_length, &ping_response, &ping_response_length);
    }
    int watch_count_after_ping =
        fixture.watcher ? cbm_watcher_watch_count(fixture.watcher) : -1;
    bool worker_cancelled =
        app_wait_for_atomic_int(&fixture.fake.cancels, 1);
    bool cleaned = app_watch_race_fixture_finish(&fixture);
    bool ping_matches =
        ping_response && strstr((char *)ping_response, "\"id\":3021") != NULL;
    int worker_starts = atomic_load(&fixture.fake.starts);
    int worker_destroys = atomic_load(&fixture.fake.destroys);
    free(request.response);
    free(ping_response);
    free(tool);
    free(ping);

    ASSERT_TRUE(fixture_ready);
    ASSERT_TRUE(encoded);
    ASSERT_TRUE(thread_started);
    ASSERT_TRUE(subscribed);
    ASSERT_TRUE(request_returned);
    ASSERT_TRUE(thread_joined);
    ASSERT_EQ(request.status, CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED);
    ASSERT_NULL(request.response);
    ASSERT_EQ(request.response_length, 0);
    ASSERT_EQ(watch_count_after_cancel, 1);
    ASSERT_EQ(ping_status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(ping_matches);
    ASSERT_GT(ping_response_length, 0);
    ASSERT_EQ(watch_count_after_ping, 1);
    ASSERT_TRUE(worker_cancelled);
    ASSERT_EQ(worker_starts, 1);
    ASSERT_EQ(worker_destroys, 1);
    ASSERT_TRUE(cleaned);
    PASS();
}

TEST(daemon_application_cancel_drops_watch_before_inflight_request_returns) {
    const char *old_cache = getenv("CBM_CACHE_DIR");
    bool had_cache = old_cache != NULL;
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;
    bool saved_cache_ok = !had_cache || saved_cache;
    char root[APP_TEST_PATH_CAP];
    char cache[APP_TEST_PATH_CAP];
    (void)snprintf(root, sizeof(root), "%s/cbm-app-cancel-watch-root-XXXXXX",
                   cbm_tmpdir());
    (void)snprintf(cache, sizeof(cache), "%s/cbm-app-cancel-watch-cache-XXXXXX",
                   cbm_tmpdir());
    bool dirs_ok = cbm_mkdtemp(root) != NULL && cbm_mkdtemp(cache) != NULL;
    bool env_ok = saved_cache_ok && cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0;
    char *project = dirs_ok ? cbm_project_name_from_path(root) : NULL;
    char db_path[APP_TEST_PATH_CAP] = {0};
    if (project) {
        (void)snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    }
    bool db_ok = project && app_test_create_empty_file(db_path);

    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_store_t *store = cbm_store_open_memory();
    cbm_watcher_t *watcher = cbm_watcher_new(store, app_test_index_noop, NULL);
    cbm_daemon_application_config_t config = {
        .watcher = watcher,
        .worker_ops = &worker_ops,
    };
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session = app_test_open(&callbacks, 39);

    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *ping = NULL;
    uint32_t ping_length = 0;
    char args[APP_TEST_PATH_CAP + 32];
    (void)snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    bool setup = env_ok && db_ok && store && watcher && application && session &&
                 app_test_context_request(root, root, &context, &context_length) &&
                 app_test_text_request(
                     CBM_DAEMON_APPLICATION_REQUEST_MCP,
                     "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"ping\"}",
                     &ping, &ping_length) &&
                 app_test_tool_request("index_repository", args, &tool,
                                       &tool_length);
    if (setup) {
        setup = app_test_request(&callbacks, session, context, context_length,
                                 &response, &response_length) ==
                CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(response);
        response = NULL;
        response_length = 0;
    }
    if (setup) {
        setup = app_test_request(&callbacks, session, ping, ping_length,
                                 &response, &response_length) ==
                CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(response);
        response = NULL;
        response_length = 0;
    }
    bool watch_registered = setup && cbm_watcher_watch_count(watcher) == 1;

    app_request_thread_t request = {
        .callbacks = callbacks,
        .session = session,
        .request = tool,
        .request_length = tool_length,
        .status = CBM_DAEMON_RUNTIME_APPLICATION_HANDLER_ERROR,
    };
    atomic_init(&request.done, false);
    cbm_thread_t request_thread;
    bool thread_started =
        watch_registered &&
        cbm_thread_create(&request_thread, 0, app_request_thread, &request) == 0;
    bool worker_started =
        thread_started && app_wait_for_atomic_int(&fake.starts, 1);
    if (thread_started) {
        /* Runtime close is intentionally delayed until the in-flight request
         * returns; cancel is the disconnect ownership boundary. */
        callbacks.session_cancel(callbacks.context, session);
    }
    int watch_count_at_cancel = cbm_watcher_watch_count(watcher);
    bool thread_joined = thread_started && cbm_thread_join(&request_thread) == 0;
    int watch_count_after_request = cbm_watcher_watch_count(watcher);
    bool response_cancelled =
        request.status == CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED &&
        request.response == NULL && request.response_length == 0;

    if (session) {
        callbacks.session_close(callbacks.context, session);
    }
    bool stopped = application &&
                   cbm_daemon_application_shutdown(application,
                                                   APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    cbm_watcher_stop(watcher);
    cbm_watcher_free(watcher);
    cbm_store_close(store);
    free(request.response);
    free(context);
    free(ping);
    free(tool);
    free(project);
    (void)cbm_unlink(db_path);
    (void)cbm_rmdir(root);
    (void)cbm_rmdir(cache);
    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
    } else if (!had_cache) {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(saved_cache);

    ASSERT_TRUE(setup);
    ASSERT_TRUE(watch_registered);
    ASSERT_TRUE(thread_started);
    ASSERT_TRUE(worker_started);
    ASSERT_EQ(watch_count_at_cancel, 0);
    ASSERT_TRUE(thread_joined);
    ASSERT_EQ(watch_count_after_request, 0);
    ASSERT_TRUE(response_cancelled);
    ASSERT_EQ(atomic_load(&fake.cancels), 1);
    ASSERT_EQ(atomic_load(&fake.destroys), 1);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_stale_watcher_callback_is_rejected_at_job_admission) {
    app_watch_race_fixture_t fixture;
    bool fixture_ready = app_watch_race_fixture_init(&fixture, 40);
    app_watcher_index_thread_t request = {
        .application = fixture.application,
        .project = fixture.project,
        .root = fixture.root,
        .pause_before_subscribe = true,
        .result = -1,
    };
    atomic_init(&request.ready, false);
    atomic_init(&request.proceed, false);
    atomic_init(&request.done, false);
    cbm_thread_t thread;
    bool thread_started =
        fixture_ready &&
        cbm_thread_create(&thread, 0, app_watcher_index_thread, &request) == 0;
    bool paused_before_subscribe =
        thread_started && app_wait_for_atomic_bool(&request.ready, true);

    if (thread_started) {
        /* Models a watcher callback selected before unwatch but scheduled
         * after the last owning session crosses its cancel boundary. */
        fixture.callbacks.session_cancel(fixture.callbacks.context,
                                         fixture.session);
    }
    int watches_after_cancel = cbm_watcher_watch_count(fixture.watcher);
    atomic_store(&request.proceed, true);
    /* On unfixed code the stale callback starts a worker. Let that RED path
     * finish instead of hanging the suite; the zero-start assertion remains
     * the reproduction oracle. */
    atomic_store(&fixture.fake.allow_completion, true);
    bool request_done = thread_started &&
                        app_wait_for_atomic_bool(&request.done, true);
    bool thread_joined = request_done && cbm_thread_join(&thread) == 0;
    int worker_starts = atomic_load(&fixture.fake.starts);
    size_t active_jobs =
        cbm_daemon_application_active_jobs(fixture.application);
    bool cleaned = app_watch_race_fixture_finish(&fixture);

    ASSERT_TRUE(fixture_ready);
    ASSERT_TRUE(thread_started);
    ASSERT_TRUE(paused_before_subscribe);
    ASSERT_EQ(watches_after_cancel, 0);
    ASSERT_TRUE(thread_joined);
    ASSERT_EQ(request.result, 1);
    ASSERT_EQ(worker_starts, 0);
    ASSERT_EQ(active_jobs, 0);
    ASSERT_TRUE(cleaned);
    PASS();
}

TEST(daemon_application_final_cancel_drains_admitted_watcher_job) {
    app_watch_race_fixture_t fixture;
    bool fixture_ready = app_watch_race_fixture_init(&fixture, 41);
    app_watcher_index_thread_t request = {
        .application = fixture.application,
        .project = fixture.project,
        .root = fixture.root,
        .pause_before_subscribe = false,
        .result = -1,
    };
    atomic_init(&request.ready, false);
    atomic_init(&request.proceed, true);
    atomic_init(&request.done, false);
    cbm_thread_t thread;
    bool thread_started =
        fixture_ready &&
        cbm_thread_create(&thread, 0, app_watcher_index_thread, &request) == 0;
    bool worker_started =
        thread_started && app_wait_for_atomic_int(&fixture.fake.starts, 1);
    bool job_active = worker_started &&
                      cbm_daemon_application_active_jobs(
                          fixture.application) == 1;

    if (thread_started) {
        fixture.callbacks.session_cancel(fixture.callbacks.context,
                                         fixture.session);
    }
    int watches_after_cancel = cbm_watcher_watch_count(fixture.watcher);
    bool cancelled_by_final_session =
        thread_started && app_wait_for_atomic_int(&fixture.fake.cancels, 1);
    if (!cancelled_by_final_session) {
        /* RED cleanup: prior behavior leaves this watcher-owned worker alive
         * while another daemon client could keep the process running. */
        atomic_store(&fixture.fake.allow_completion, true);
    }
    bool request_done = thread_started &&
                        app_wait_for_atomic_bool(&request.done, true);
    bool thread_joined = request_done && cbm_thread_join(&thread) == 0;
    size_t active_jobs_after_join =
        cbm_daemon_application_active_jobs(fixture.application);
    int worker_cancels = atomic_load(&fixture.fake.cancels);
    int worker_destroys = atomic_load(&fixture.fake.destroys);
    bool cleaned = app_watch_race_fixture_finish(&fixture);

    ASSERT_TRUE(fixture_ready);
    ASSERT_TRUE(thread_started);
    ASSERT_TRUE(worker_started);
    ASSERT_TRUE(job_active);
    ASSERT_EQ(watches_after_cancel, 0);
    ASSERT_TRUE(thread_joined);
    ASSERT_EQ(request.result, 1);
    ASSERT_EQ(active_jobs_after_join, 0);
    ASSERT_TRUE(cancelled_by_final_session);
    ASSERT_EQ(worker_cancels, 1);
    ASSERT_EQ(worker_destroys, 1);
    ASSERT_TRUE(cleaned);
    PASS();
}

TEST(daemon_application_watcher_job_follows_exact_live_watch_owners) {
    app_watch_race_fixture_t fixture;
    bool fixture_ready = app_watch_race_fixture_init(&fixture, 42);
    cbm_daemon_runtime_application_session_t *second =
        fixture_ready ? app_test_open(&fixture.callbacks, 43) : NULL;
    /* This unrelated live session keeps the daemon generation alive after
     * both matching watch owners disconnect. The watcher job must still be
     * cancelled when its own final owner disappears. */
    cbm_daemon_runtime_application_session_t *unrelated =
        fixture_ready ? app_test_open(&fixture.callbacks, 44) : NULL;

    uint8_t *context = NULL;
    uint32_t context_length = 0;
    uint8_t *ping = NULL;
    uint32_t ping_length = 0;
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    bool encoded = fixture_ready && second && unrelated &&
                   app_test_context_request(fixture.root, fixture.root,
                                            &context, &context_length) &&
                   app_test_text_request(
                       CBM_DAEMON_APPLICATION_REQUEST_MCP,
                       "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"ping\"}",
                       &ping, &ping_length);
    bool second_watching =
        encoded &&
        app_test_request(&fixture.callbacks, second, context, context_length,
                         &response, &response_length) ==
            CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(response);
    response = NULL;
    response_length = 0;
    second_watching =
        second_watching &&
        app_test_request(&fixture.callbacks, second, ping, ping_length,
                         &response, &response_length) ==
            CBM_DAEMON_RUNTIME_APPLICATION_OK &&
        cbm_watcher_watch_count(fixture.watcher) == 1;
    free(response);
    response = NULL;

    app_watcher_index_thread_t request = {
        .application = fixture.application,
        .project = fixture.project,
        .root = fixture.root,
        .pause_before_subscribe = false,
        .result = -1,
    };
    atomic_init(&request.ready, false);
    atomic_init(&request.proceed, true);
    atomic_init(&request.done, false);
    cbm_thread_t thread;
    bool thread_started =
        second_watching &&
        cbm_thread_create(&thread, 0, app_watcher_index_thread, &request) == 0;
    bool worker_started =
        thread_started && app_wait_for_atomic_int(&fixture.fake.starts, 1);
    size_t initial_subscribers =
        worker_started
            ? cbm_daemon_application_job_subscribers(fixture.application,
                                                     fixture.project)
            : 0;

    if (fixture.session) {
        fixture.callbacks.session_close(fixture.callbacks.context,
                                        fixture.session);
        fixture.session = NULL;
    }
    int watches_after_first = cbm_watcher_watch_count(fixture.watcher);
    size_t subscribers_after_first =
        cbm_daemon_application_job_subscribers(fixture.application,
                                               fixture.project);
    int cancels_after_first = atomic_load(&fixture.fake.cancels);

    if (second) {
        fixture.callbacks.session_close(fixture.callbacks.context, second);
        second = NULL;
    }
    int watches_after_second = cbm_watcher_watch_count(fixture.watcher);
    size_t subscribers_after_second =
        cbm_daemon_application_job_subscribers(fixture.application,
                                               fixture.project);
    bool cancelled_without_final_session =
        thread_started && app_wait_for_atomic_int(&fixture.fake.cancels, 1);
    if (!cancelled_without_final_session) {
        /* RED cleanup: the old anonymous watcher subscription keeps the job
         * alive here, so let it finish without leaking the test thread. */
        atomic_store(&fixture.fake.allow_completion, true);
    }
    bool request_done =
        thread_started && app_wait_for_atomic_bool(&request.done, true);
    bool thread_joined = request_done && cbm_thread_join(&thread) == 0;

    if (unrelated) {
        fixture.callbacks.session_close(fixture.callbacks.context, unrelated);
        unrelated = NULL;
    }
    bool cleaned = app_watch_race_fixture_finish(&fixture);
    free(context);
    free(ping);
    free(response);

    ASSERT_TRUE(fixture_ready);
    ASSERT_TRUE(second_watching);
    ASSERT_TRUE(thread_started);
    ASSERT_TRUE(worker_started);
    ASSERT_EQ(initial_subscribers, 2);
    ASSERT_EQ(watches_after_first, 1);
    ASSERT_EQ(subscribers_after_first, 1);
    ASSERT_EQ(cancels_after_first, 0);
    ASSERT_EQ(watches_after_second, 0);
    ASSERT_EQ(subscribers_after_second, 0);
    ASSERT_TRUE(cancelled_without_final_session);
    ASSERT_TRUE(thread_joined);
    ASSERT_EQ(request.result, 1);
    ASSERT_EQ(atomic_load(&fixture.fake.destroys), 1);
    ASSERT_TRUE(cleaned);
    PASS();
}

/* A staged index snapshots the current DB before it publishes a replacement.
 * Any ADR write accepted during that lifetime can therefore be silently lost
 * (and can make replacement fail on Windows). The daemon job must keep the
 * project reserved against mutations until the physical worker is terminal. */
TEST(daemon_application_serializes_adr_mutation_with_index_job) {
    const char *old_cache = getenv("CBM_CACHE_DIR");
    bool had_cache = old_cache != NULL;
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};

    char root[APP_TEST_PATH_CAP];
    char cache[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-mutation-root-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-app-mutation-cache-XXXXXX", cbm_tmpdir());
    bool dirs_ok = cbm_mkdtemp(root) != NULL && cbm_mkdtemp(cache) != NULL;
    bool env_ok = dirs_ok && (!had_cache || saved_cache) &&
                  cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0;
    char *project = env_ok ? cbm_project_name_from_path(root) : NULL;
    char db_path[APP_TEST_PATH_CAP] = {0};
    cbm_store_t *seed = NULL;
    bool seeded = false;
    if (project) {
        snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
        seed = cbm_store_open_path(db_path);
        seeded = seed && cbm_store_upsert_project(seed, project, root) == CBM_STORE_OK;
        cbm_store_close(seed);
    }

    cbm_daemon_application_t *application =
        seeded ? cbm_daemon_application_new(&config) : NULL;
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *index_session =
        application ? app_test_open(&callbacks, 33) : NULL;
    cbm_daemon_runtime_application_session_t *adr_session =
        application ? app_test_open(&callbacks, 34) : NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char index_args[APP_TEST_PATH_CAP + 32];
    snprintf(index_args, sizeof(index_args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *index_tool = NULL;
    uint32_t index_tool_length = 0;
    char adr_args[APP_TEST_PATH_CAP + 128];
    snprintf(adr_args, sizeof(adr_args),
             "{\"project\":\"%s\",\"mode\":\"update\","
             "\"content\":\"serialized ADR\"}",
             project ? project : "");
    uint8_t *adr_tool = NULL;
    uint32_t adr_tool_length = 0;
    bool setup = application && index_session && adr_session &&
                 app_test_context_request(root, root, &context, &context_length) &&
                 app_test_tool_request("index_repository", index_args, &index_tool,
                                       &index_tool_length) &&
                 app_test_tool_request("manage_adr", adr_args, &adr_tool, &adr_tool_length);
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    if (setup) {
        setup = app_test_request(&callbacks, index_session, context, context_length, &response,
                                 &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(response);
        response = NULL;
    }
    if (setup) {
        setup = app_test_request(&callbacks, adr_session, context, context_length, &response,
                                 &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(response);
        response = NULL;
    }

    app_request_thread_t index_request = {
        .callbacks = callbacks,
        .session = index_session,
        .request = index_tool,
        .request_length = index_tool_length,
    };
    app_request_thread_t adr_request = {
        .callbacks = callbacks,
        .session = adr_session,
        .request = adr_tool,
        .request_length = adr_tool_length,
    };
    cbm_thread_t index_thread;
    cbm_thread_t adr_thread;
    bool index_started =
        setup && cbm_thread_create(&index_thread, 0, app_request_thread, &index_request) == 0;
    bool index_active = index_started && project &&
                        app_wait_for_subscribers(application, project, 1) &&
                        app_wait_for_atomic_int(&fake.starts, 1);
    bool adr_started = index_active &&
                       cbm_thread_create(&adr_thread, 0, app_request_thread, &adr_request) == 0;
    uint64_t early_deadline = cbm_now_ms() + 100;
    while (adr_started && !atomic_load(&adr_request.done) && cbm_now_ms() < early_deadline) {
        cbm_usleep(1000);
    }
    bool adr_completed_while_index_active = adr_started && atomic_load(&adr_request.done);

    atomic_store(&fake.allow_completion, true);
    if (index_started) {
        (void)cbm_thread_join(&index_thread);
    }
    if (adr_started) {
        (void)cbm_thread_join(&adr_thread);
    }

    cbm_store_t *check = cbm_store_open_path_query(db_path);
    cbm_adr_t adr = {0};
    bool adr_persisted = check && cbm_store_adr_get(check, project, &adr) == CBM_STORE_OK &&
                         adr.content && strcmp(adr.content, "serialized ADR") == 0;
    if (adr.content) {
        cbm_store_adr_free(&adr);
    }
    cbm_store_close(check);
    if (index_session) {
        callbacks.session_close(callbacks.context, index_session);
    }
    if (adr_session) {
        callbacks.session_close(callbacks.context, adr_session);
    }
    bool stopped = application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    bool adr_response_updated =
        adr_request.response && strstr((char *)adr_request.response, "updated") != NULL;

    free(index_request.response);
    free(adr_request.response);
    free(context);
    free(index_tool);
    free(adr_tool);
    (void)cbm_unlink(db_path);
    char sidecar[APP_TEST_PATH_CAP];
    snprintf(sidecar, sizeof(sidecar), "%s-wal", db_path);
    (void)cbm_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", db_path);
    (void)cbm_unlink(sidecar);
    free(project);
    (void)cbm_rmdir(root);
    (void)cbm_rmdir(cache);
    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
    } else if (!had_cache) {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(saved_cache);

    ASSERT_TRUE(env_ok);
    ASSERT_TRUE(seeded);
    ASSERT_TRUE(setup);
    ASSERT_TRUE(index_active);
    ASSERT_TRUE(adr_started);
    ASSERT_FALSE(adr_completed_while_index_active);
    ASSERT_EQ(index_request.status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_EQ(adr_request.status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(adr_response_updated);
    ASSERT_TRUE(adr_persisted);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_reserved_mutation_delays_worker_start) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-mutation-first-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    const char *project = "mutation-first";
    bool lease_held = application &&
                      cbm_daemon_application_project_mutation_try_begin(application, project);
    app_index_thread_t request = {
        .application = application,
        .project = project,
        .root = root,
        .result = -1,
    };
    cbm_thread_t thread;
    bool thread_started = root_ok && lease_held &&
                          cbm_thread_create(&thread, 0, app_index_thread, &request) == 0;
    bool reserved = thread_started && app_wait_for_subscribers(application, project, 1);
    uint64_t early_deadline = cbm_now_ms() + 100;
    while (atomic_load(&fake.starts) == 0 && cbm_now_ms() < early_deadline) {
        cbm_usleep(1000);
    }
    bool worker_started_while_mutating = atomic_load(&fake.starts) != 0;
    if (lease_held) {
        cbm_daemon_application_project_mutation_end(application, project);
    }
    bool worker_started = thread_started && app_wait_for_atomic_int(&fake.starts, 1);
    atomic_store(&fake.allow_completion, true);
    if (thread_started) {
        (void)cbm_thread_join(&thread);
    }
    bool stopped = application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    (void)cbm_rmdir(root);

    ASSERT_TRUE(root_ok);
    ASSERT_TRUE(lease_held);
    ASSERT_TRUE(thread_started);
    ASSERT_TRUE(reserved);
    ASSERT_FALSE(worker_started_while_mutating);
    ASSERT_TRUE(worker_started);
    ASSERT_EQ(request.result, 0);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_mutation_does_not_block_other_project) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-mutation-other-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    bool lease_held = application &&
                      cbm_daemon_application_project_mutation_try_begin(application,
                                                                        "project-a");
    app_index_thread_t request = {
        .application = application,
        .project = "project-b",
        .root = root,
        .result = -1,
    };
    cbm_thread_t thread;
    bool thread_started = root_ok && lease_held &&
                          cbm_thread_create(&thread, 0, app_index_thread, &request) == 0;
    bool worker_started = thread_started && app_wait_for_atomic_int(&fake.starts, 1);
    atomic_store(&fake.allow_completion, true);
    if (thread_started) {
        (void)cbm_thread_join(&thread);
    }
    if (lease_held) {
        cbm_daemon_application_project_mutation_end(application, "project-a");
    }
    bool stopped = application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    (void)cbm_rmdir(root);

    ASSERT_TRUE(root_ok);
    ASSERT_TRUE(lease_held);
    ASSERT_TRUE(thread_started);
    ASSERT_TRUE(worker_started);
    ASSERT_EQ(request.result, 0);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_mutation_still_owns_os_project_lock) {
    app_project_lock_fixture_t locks;
    bool locks_ready = app_project_lock_fixture_init(&locks);
    cbm_daemon_application_config_t config = {
        .project_locks = locks_ready ? locks.daemon_locks : NULL,
    };
    cbm_daemon_application_t *application =
        locks_ready ? cbm_daemon_application_new(&config) : NULL;
    const char *project = "daemon-mutation-native-lock";
    bool mutation_started =
        application &&
        cbm_daemon_application_project_mutation_try_begin(application,
                                                          project);

    cbm_project_lock_lease_t *active_probe = NULL;
    cbm_private_file_lock_status_t active_status =
        mutation_started
            ? cbm_project_lock_try_acquire(locks.probe_locks, project,
                                           &active_probe)
            : CBM_PRIVATE_FILE_LOCK_IO;
    bool mutation_holds_lock =
        active_status == CBM_PRIVATE_FILE_LOCK_BUSY && !active_probe;
    bool active_probe_released = app_project_lock_release(&active_probe);
    if (mutation_started) {
        cbm_daemon_application_project_mutation_end(application, project);
    }

    cbm_project_lock_lease_t *terminal_probe = NULL;
    cbm_private_file_lock_status_t terminal_status =
        mutation_started
            ? cbm_project_lock_try_acquire(locks.probe_locks, project,
                                           &terminal_probe)
            : CBM_PRIVATE_FILE_LOCK_IO;
    bool mutation_released_lock =
        terminal_status == CBM_PRIVATE_FILE_LOCK_OK && terminal_probe;
    bool terminal_probe_released = app_project_lock_release(&terminal_probe);
    bool stopped = application &&
                   cbm_daemon_application_shutdown(application,
                                                   APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    bool locks_clean = app_project_lock_fixture_cleanup(&locks);

    ASSERT_TRUE(locks_ready);
    ASSERT_NOT_NULL(application);
    ASSERT_TRUE(mutation_started);
    ASSERT_TRUE(mutation_holds_lock);
    ASSERT_TRUE(active_probe_released);
    ASSERT_TRUE(mutation_released_lock);
    ASSERT_TRUE(terminal_probe_released);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(locks_clean);
    PASS();
}

/* RED on daemon-held project leases: the injected physical-worker boundary
 * attempts the exact same native lock. A parent lease makes that attempt fail;
 * the correct ownership has the worker acquire it and hold it until destroy. */
TEST(daemon_application_worker_boundary_is_sole_os_project_lock_owner) {
    app_project_lock_fixture_t locks;
    bool locks_ready = app_project_lock_fixture_init(&locks);
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    fake.project_locks = locks_ready ? locks.worker_locks : NULL;
    fake.project_lock_busy_is_error = true;
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {
        .worker_ops = &worker_ops,
        .project_locks = locks_ready ? locks.daemon_locks : NULL,
    };
    cbm_daemon_application_t *application =
        locks_ready ? cbm_daemon_application_new(&config) : NULL;

    char root[APP_TEST_PATH_CAP];
    (void)snprintf(root, sizeof(root),
                   "%s/cbm-app-worker-lock-owner-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    const char *project = "worker-lock-owner";
    app_index_thread_t request = {
        .application = application,
        .project = project,
        .root = root,
        .result = -1,
    };
    cbm_thread_t thread;
    bool thread_started = root_ok && application &&
                          cbm_thread_create(&thread, 0, app_index_thread,
                                            &request) == 0;
    bool boundary_started =
        thread_started && app_wait_for_atomic_int(&fake.starts, 1);
    bool worker_acquired =
        boundary_started &&
        app_wait_for_atomic_int(&fake.project_lock_acquisitions, 1);
    cbm_project_lock_lease_t *active_probe = NULL;
    cbm_private_file_lock_status_t active_probe_status =
        worker_acquired
            ? cbm_project_lock_try_acquire(locks.probe_locks, project,
                                           &active_probe)
            : CBM_PRIVATE_FILE_LOCK_IO;
    bool worker_holds_lock =
        active_probe_status == CBM_PRIVATE_FILE_LOCK_BUSY && !active_probe;
    bool active_probe_released = app_project_lock_release(&active_probe);
    atomic_store(&fake.allow_completion, true);
    if (thread_started) {
        (void)cbm_thread_join(&thread);
    }
    cbm_project_lock_lease_t *terminal_probe = NULL;
    cbm_private_file_lock_status_t terminal_probe_status =
        thread_started
            ? cbm_project_lock_try_acquire(locks.probe_locks, project,
                                           &terminal_probe)
            : CBM_PRIVATE_FILE_LOCK_IO;
    bool worker_released_lock =
        terminal_probe_status == CBM_PRIVATE_FILE_LOCK_OK && terminal_probe;
    bool terminal_probe_released = app_project_lock_release(&terminal_probe);

    bool stopped = application &&
                   cbm_daemon_application_shutdown(application,
                                                   APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    bool locks_clean = app_project_lock_fixture_cleanup(&locks);
    (void)th_rmtree(root);

    ASSERT_TRUE(locks_ready);
    ASSERT_TRUE(root_ok);
    ASSERT_TRUE(thread_started);
    ASSERT_TRUE(boundary_started);
    ASSERT_TRUE(worker_acquired);
    ASSERT_EQ(atomic_load(&fake.project_lock_attempts), 1);
    ASSERT_TRUE(worker_holds_lock);
    ASSERT_TRUE(active_probe_released);
    ASSERT_EQ(request.result, 0);
    ASSERT_TRUE(worker_released_lock);
    ASSERT_TRUE(terminal_probe_released);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(locks_clean);
    ASSERT_EQ(atomic_load(&fake.starts), 1);
    ASSERT_EQ(atomic_load(&fake.destroys), 1);
    PASS();
}

TEST(daemon_application_worker_lock_serializes_external_mutation) {
    app_project_lock_fixture_t locks;
    bool locks_ready = app_project_lock_fixture_init(&locks);
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    fake.project_locks = locks_ready ? locks.worker_locks : NULL;
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {
        .worker_ops = &worker_ops,
        .project_locks = locks_ready ? locks.daemon_locks : NULL,
    };
    cbm_daemon_application_t *application =
        locks_ready ? cbm_daemon_application_new(&config) : NULL;

    char blocked_root[APP_TEST_PATH_CAP];
    char other_root[APP_TEST_PATH_CAP];
    (void)snprintf(blocked_root, sizeof(blocked_root),
                   "%s/cbm-app-external-blocked-XXXXXX", cbm_tmpdir());
    (void)snprintf(other_root, sizeof(other_root),
                   "%s/cbm-app-external-other-XXXXXX", cbm_tmpdir());
    bool blocked_root_ok = cbm_mkdtemp(blocked_root) != NULL;
    bool other_root_ok = cbm_mkdtemp(other_root) != NULL;
    const char *blocked_project = "external-blocked";
    const char *other_project = "external-other";

    cbm_project_lock_lease_t *external_lease = NULL;
    bool external_lease_held =
        application && blocked_root_ok && other_root_ok &&
        cbm_project_lock_acquire(locks.external_locks, blocked_project,
                                 UINT64_MAX, NULL, &external_lease) ==
            CBM_PRIVATE_FILE_LOCK_OK &&
        external_lease;
    app_index_thread_t blocked_request = {
        .application = application,
        .project = blocked_project,
        .root = blocked_root,
        .result = -1,
    };
    cbm_thread_t blocked_thread;
    bool blocked_thread_started =
        external_lease_held &&
        cbm_thread_create(&blocked_thread, 0, app_index_thread,
                          &blocked_request) == 0;
    bool blocked_boundary_started =
        blocked_thread_started && app_wait_for_atomic_int(&fake.starts, 1);
    bool blocked_boundary_attempted =
        blocked_boundary_started &&
        app_wait_for_atomic_int_at_least(&fake.project_lock_attempts, 1);
    bool blocked_waiting = blocked_boundary_attempted &&
                           atomic_load(&fake.project_lock_acquisitions) == 0;

    app_index_thread_t other_request = {
        .application = application,
        .project = other_project,
        .root = other_root,
        .result = -1,
    };
    cbm_thread_t other_thread;
    bool other_thread_started =
        blocked_waiting &&
        cbm_thread_create(&other_thread, 0, app_index_thread,
                          &other_request) == 0;
    bool both_boundaries_started =
        other_thread_started && app_wait_for_atomic_int(&fake.starts, 2);
    bool other_worker_acquired =
        both_boundaries_started &&
        app_wait_for_atomic_int(&fake.project_lock_acquisitions, 1);

    cbm_project_lock_lease_t *other_probe = NULL;
    cbm_private_file_lock_status_t active_other_status =
        other_worker_acquired
            ? cbm_project_lock_try_acquire(locks.probe_locks, other_project,
                                           &other_probe)
            : CBM_PRIVATE_FILE_LOCK_IO;
    bool other_lock_held_while_worker_active =
        active_other_status == CBM_PRIVATE_FILE_LOCK_BUSY && !other_probe;
    bool active_other_probe_released = app_project_lock_release(&other_probe);

    atomic_store(&fake.allow_completion, true);
    if (other_thread_started && !other_worker_acquired && application) {
        (void)cbm_daemon_application_shutdown(application,
                                              APP_TEST_TIMEOUT_MS);
    }
    if (other_thread_started) {
        (void)cbm_thread_join(&other_thread);
    }
    cbm_project_lock_lease_t *terminal_other_probe = NULL;
    cbm_private_file_lock_status_t terminal_other_status =
        other_worker_acquired
            ? cbm_project_lock_try_acquire(locks.probe_locks, other_project,
                                           &terminal_other_probe)
            : CBM_PRIVATE_FILE_LOCK_IO;
    bool other_lock_released_at_terminal =
        terminal_other_status == CBM_PRIVATE_FILE_LOCK_OK &&
        terminal_other_probe;
    bool terminal_other_probe_released =
        app_project_lock_release(&terminal_other_probe);
    bool blocked_still_waiting =
        atomic_load(&fake.project_lock_acquisitions) == 1;

    bool external_lease_released = app_project_lock_release(&external_lease);
    bool blocked_worker_acquired =
        blocked_thread_started &&
        app_wait_for_atomic_int(&fake.project_lock_acquisitions, 2);
    if (blocked_thread_started && !blocked_worker_acquired && application) {
        (void)cbm_daemon_application_shutdown(application,
                                              APP_TEST_TIMEOUT_MS);
    }
    if (blocked_thread_started) {
        (void)cbm_thread_join(&blocked_thread);
    }
    cbm_project_lock_lease_t *blocked_probe = NULL;
    cbm_private_file_lock_status_t terminal_blocked_status =
        blocked_worker_acquired
            ? cbm_project_lock_try_acquire(locks.probe_locks,
                                           blocked_project, &blocked_probe)
            : CBM_PRIVATE_FILE_LOCK_IO;
    bool blocked_lock_released_at_terminal =
        terminal_blocked_status == CBM_PRIVATE_FILE_LOCK_OK && blocked_probe;
    bool blocked_probe_released = app_project_lock_release(&blocked_probe);

    bool stopped = application &&
                   cbm_daemon_application_shutdown(application,
                                                   APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    bool locks_clean = app_project_lock_fixture_cleanup(&locks);
    (void)th_rmtree(blocked_root);
    (void)th_rmtree(other_root);

    ASSERT_TRUE(locks_ready);
    ASSERT_TRUE(blocked_root_ok);
    ASSERT_TRUE(other_root_ok);
    ASSERT_TRUE(external_lease_held);
    ASSERT_TRUE(blocked_thread_started);
    ASSERT_TRUE(blocked_boundary_started);
    ASSERT_TRUE(blocked_boundary_attempted);
    ASSERT_TRUE(blocked_waiting);
    ASSERT_TRUE(other_thread_started);
    ASSERT_TRUE(both_boundaries_started);
    ASSERT_TRUE(other_worker_acquired);
    ASSERT_TRUE(other_lock_held_while_worker_active);
    ASSERT_TRUE(active_other_probe_released);
    ASSERT_EQ(other_request.result, 0);
    ASSERT_TRUE(other_lock_released_at_terminal);
    ASSERT_TRUE(terminal_other_probe_released);
    ASSERT_TRUE(blocked_still_waiting);
    ASSERT_TRUE(external_lease_released);
    ASSERT_TRUE(blocked_worker_acquired);
    ASSERT_EQ(blocked_request.result, 0);
    ASSERT_TRUE(blocked_lock_released_at_terminal);
    ASSERT_TRUE(blocked_probe_released);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(locks_clean);
    ASSERT_EQ(atomic_load(&fake.starts), 2);
    ASSERT_EQ(atomic_load(&fake.project_lock_acquisitions), 2);
    ASSERT_EQ(atomic_load(&fake.destroys), 2);
    PASS();
}

TEST(daemon_application_final_unsubscribe_cancels_external_lock_wait) {
    app_project_lock_fixture_t locks;
    bool locks_ready = app_project_lock_fixture_init(&locks);
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    fake.project_locks = locks_ready ? locks.worker_locks : NULL;
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {
        .worker_ops = &worker_ops,
        .project_locks = locks_ready ? locks.daemon_locks : NULL,
    };
    cbm_daemon_application_t *application =
        locks_ready ? cbm_daemon_application_new(&config) : NULL;
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session =
        application ? app_test_open(&callbacks, 37) : NULL;

    char root[APP_TEST_PATH_CAP];
    (void)snprintf(root, sizeof(root),
                   "%s/cbm-app-external-cancel-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    char *project = root_ok ? cbm_project_name_from_path(root) : NULL;
    cbm_project_lock_lease_t *external_lease = NULL;
    bool external_lease_held =
        session && project &&
        cbm_project_lock_acquire(locks.external_locks, project, UINT64_MAX,
                                 NULL, &external_lease) ==
            CBM_PRIVATE_FILE_LOCK_OK &&
        external_lease;

    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char args[APP_TEST_PATH_CAP + 32];
    (void)snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    bool setup = external_lease_held &&
                 app_test_context_request(root, root, &context,
                                          &context_length) &&
                 app_test_tool_request("index_repository", args, &tool,
                                       &tool_length);
    uint8_t *empty = NULL;
    uint32_t empty_length = 0;
    if (setup) {
        setup = app_test_request(&callbacks, session, context, context_length,
                                 &empty, &empty_length) ==
                CBM_DAEMON_RUNTIME_APPLICATION_OK;
    }
    free(empty);

    app_request_thread_t request = {
        .callbacks = callbacks,
        .session = session,
        .request = tool,
        .request_length = tool_length,
    };
    atomic_init(&request.done, false);
    cbm_thread_t request_thread;
    bool request_started =
        setup && cbm_thread_create(&request_thread, 0, app_request_thread,
                                   &request) == 0;
    bool subscribed = request_started &&
                      app_wait_for_subscribers(application, project, 1);
    bool worker_boundary_started =
        subscribed && app_wait_for_atomic_int(&fake.starts, 1);
    bool worker_lock_attempted =
        worker_boundary_started &&
        app_wait_for_atomic_int_at_least(&fake.project_lock_attempts, 1);
    bool worker_waiting = worker_lock_attempted &&
                          atomic_load(&fake.project_lock_acquisitions) == 0;
    if (subscribed) {
        callbacks.session_cancel(callbacks.context, session);
    }
    bool final_unsubscribed =
        subscribed &&
        app_wait_for_subscribers(application, project, 0);
    uint64_t request_deadline = cbm_now_ms() + APP_TEST_TIMEOUT_MS;
    while (request_started && !atomic_load(&request.done) &&
           cbm_now_ms() < request_deadline) {
        cbm_usleep(1000);
    }
    bool request_finished = request_started && atomic_load(&request.done);
    bool job_terminal =
        final_unsubscribed && app_wait_for_active_jobs(application, 0);
    if (request_started) {
        (void)cbm_thread_join(&request_thread);
    }
    bool cancellation_reported =
        request.status == CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED &&
        request.response == NULL && request.response_length == 0;
    bool worker_never_acquired =
        atomic_load(&fake.project_lock_acquisitions) == 0;

    bool external_lease_released = app_project_lock_release(&external_lease);
    cbm_project_lock_lease_t *leak_probe = NULL;
    bool can_probe_for_leak = job_terminal && external_lease_held &&
                              external_lease_released && locks.probe_locks &&
                              project;
    cbm_private_file_lock_status_t leak_probe_status =
        can_probe_for_leak
            ? cbm_project_lock_try_acquire(locks.probe_locks, project,
                                           &leak_probe)
            : CBM_PRIVATE_FILE_LOCK_IO;
    bool no_daemon_lease_leaked =
        leak_probe_status == CBM_PRIVATE_FILE_LOCK_OK && leak_probe;
    bool leak_probe_released = app_project_lock_release(&leak_probe);

    if (session) {
        callbacks.session_close(callbacks.context, session);
    }
    bool stopped = application &&
                   cbm_daemon_application_shutdown(application,
                                                   APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);
    bool locks_clean = app_project_lock_fixture_cleanup(&locks);

    free(request.response);
    free(context);
    free(tool);
    free(project);
    (void)th_rmtree(root);

    ASSERT_TRUE(locks_ready);
    ASSERT_TRUE(root_ok);
    ASSERT_TRUE(external_lease_held);
    ASSERT_TRUE(setup);
    ASSERT_TRUE(request_started);
    ASSERT_TRUE(subscribed);
    ASSERT_TRUE(worker_boundary_started);
    ASSERT_TRUE(worker_lock_attempted);
    ASSERT_TRUE(worker_waiting);
    ASSERT_TRUE(final_unsubscribed);
    ASSERT_TRUE(request_finished);
    ASSERT_TRUE(job_terminal);
    ASSERT_EQ(request.status, CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED);
    ASSERT_TRUE(cancellation_reported);
    ASSERT_TRUE(worker_never_acquired);
    ASSERT_EQ(atomic_load(&fake.starts), 1);
    ASSERT_EQ(atomic_load(&fake.cancels), 1);
    ASSERT_EQ(atomic_load(&fake.destroys), 1);
    ASSERT_TRUE(external_lease_released);
    ASSERT_TRUE(no_daemon_lease_leaked);
    ASSERT_TRUE(leak_probe_released);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(locks_clean);
    PASS();
}

TEST(daemon_application_cancels_mutation_wait_without_cancelling_shared_job) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *index_session = app_test_open(&callbacks, 35);
    cbm_daemon_runtime_application_session_t *mutation_session = app_test_open(&callbacks, 36);
    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-mutation-cancel-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    char *project = root_ok ? cbm_project_name_from_path(root) : NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char index_args[APP_TEST_PATH_CAP + 32];
    snprintf(index_args, sizeof(index_args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *index_tool = NULL;
    uint32_t index_tool_length = 0;
    char mutation_args[APP_TEST_PATH_CAP + 96];
    snprintf(mutation_args, sizeof(mutation_args),
             "{\"project\":\"%s\",\"mode\":\"update\",\"content\":\"x\"}",
             project ? project : "");
    uint8_t *mutation_tool = NULL;
    uint32_t mutation_tool_length = 0;
    bool setup = application && index_session && mutation_session && project &&
                 app_test_context_request(root, root, &context, &context_length) &&
                 app_test_tool_request("index_repository", index_args, &index_tool,
                                       &index_tool_length) &&
                 app_test_tool_request("manage_adr", mutation_args, &mutation_tool,
                                       &mutation_tool_length);
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    cbm_daemon_runtime_application_session_t *sessions[2] = {index_session, mutation_session};
    for (size_t i = 0; setup && i < 2; i++) {
        setup = app_test_request(&callbacks, sessions[i], context, context_length, &response,
                                 &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
        free(response);
        response = NULL;
    }

    app_request_thread_t index_request = {
        .callbacks = callbacks,
        .session = index_session,
        .request = index_tool,
        .request_length = index_tool_length,
    };
    app_request_thread_t mutation_request = {
        .callbacks = callbacks,
        .session = mutation_session,
        .request = mutation_tool,
        .request_length = mutation_tool_length,
    };
    cbm_thread_t index_thread;
    cbm_thread_t mutation_thread;
    bool index_started = setup &&
                         cbm_thread_create(&index_thread, 0, app_request_thread,
                                           &index_request) == 0;
    bool index_active = index_started &&
                        app_wait_for_subscribers(application, project, 1) &&
                        app_wait_for_atomic_int(&fake.starts, 1);
    bool mutation_started = index_active &&
                            cbm_thread_create(&mutation_thread, 0, app_request_thread,
                                              &mutation_request) == 0;
    uint64_t blocked_deadline = cbm_now_ms() + 100;
    while (mutation_started && !atomic_load(&mutation_request.done) &&
           cbm_now_ms() < blocked_deadline) {
        cbm_usleep(1000);
    }
    bool initially_blocked = mutation_started && !atomic_load(&mutation_request.done);
    if (initially_blocked) {
        callbacks.session_cancel(callbacks.context, mutation_session);
    }
    uint64_t cancel_deadline = cbm_now_ms() + APP_TEST_TIMEOUT_MS;
    while (mutation_started && !atomic_load(&mutation_request.done) &&
           cbm_now_ms() < cancel_deadline) {
        cbm_usleep(1000);
    }
    bool cancelled_wait_finished = mutation_started && atomic_load(&mutation_request.done);
    int physical_cancels_before_release = atomic_load(&fake.cancels);
    atomic_store(&fake.allow_completion, true);
    if (mutation_started) {
        (void)cbm_thread_join(&mutation_thread);
    }
    if (index_started) {
        (void)cbm_thread_join(&index_thread);
    }
    bool cancellation_reported =
        mutation_request.status == CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED &&
        mutation_request.response == NULL &&
        mutation_request.response_length == 0;
    callbacks.session_close(callbacks.context, index_session);
    callbacks.session_close(callbacks.context, mutation_session);
    bool stopped = application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);

    free(index_request.response);
    free(mutation_request.response);
    free(context);
    free(index_tool);
    free(mutation_tool);
    free(project);
    (void)cbm_rmdir(root);

    ASSERT_TRUE(setup);
    ASSERT_TRUE(index_active);
    ASSERT_TRUE(mutation_started);
    ASSERT_TRUE(initially_blocked);
    ASSERT_TRUE(cancelled_wait_finished);
    ASSERT_TRUE(cancellation_reported);
    ASSERT_EQ(physical_cancels_before_release, 0);
    ASSERT_EQ(index_request.status, CBM_DAEMON_RUNTIME_APPLICATION_OK);
    ASSERT_TRUE(stopped);
    PASS();
}

TEST(daemon_application_recovers_with_unique_per_job_quarantine_files) {
    const char *old_restarts = getenv("CBM_INDEX_MAX_RESTARTS");
    char *saved_restarts = old_restarts ? cbm_strdup(old_restarts) : NULL;
    bool saved_ok = !old_restarts || saved_restarts;
    const char *old_cache = getenv("CBM_CACHE_DIR");
    bool had_cache = old_cache != NULL;
    char *saved_cache = old_cache ? cbm_strdup(old_cache) : NULL;
    bool saved_cache_ok = !had_cache || saved_cache;
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    atomic_store(&fake.scripted, true);
    for (int base = 0; base <= 4; base += 4) {
        fake.outcomes[base] = CBM_PROC_CRASH;
        fake.outcomes[base + 1] = CBM_PROC_CRASH;
        fake.outcomes[base + 2] = CBM_PROC_CRASH;
        fake.outcomes[base + 3] = CBM_PROC_CLEAN;
        fake.marker_payloads[base + 1] = "S crashing.c\n";
        fake.marker_payloads[base + 2] = "S crashing.c\n";
    }
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    char root[APP_TEST_PATH_CAP];
    char cache[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-recovery-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-app-recovery-cache-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    bool cache_ok = cbm_mkdtemp(cache) != NULL;
    bool env_ok = saved_ok && saved_cache_ok && cache_ok &&
                  cbm_setenv("CBM_INDEX_MAX_RESTARTS", "3", 1) == 0 &&
                  cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0;

    int first = application && root_ok && env_ok
                    ? cbm_daemon_application_index(application, "recovery-one", root)
                    : -1;
    int second = application && root_ok && env_ok
                     ? cbm_daemon_application_index(application, "recovery-two", root)
                     : -1;
    bool stopped = application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    bool first_marker_removed = !cbm_file_exists(fake.marker_paths[1]);
    bool first_quarantine_removed = !cbm_file_exists(fake.quarantine_paths[1]);
    bool second_marker_removed = !cbm_file_exists(fake.marker_paths[5]);
    bool second_quarantine_removed = !cbm_file_exists(fake.quarantine_paths[5]);
    cbm_daemon_application_free(application);
    if (saved_restarts) {
        (void)cbm_setenv("CBM_INDEX_MAX_RESTARTS", saved_restarts, 1);
    } else if (!old_restarts) {
        (void)cbm_unsetenv("CBM_INDEX_MAX_RESTARTS");
    }
    free(saved_restarts);
    if (saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
    } else if (!had_cache) {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(saved_cache);

    ASSERT_TRUE(env_ok);
    ASSERT_TRUE(root_ok);
    ASSERT_TRUE(cache_ok);
    ASSERT_EQ(first, 0);
    ASSERT_EQ(second, 0);
    ASSERT_TRUE(stopped);
    ASSERT_EQ(atomic_load(&fake.starts), 8);
    ASSERT_EQ(atomic_load(&fake.destroys), 8);
    ASSERT_STR_EQ(fake.marker_paths[0], "");
    ASSERT_STR_EQ(fake.quarantine_paths[0], "");
    ASSERT_TRUE(fake.marker_paths[1][0] != '\0');
    ASSERT_TRUE(fake.quarantine_paths[1][0] != '\0');
    ASSERT_STR_EQ(fake.marker_paths[1], fake.marker_paths[2]);
    ASSERT_STR_EQ(fake.marker_paths[2], fake.marker_paths[3]);
    ASSERT_STR_EQ(fake.quarantine_paths[1], fake.quarantine_paths[3]);
    ASSERT_STR_NEQ(fake.marker_paths[1], fake.quarantine_paths[1]);
    ASSERT_STR_NEQ(fake.marker_paths[1], fake.marker_paths[5]);
    ASSERT_STR_NEQ(fake.quarantine_paths[1], fake.quarantine_paths[5]);
    ASSERT_TRUE(strstr(fake.quarantine_seen[3], "crashing.c\tcrash\n") != NULL);
    ASSERT_TRUE(strstr(fake.quarantine_seen[7], "crashing.c\tcrash\n") != NULL);
    ASSERT_TRUE(first_marker_removed);
    ASSERT_TRUE(first_quarantine_removed);
    ASSERT_TRUE(second_marker_removed);
    ASSERT_TRUE(second_quarantine_removed);

    (void)cbm_rmdir(root);
    char logs[APP_TEST_PATH_CAP];
    snprintf(logs, sizeof(logs), "%s/logs", cache);
    (void)cbm_rmdir(logs);
    (void)cbm_rmdir(cache);
    PASS();
}

TEST(daemon_application_cancellation_between_recovery_attempts_stops_retry) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    atomic_store(&fake.scripted, true);
    atomic_store(&fake.hold_destroy_attempt, 0);
    fake.outcomes[0] = CBM_PROC_CRASH;
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session = app_test_open(&callbacks, 41);
    char root[APP_TEST_PATH_CAP];
    snprintf(root, sizeof(root), "%s/cbm-app-retry-cancel-XXXXXX", cbm_tmpdir());
    bool root_ok = cbm_mkdtemp(root) != NULL;
    uint8_t *context = NULL;
    uint32_t context_length = 0;
    char args[APP_TEST_PATH_CAP + 32];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    bool setup = application && session && root_ok &&
                 app_test_context_request(root, root, &context, &context_length) &&
                 app_test_tool_request("index_repository", args, &tool, &tool_length);
    uint8_t *empty = NULL;
    uint32_t empty_length = 0;
    if (setup) {
        setup = app_test_request(&callbacks, session, context, context_length, &empty,
                                 &empty_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
    }
    free(empty);
    app_request_thread_t request = {
        .callbacks = callbacks,
        .session = session,
        .request = tool,
        .request_length = tool_length,
    };
    cbm_thread_t thread;
    bool started = setup && cbm_thread_create(&thread, 0, app_request_thread, &request) == 0;
    bool between_attempts = started && app_wait_for_atomic_int(&fake.destroys, 1);
    if (between_attempts) {
        callbacks.session_cancel(callbacks.context, session);
    }
    atomic_store(&fake.release_destroy, true);
    if (started) {
        (void)cbm_thread_join(&thread);
    }
    callbacks.session_close(callbacks.context, session);
    bool stopped = application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);

    ASSERT_TRUE(setup);
    ASSERT_TRUE(started);
    ASSERT_TRUE(between_attempts);
    ASSERT_TRUE(stopped);
    ASSERT_EQ(atomic_load(&fake.starts), 1);
    ASSERT_EQ(atomic_load(&fake.destroys), 1);
    ASSERT_EQ(request.status, CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED);
    ASSERT_NULL(request.response);
    ASSERT_EQ(request.response_length, 0);

    free(request.response);
    free(context);
    free(tool);
    (void)cbm_rmdir(root);
    PASS();
}

TEST(daemon_application_enforces_global_physical_job_limit) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {
        .worker_ops = &worker_ops,
        .physical_job_limit = 1,
    };
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    char first_root[APP_TEST_PATH_CAP];
    char second_root[APP_TEST_PATH_CAP];
    snprintf(first_root, sizeof(first_root), "%s/cbm-app-cap-first-XXXXXX", cbm_tmpdir());
    snprintf(second_root, sizeof(second_root), "%s/cbm-app-cap-second-XXXXXX", cbm_tmpdir());
    bool roots_ok = cbm_mkdtemp(first_root) != NULL && cbm_mkdtemp(second_root) != NULL;
    app_index_thread_t first = {
        .application = application,
        .project = "cap-first",
        .root = first_root,
        .result = -1,
    };
    cbm_thread_t thread;
    bool started =
        application && roots_ok && cbm_thread_create(&thread, 0, app_index_thread, &first) == 0;
    bool admitted = started && app_wait_for_subscribers(application, "cap-first", 1) &&
                    app_wait_for_atomic_int(&fake.starts, 1);
    int busy = admitted ? cbm_daemon_application_index(application, "cap-second", second_root) : -1;
    cbm_daemon_runtime_application_callbacks_t callbacks =
        cbm_daemon_application_runtime_callbacks(application);
    cbm_daemon_runtime_application_session_t *session = app_test_open(&callbacks, 51);
    uint8_t *session_context = NULL;
    uint32_t session_context_length = 0;
    char args[APP_TEST_PATH_CAP + 32];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", second_root);
    uint8_t *tool = NULL;
    uint32_t tool_length = 0;
    bool session_encoded = admitted && session &&
                           app_test_context_request(second_root, second_root, &session_context,
                                                    &session_context_length) &&
                           app_test_tool_request("index_repository", args, &tool, &tool_length);
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    bool session_busy =
        session_encoded &&
        app_test_request(&callbacks, session, session_context, session_context_length, &response,
                         &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK;
    free(response);
    response = NULL;
    session_busy = session_busy &&
                   app_test_request(&callbacks, session, tool, tool_length, &response,
                                    &response_length) == CBM_DAEMON_RUNTIME_APPLICATION_OK &&
                   response && strstr((char *)response, "physical index job limit reached");
    callbacks.session_close(callbacks.context, session);
    bool stopped = application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    if (started) {
        (void)cbm_thread_join(&thread);
    }
    cbm_daemon_application_free(application);
    free(session_context);
    free(tool);
    free(response);

    ASSERT_TRUE(roots_ok);
    ASSERT_TRUE(started);
    ASSERT_TRUE(admitted);
    ASSERT_EQ(busy, 1);
    ASSERT_TRUE(session_busy);
    ASSERT_TRUE(stopped);
    ASSERT_EQ(atomic_load(&fake.starts), 1);
    ASSERT_EQ(atomic_load(&fake.cancels), 1);
    ASSERT_EQ(atomic_load(&fake.destroys), 1);

    (void)cbm_rmdir(first_root);
    (void)cbm_rmdir(second_root);
    PASS();
}

TEST(daemon_application_default_limit_admits_four_and_rejects_fifth) {
    enum { DEFAULT_CAP_RUNNING = 4, DEFAULT_CAP_TOTAL = 5 };
    const size_t aggregate_budget = 4099;
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {
        .worker_ops = &worker_ops,
        .aggregate_memory_budget_bytes = aggregate_budget,
    };
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);
    char roots[DEFAULT_CAP_TOTAL][APP_TEST_PATH_CAP];
    char projects[DEFAULT_CAP_TOTAL][32];
    bool roots_ok = true;
    for (int i = 0; i < DEFAULT_CAP_TOTAL; i++) {
        (void)snprintf(roots[i], sizeof(roots[i]), "%s/cbm-app-default-cap-%d-XXXXXX", cbm_tmpdir(),
                       i);
        (void)snprintf(projects[i], sizeof(projects[i]), "default-cap-%d", i);
        roots_ok = roots_ok && cbm_mkdtemp(roots[i]) != NULL;
    }

    app_index_thread_t requests[DEFAULT_CAP_RUNNING];
    cbm_thread_t threads[DEFAULT_CAP_RUNNING];
    bool started[DEFAULT_CAP_RUNNING] = {false};
    for (int i = 0; application && roots_ok && i < DEFAULT_CAP_RUNNING; i++) {
        requests[i] = (app_index_thread_t){
            .application = application,
            .project = projects[i],
            .root = roots[i],
            .result = -1,
        };
        started[i] = cbm_thread_create(&threads[i], 0, app_index_thread, &requests[i]) == 0;
    }
    bool all_started = true;
    for (int i = 0; i < DEFAULT_CAP_RUNNING; i++) {
        all_started = all_started && started[i];
    }
    bool four_admitted = all_started &&
                         app_wait_for_active_jobs(application, DEFAULT_CAP_RUNNING) &&
                         app_wait_for_atomic_int(&fake.starts, DEFAULT_CAP_RUNNING);
    int fifth =
        four_admitted ? cbm_daemon_application_index(application, projects[4], roots[4]) : -1;
    bool stopped = application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    for (int i = 0; i < DEFAULT_CAP_RUNNING; i++) {
        if (started[i]) {
            (void)cbm_thread_join(&threads[i]);
        }
    }
    cbm_daemon_application_free(application);
    for (int i = 0; i < DEFAULT_CAP_TOTAL; i++) {
        (void)cbm_rmdir(roots[i]);
    }

    ASSERT_TRUE(roots_ok);
    ASSERT_TRUE(all_started);
    ASSERT_TRUE(four_admitted);
    ASSERT_EQ(fifth, 1);
    ASSERT_TRUE(stopped);
    ASSERT_EQ(atomic_load(&fake.starts), DEFAULT_CAP_RUNNING);
    ASSERT_EQ(atomic_load(&fake.cancels), DEFAULT_CAP_RUNNING);
    ASSERT_EQ(atomic_load(&fake.destroys), DEFAULT_CAP_RUNNING);
    size_t assigned_budget = 0;
    for (int i = 0; i < DEFAULT_CAP_RUNNING; i++) {
        ASSERT_EQ(fake.memory_budgets[i], aggregate_budget / DEFAULT_CAP_RUNNING);
        assigned_budget += fake.memory_budgets[i];
    }
    ASSERT_TRUE(assigned_budget <= aggregate_budget);
    PASS();
}

TEST(daemon_application_rejects_clean_exit_when_process_tree_is_not_contained) {
    app_fake_worker_context_t fake;
    app_fake_worker_context_init(&fake);
    atomic_store(&fake.allow_completion, true);
    atomic_store(&fake.unsafe_clean, true);
    cbm_daemon_application_worker_ops_t worker_ops = {
        .context = &fake,
        .start = app_fake_worker_start,
        .poll = app_fake_worker_poll,
        .cancel = app_fake_worker_cancel,
        .log_path = app_fake_worker_log_path,
        .destroy = app_fake_worker_destroy,
    };
    cbm_daemon_application_config_t config = {.worker_ops = &worker_ops};
    cbm_daemon_application_t *application = cbm_daemon_application_new(&config);

    int result = application
                     ? cbm_daemon_application_index(
                           application, "unsafe-clean-project", cbm_tmpdir())
                     : -1;
    bool stopped = application && cbm_daemon_application_shutdown(application, APP_TEST_TIMEOUT_MS);
    cbm_daemon_application_free(application);

    ASSERT_NOT_NULL(application);
    ASSERT_LT(result, 0);
    ASSERT_TRUE(stopped);
    ASSERT_EQ(atomic_load(&fake.starts), 1);
    ASSERT_EQ(atomic_load(&fake.destroys), 1);
    PASS();
}

SUITE(daemon_application) {
    RUN_TEST(daemon_application_new_session_does_not_retain_initial_store);
    RUN_TEST(daemon_application_request_cancel_is_scoped_to_exact_token);
    RUN_TEST(daemon_application_requires_immutable_explicit_context);
    RUN_TEST(daemon_application_ui_config_updates_are_masked_and_serialized);
    RUN_TEST(daemon_application_ui_config_rejects_noncanonical_frames);
    RUN_TEST(daemon_application_restricted_profile_owns_no_background_surfaces);
    RUN_TEST(daemon_application_hook_context_preserves_event_and_dialect);
    RUN_TEST(daemon_application_mcp_notification_has_no_response);
    RUN_TEST(daemon_application_reference_counts_one_shared_watch);
    RUN_TEST(daemon_application_free_releases_live_watch_once);
    RUN_TEST(daemon_application_prune_clears_logical_watch_for_reregistration);
    RUN_TEST(daemon_application_coalesces_identical_index_requests);
    RUN_TEST(daemon_application_fresh_request_does_not_reuse_terminal_subscribed_job);
    RUN_TEST(daemon_application_request_cancel_detaches_only_one_coalesced_subscriber);
    RUN_TEST(daemon_application_cancels_physical_job_only_after_final_session);
    RUN_TEST(daemon_application_disconnect_before_request_callback_is_sticky);
    RUN_TEST(daemon_application_request_cancel_preserves_persistent_watch_and_session);
    RUN_TEST(daemon_application_cancel_drops_watch_before_inflight_request_returns);
    RUN_TEST(daemon_application_stale_watcher_callback_is_rejected_at_job_admission);
    RUN_TEST(daemon_application_final_cancel_drains_admitted_watcher_job);
    RUN_TEST(daemon_application_watcher_job_follows_exact_live_watch_owners);
    RUN_TEST(daemon_application_serializes_adr_mutation_with_index_job);
    RUN_TEST(daemon_application_reserved_mutation_delays_worker_start);
    RUN_TEST(daemon_application_mutation_does_not_block_other_project);
    RUN_TEST(daemon_application_mutation_still_owns_os_project_lock);
    RUN_TEST(daemon_application_worker_boundary_is_sole_os_project_lock_owner);
    RUN_TEST(daemon_application_worker_lock_serializes_external_mutation);
    RUN_TEST(daemon_application_final_unsubscribe_cancels_external_lock_wait);
    RUN_TEST(daemon_application_cancels_mutation_wait_without_cancelling_shared_job);
    RUN_TEST(daemon_application_recovers_with_unique_per_job_quarantine_files);
    RUN_TEST(daemon_application_cancellation_between_recovery_attempts_stops_retry);
    RUN_TEST(daemon_application_enforces_global_physical_job_limit);
    RUN_TEST(daemon_application_default_limit_admits_four_and_rejects_fifth);
    RUN_TEST(daemon_application_rejects_clean_exit_when_process_tree_is_not_contained);
}
