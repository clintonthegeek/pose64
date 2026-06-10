# Deferred / preserved patches

Working-tree changes intentionally removed from the baseline during Phase 0,
preserved here so they are not lost.

## 2026-03-13-puppetstring-poll-delivery.patch

The candidate "poll-always" event-delivery change from the final 2026-03-13
session (`EmPatchMgr::PuppetString`): after enqueueing a pen/key event it forces
a nil event + `callROM = kSkipROM; return;`, and in interactive mode (no
Gremlins, no playback) sets `clearTimeout = true` unconditionally so the guest
never sleeps on an infinite timeout — compensating for the removed
`PrvWakeUpCPU` wakeup.

**Status:** UNVERIFIED. Deferred to **Phase 2** (recovery plan Task 2.1/2.2,
option A). The baseline must not carry it (STATUS.md landmine #3).

**Caveat:** the diff also contains the `fprintf` debug instrumentation that was
stripped in Phase 0. Phase 2 should extract only the `kSkipROM`/`clearTimeout`
behavior hunks (the ones without `fprintf`), apply against the live
`EvtGetEvent`/`EvtGetPen` patch path, and gate it on the new delivery test
(Task 2.1) run WITH and WITHOUT the change.

## cpp-mcp-accept-mcp-2025-06-18.patch

A local modification that was sitting uncommitted in the `src/cpp-mcp` working
tree (against upstream `hkr04/cpp-mcp` @ `dc86c91`). It teaches cpp-mcp's
**HTTP/SSE** server to accept MCP protocol version `2025-06-18` and adds an
OAuth-discovery error handler.

**Status:** UNUSED by POSE64. `pose64-mcp-proxy` is built from
`src/pose64-mcp-proxy.cpp` alone and only uses `cpp-mcp/common` as an include
path — it never compiles `cpp-mcp/src/mcp_server.cpp`, and the proxy speaks
**stdio**, not HTTP. The change was preserved here and discarded from the
submodule so it can be pinned at a clean upstream commit. Apply it (or upstream
it to hkr04/cpp-mcp) only if the proxy is ever rearchitected onto cpp-mcp's HTTP
server.
