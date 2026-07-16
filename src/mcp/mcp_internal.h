#ifndef CBM_MCP_INTERNAL_H
#define CBM_MCP_INTERNAL_H

#include "mcp/mcp.h"

/* White-box fault injection for deterministic cross-platform quarantine
 * safety tests. This header is internal and is not part of the MCP API. */
typedef bool (*cbm_mcp_quarantine_test_hook_fn)(void *context, const char *step);

void cbm_mcp_server_set_quarantine_test_hook(cbm_mcp_server_t *srv,
                                             cbm_mcp_quarantine_test_hook_fn hook,
                                             void *context);

/* Release only the constructor-created pristine in-memory store. Public
 * cbm_mcp_server_new(NULL) semantics remain unchanged; daemon sessions use
 * this immediately before publication so idle sessions retain no SQLite DB. */
bool cbm_mcp_server_release_pristine_memory_store(cbm_mcp_server_t *srv);

#endif
