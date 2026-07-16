/*
 * test_cross_repo.c — Input, work-bound, and write-failure guards for the
 * cross-repository matching pass.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat.h"
#include "foundation/compat_thread.h"
#include "pipeline/pass_cross_repo.h"

#include <sqlite3/sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

typedef struct {
    char cache[256];
    char *saved_cache;
} cross_repo_fixture_t;

static bool cross_repo_fixture_begin(cross_repo_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    const char *saved = getenv("CBM_CACHE_DIR");
    if (saved) {
        fixture->saved_cache = strdup(saved);
        if (!fixture->saved_cache) {
            return false;
        }
    }
    snprintf(fixture->cache, sizeof(fixture->cache), "/tmp/cbm-cross-hardening-XXXXXX");
    return cbm_mkdtemp(fixture->cache) != NULL &&
           cbm_setenv("CBM_CACHE_DIR", fixture->cache, 1) == 0;
}

static void cross_repo_fixture_end(cross_repo_fixture_t *fixture) {
    if (fixture->saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", fixture->saved_cache, 1);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    if (fixture->cache[0]) {
        th_rmtree(fixture->cache);
    }
    free(fixture->saved_cache);
    memset(fixture, 0, sizeof(*fixture));
}

static bool cross_repo_project_path(const cross_repo_fixture_t *fixture, const char *project,
                                    char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s/%s.db", fixture->cache, project);
    return written > 0 && (size_t)written < out_size;
}

static bool cross_repo_create_project(const cross_repo_fixture_t *fixture, const char *project) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return false;
    }
    bool ok = cbm_store_upsert_project(store, project, fixture->cache) == CBM_STORE_OK;
    cbm_store_close(store);
    return ok;
}

/* Seed one HTTP_CALLS/HANDLES pair into two exact project stores. The suffix
 * keeps node QNs unique when a source is linked to more than one target. */
static bool cross_repo_seed_http_pair(const cross_repo_fixture_t *fixture,
                                      const char *source_project, const char *target_project,
                                      const char *route_path, const char *suffix) {
    char source_path[512];
    char target_path[512];
    if (!cross_repo_project_path(fixture, source_project, source_path, sizeof(source_path)) ||
        !cross_repo_project_path(fixture, target_project, target_path, sizeof(target_path))) {
        return false;
    }
    cbm_store_t *source = cbm_store_open_path(source_path);
    cbm_store_t *target = cbm_store_open_path(target_path);
    if (!source || !target) {
        cbm_store_close(source);
        cbm_store_close(target);
        return false;
    }

    bool ok = cbm_store_upsert_project(source, source_project, fixture->cache) == CBM_STORE_OK &&
              cbm_store_upsert_project(target, target_project, fixture->cache) == CBM_STORE_OK;
    char caller_qn[256];
    char local_route_qn[256];
    char target_route_qn[256];
    char handler_qn[256];
    char route_name[128];
    char edge_props[256];
    snprintf(caller_qn, sizeof(caller_qn), "%s.call.%s", source_project, suffix);
    snprintf(local_route_qn, sizeof(local_route_qn), "%s.local-route.%s", source_project, suffix);
    snprintf(target_route_qn, sizeof(target_route_qn), "__route__GET__%s", route_path);
    snprintf(handler_qn, sizeof(handler_qn), "%s.handle.%s", target_project, suffix);
    snprintf(route_name, sizeof(route_name), "GET %s", route_path);
    snprintf(edge_props, sizeof(edge_props), "{\"url_path\":\"%s\",\"method\":\"GET\"}",
             route_path);

    cbm_node_t caller = {.project = source_project,
                         .label = "Function",
                         .name = "call_remote",
                         .qualified_name = caller_qn,
                         .file_path = "client.c"};
    cbm_node_t local_route = {.project = source_project,
                              .label = "Route",
                              .name = route_name,
                              .qualified_name = local_route_qn,
                              .file_path = "client.c"};
    int64_t caller_id = ok ? cbm_store_upsert_node(source, &caller) : 0;
    int64_t local_route_id = ok ? cbm_store_upsert_node(source, &local_route) : 0;
    cbm_edge_t http_call = {.project = source_project,
                            .source_id = caller_id,
                            .target_id = local_route_id,
                            .type = "HTTP_CALLS",
                            .properties_json = edge_props};
    ok = ok && caller_id > 0 && local_route_id > 0 && cbm_store_insert_edge(source, &http_call) > 0;

    cbm_node_t target_route = {.project = target_project,
                               .label = "Route",
                               .name = route_name,
                               .qualified_name = target_route_qn,
                               .file_path = "server.c"};
    cbm_node_t handler = {.project = target_project,
                          .label = "Function",
                          .name = "handle_remote",
                          .qualified_name = handler_qn,
                          .file_path = "server.c"};
    int64_t target_route_id = ok ? cbm_store_upsert_node(target, &target_route) : 0;
    int64_t handler_id = ok ? cbm_store_upsert_node(target, &handler) : 0;
    cbm_edge_t handles = {.project = target_project,
                          .source_id = handler_id,
                          .target_id = target_route_id,
                          .type = "HANDLES"};
    ok = ok && target_route_id > 0 && handler_id > 0 && cbm_store_insert_edge(target, &handles) > 0;

    cbm_store_close(source);
    cbm_store_close(target);
    return ok;
}

static bool cross_repo_exec(const cross_repo_fixture_t *fixture, const char *project,
                            const char *sql) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    cbm_store_t *store = cbm_store_open_path_existing(path);
    if (!store) {
        return false;
    }
    char *error = NULL;
    int rc = sqlite3_exec(cbm_store_get_db(store), sql, NULL, NULL, &error);
    sqlite3_free(error);
    cbm_store_close(store);
    return rc == SQLITE_OK;
}

static int cross_repo_count_edges(const cross_repo_fixture_t *fixture, const char *project,
                                  const char *edge_type) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return -1;
    }
    cbm_store_t *store = cbm_store_open_path_query(path);
    if (!store) {
        return -1;
    }
    int count = cbm_store_count_edges_by_type(store, project, edge_type);
    cbm_store_close(store);
    return count;
}

TEST(cross_repo_null_target_fails_without_dereference) {
    cross_repo_fixture_t fixture;
    if (!cross_repo_fixture_begin(&fixture) ||
        !cross_repo_create_project(&fixture, "null-target-source")) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to create isolated source project");
    }

    bool rejected = false;
#if defined(_WIN32)
    const char *targets[] = {NULL};
    cbm_cross_repo_result_t result = cbm_cross_repo_match("null-target-source", targets, 1);
    rejected = result.failed;
#else
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        const char *targets[] = {NULL};
        cbm_cross_repo_result_t result = cbm_cross_repo_match("null-target-source", targets, 1);
        _exit(result.failed ? 0 : 2);
    }
    int status = 0;
    rejected = child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0;
#endif

    cross_repo_fixture_end(&fixture);
    ASSERT_TRUE(rejected);
    PASS();
}

TEST(cross_repo_wildcard_keeps_projects_containing_internal_tokens) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "wildcard-source", "orders_config_service",
                                           "/config-orders", "a") &&
                 cross_repo_seed_http_pair(&fixture, "wildcard-source", "orders_cross_repo_service",
                                           "/cross-orders", "b") &&
                 cross_repo_seed_http_pair(&fixture, "wildcard-source", "orders-wal-service",
                                           "/wal-orders", "c") &&
                 cross_repo_seed_http_pair(&fixture, "wildcard-source", "orders-shm-service",
                                           "/shm-orders", "d");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed wildcard fixture");
    }

    const char *targets[] = {"*"};
    cbm_cross_repo_result_t result = cbm_cross_repo_match("wildcard-source", targets, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 4);
    ASSERT_EQ(result.http_edges, 4);
    PASS();
}

static bool cross_repo_seed_bounded_scan(const cross_repo_fixture_t *fixture,
                                         const char *source_project, const char *target_project) {
    enum { TEST_SCAN_ROWS = 4097 };
    char source_path[512];
    char target_path[512];
    if (!cross_repo_project_path(fixture, source_project, source_path, sizeof(source_path)) ||
        !cross_repo_project_path(fixture, target_project, target_path, sizeof(target_path))) {
        return false;
    }
    cbm_store_t *source = cbm_store_open_path(source_path);
    cbm_store_t *target = cbm_store_open_path(target_path);
    if (!source || !target) {
        cbm_store_close(source);
        cbm_store_close(target);
        return false;
    }
    bool ok = cbm_store_upsert_project(source, source_project, fixture->cache) == CBM_STORE_OK &&
              cbm_store_upsert_project(target, target_project, fixture->cache) == CBM_STORE_OK;
    cbm_node_t caller = {.project = source_project,
                         .label = "Function",
                         .name = "bounded_caller",
                         .qualified_name = "bounded.source.caller",
                         .file_path = "client.c"};
    int64_t caller_id = ok ? cbm_store_upsert_node(source, &caller) : 0;
    ok = ok && caller_id > 0 &&
         sqlite3_exec(cbm_store_get_db(source), "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    for (int i = 0; ok && i < TEST_SCAN_ROWS; i++) {
        char name[64];
        char qn[96];
        snprintf(name, sizeof(name), "local_route_%d", i);
        snprintf(qn, sizeof(qn), "bounded.source.route.%d", i);
        cbm_node_t local_route = {.project = source_project,
                                  .label = "Route",
                                  .name = name,
                                  .qualified_name = qn,
                                  .file_path = "client.c"};
        int64_t route_id = cbm_store_upsert_node(source, &local_route);
        cbm_edge_t edge = {
            .project = source_project,
            .source_id = caller_id,
            .target_id = route_id,
            .type = "HTTP_CALLS",
            .properties_json = i == TEST_SCAN_ROWS - 1
                                   ? "{\"url_path\":\"/after-bound\",\"method\":\"GET\"}"
                                   : "{}",
        };
        ok = route_id > 0 && cbm_store_insert_edge(source, &edge) > 0;
    }
    if (ok) {
        ok = sqlite3_exec(cbm_store_get_db(source), "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    } else {
        (void)sqlite3_exec(cbm_store_get_db(source), "ROLLBACK", NULL, NULL, NULL);
    }

    cbm_node_t target_route = {.project = target_project,
                               .label = "Route",
                               .name = "GET /after-bound",
                               .qualified_name = "__route__GET__/after-bound",
                               .file_path = "server.c"};
    cbm_node_t handler = {.project = target_project,
                          .label = "Function",
                          .name = "bounded_handler",
                          .qualified_name = "bounded.target.handler",
                          .file_path = "server.c"};
    int64_t target_route_id = ok ? cbm_store_upsert_node(target, &target_route) : 0;
    int64_t handler_id = ok ? cbm_store_upsert_node(target, &handler) : 0;
    cbm_edge_t handles = {.project = target_project,
                          .source_id = handler_id,
                          .target_id = target_route_id,
                          .type = "HANDLES"};
    ok = ok && target_route_id > 0 && handler_id > 0 && cbm_store_insert_edge(target, &handles) > 0;
    cbm_store_close(source);
    cbm_store_close(target);
    return ok;
}

TEST(cross_repo_scan_bound_counts_examined_rows_not_matches) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_bounded_scan(&fixture, "bounded-source", "bounded-target");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed bounded scan fixture");
    }
    const char *target = "bounded-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("bounded-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 1);
    ASSERT_EQ(result.http_edges, 0);
    PASS();
}

TEST(cross_repo_propagates_delete_failure) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "delete-source", "delete-target",
                                           "/delete-failure", "delete");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed delete failure fixture");
    }
    const char *target = "delete-target";
    cbm_cross_repo_result_t initial = cbm_cross_repo_match("delete-source", &target, 1);
    bool trigger_created =
        !initial.failed && initial.http_edges == 1 &&
        cross_repo_exec(&fixture, "delete-source",
                        "CREATE TRIGGER fail_cross_delete BEFORE DELETE ON edges "
                        "WHEN OLD.type = 'CROSS_HTTP_CALLS' BEGIN "
                        "SELECT RAISE(ABORT, 'forced cross delete failure'); END;");
    cbm_cross_repo_result_t failed = {0};
    if (trigger_created) {
        failed = cbm_cross_repo_match("delete-source", &target, 1);
    }
    cross_repo_fixture_end(&fixture);

    ASSERT_TRUE(trigger_created);
    ASSERT_TRUE(failed.failed);
    ASSERT_EQ(failed.http_edges, 0);
    PASS();
}

TEST(cross_repo_failed_bidirectional_insert_is_not_counted) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "insert-source", "insert-target",
                                           "/insert-failure", "insert") &&
                 cross_repo_exec(&fixture, "insert-target",
                                 "CREATE TRIGGER fail_cross_insert BEFORE INSERT ON edges "
                                 "WHEN NEW.type = 'CROSS_HTTP_CALLS' BEGIN "
                                 "SELECT RAISE(ABORT, 'forced cross insert failure'); END;");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed insert failure fixture");
    }
    const char *target = "insert-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("insert-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_TRUE(result.failed);
    ASSERT_EQ(result.http_edges, 0);
    ASSERT_EQ(result.projects_scanned, 0);
    PASS();
}

/* Keep the source scan active after the target-b match is published. This
 * gives the watcher a deterministic observable hand-off point without adding
 * a test hook to the production pass. */
static bool cross_repo_seed_cancel_scan_tail(const cross_repo_fixture_t *fixture,
                                             const char *source_project) {
    enum { CANCEL_SCAN_TOTAL_ROWS = 4096, SEEDED_MATCH_ROWS = 3 };
    char source_path[512];
    if (!cross_repo_project_path(fixture, source_project, source_path, sizeof(source_path))) {
        return false;
    }
    cbm_store_t *source = cbm_store_open_path_existing(source_path);
    if (!source) {
        return false;
    }
    bool ok =
        sqlite3_exec(cbm_store_get_db(source), "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    cbm_node_t caller = {.project = source_project,
                         .label = "Function",
                         .name = "cancel_tail_caller",
                         .qualified_name = "cancel.source.tail-caller",
                         .file_path = "cancel-tail.c"};
    int64_t caller_id = ok ? cbm_store_upsert_node(source, &caller) : 0;
    ok = ok && caller_id > 0;
    for (int i = SEEDED_MATCH_ROWS; ok && i < CANCEL_SCAN_TOTAL_ROWS; i++) {
        char name[64];
        char qn[96];
        snprintf(name, sizeof(name), "cancel_tail_route_%d", i);
        snprintf(qn, sizeof(qn), "%s.cancel-tail.%d", source_project, i);
        cbm_node_t route = {.project = source_project,
                            .label = "Route",
                            .name = name,
                            .qualified_name = qn,
                            .file_path = "cancel-tail.c"};
        int64_t route_id = cbm_store_upsert_node(source, &route);
        cbm_edge_t edge = {.project = source_project,
                           .source_id = caller_id,
                           .target_id = route_id,
                           .type = "HTTP_CALLS",
                           .properties_json = "{}"};
        ok = route_id > 0 && caller_id > 0 && cbm_store_insert_edge(source, &edge) > 0;
    }
    if (ok) {
        ok = sqlite3_exec(cbm_store_get_db(source), "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    } else {
        (void)sqlite3_exec(cbm_store_get_db(source), "ROLLBACK", NULL, NULL, NULL);
    }
    cbm_store_close(source);
    return ok;
}

typedef struct {
    const cross_repo_fixture_t *fixture;
    atomic_int *cancelled;
    atomic_bool observed_target_write;
} cross_repo_cancel_watcher_t;

static void *cross_repo_cancel_after_target_write(void *arg) {
    enum { CANCEL_POLL_LIMIT = 10000 };
    cross_repo_cancel_watcher_t *watcher = arg;
    for (int i = 0; i < CANCEL_POLL_LIMIT; i++) {
        int count = cross_repo_count_edges(watcher->fixture, "cancel-target-b", "CROSS_HTTP_CALLS");
        if (count > 0) {
            atomic_store_explicit(&watcher->observed_target_write, true, memory_order_release);
            atomic_store_explicit(watcher->cancelled, 1, memory_order_release);
            return NULL;
        }
        cbm_usleep(1000);
    }
    /* Bound a broken implementation too: the test should fail, not hang. */
    atomic_store_explicit(watcher->cancelled, 1, memory_order_release);
    return NULL;
}

TEST(cross_repo_cancel_mid_run_keeps_completed_target_and_stops_before_later_target) {
    cross_repo_fixture_t fixture;
    bool setup =
        cross_repo_fixture_begin(&fixture) &&
        cross_repo_seed_http_pair(&fixture, "cancel-source", "cancel-target-a", "/cancel-a", "a") &&
        cross_repo_seed_http_pair(&fixture, "cancel-source", "cancel-target-b", "/cancel-b", "b") &&
        cross_repo_seed_http_pair(&fixture, "cancel-source", "cancel-target-c", "/cancel-c", "c") &&
        cross_repo_seed_cancel_scan_tail(&fixture, "cancel-source");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed cancellation fixture");
    }

    atomic_int cancelled;
    atomic_init(&cancelled, 0);
    cross_repo_cancel_watcher_t watcher = {
        .fixture = &fixture,
        .cancelled = &cancelled,
    };
    atomic_init(&watcher.observed_target_write, false);
    cbm_thread_t watcher_thread;
    bool watcher_started =
        cbm_thread_create(&watcher_thread, 0, cross_repo_cancel_after_target_write, &watcher) == 0;

    const char *targets[] = {"cancel-target-c", "cancel-target-a", "cancel-target-b"};
    cbm_cross_repo_result_t result = {0};
    if (watcher_started) {
        result = cbm_cross_repo_match_cancellable("cancel-source", targets, 3, &cancelled);
        (void)cbm_thread_join(&watcher_thread);
    }

    bool observed = atomic_load_explicit(&watcher.observed_target_write, memory_order_acquire);
    int completed_target_edges =
        cross_repo_count_edges(&fixture, "cancel-target-a", "CROSS_HTTP_CALLS");
    int interrupted_target_edges =
        cross_repo_count_edges(&fixture, "cancel-target-b", "CROSS_HTTP_CALLS");
    int later_target_edges =
        cross_repo_count_edges(&fixture, "cancel-target-c", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_TRUE(watcher_started);
    ASSERT_TRUE(observed);
    ASSERT_TRUE(result.cancelled);
    ASSERT_TRUE(result.partial_results);
    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 1);
    ASSERT_TRUE(completed_target_edges > 0);
    ASSERT_TRUE(interrupted_target_edges > 0);
    ASSERT_EQ(later_target_edges, 0);
    PASS();
}

TEST(cross_repo_pre_cancel_preserves_existing_cross_edges) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "pre-cancel-source", "pre-cancel-target",
                                           "/pre-cancel", "pre");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed pre-cancel fixture");
    }

    const char *target = "pre-cancel-target";
    cbm_cross_repo_result_t initial = cbm_cross_repo_match("pre-cancel-source", &target, 1);
    int before = cross_repo_count_edges(&fixture, "pre-cancel-source", "CROSS_HTTP_CALLS");
    atomic_int cancelled;
    atomic_init(&cancelled, 1);
    cbm_cross_repo_result_t result =
        cbm_cross_repo_match_cancellable("pre-cancel-source", &target, 1, &cancelled);
    int after = cross_repo_count_edges(&fixture, "pre-cancel-source", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(initial.failed);
    ASSERT_TRUE(before > 0);
    ASSERT_TRUE(result.cancelled);
    ASSERT_FALSE(result.partial_results);
    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 0);
    ASSERT_EQ(after, before);
    PASS();
}

SUITE(cross_repo) {
    RUN_TEST(cross_repo_null_target_fails_without_dereference);
    RUN_TEST(cross_repo_wildcard_keeps_projects_containing_internal_tokens);
    RUN_TEST(cross_repo_scan_bound_counts_examined_rows_not_matches);
    RUN_TEST(cross_repo_propagates_delete_failure);
    RUN_TEST(cross_repo_failed_bidirectional_insert_is_not_counted);
    RUN_TEST(cross_repo_cancel_mid_run_keeps_completed_target_and_stops_before_later_target);
    RUN_TEST(cross_repo_pre_cancel_preserves_existing_cross_edges);
}
