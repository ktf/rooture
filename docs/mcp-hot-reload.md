# MCP hot-reload — surviving execv without dropping the connection

This page documents how the rooture MCP server survives a `reload` call
without forcing the user to manually reconnect via `/mcp`.

---

## Problem

The `reload` tool replaces the rooture process image via `execv`.  The new
process starts fresh: all rooture state (loaded scripts, defined symbols,
open canvases, JIT-compiled functions) is gone — **this is intentional and
advertised as a full session reset**.

The problem was purely about the *connection*: the new process entered the
MCP message loop and waited for an `initialize` request.  Claude Code never
re-sends `initialize` on an established transport, so the connection appeared
dead and the tool list was empty until the user ran `/mcp` manually.

---

## Solution

### 1. Signal via environment variable

Before calling `execv`, the reload handler sets an environment variable:

```cpp
setenv("ROOTURE_MCP_RELOAD", "1", 1);
execv(g_argv[0], g_argv);
```

### 2. Detect on startup

In `main`, immediately after argument parsing:

```cpp
if (getenv("ROOTURE_MCP_RELOAD")) {
  g_mcp_reload = true;
  unsetenv("ROOTURE_MCP_RELOAD");  // clear so a further reload doesn't re-trigger
}
```

`unsetenv` is important: without it, a second `reload` call would inherit
the flag and send an unwanted notification on startup.

### 3. Send notifications at the start of the MCP loop

In `mcp_thread_fn`, before entering the `getline` loop:

```cpp
if (g_mcp_reload) {
  // Tell the user the session was reset
  send_resp({{"jsonrpc","2.0"},{"method","notifications/message"},{"params",{
    {"level","warning"},
    {"data","rooture session reset: all state (loaded scripts, defined symbols, "
            "open canvases) has been lost."}
  }}});
  // Tell Claude Code to re-fetch the tool list
  send_resp({{"jsonrpc","2.0"},{"method","notifications/tools/list_changed"}});
}
```

`notifications/message` (MCP log notification) surfaces in Claude Code as a
warning so the user knows state was lost.  `notifications/tools/list_changed`
prompts Claude Code to call `tools/list` immediately, restoring the full tool
set without any manual action.

---

## Why not preserve state?

Intentionally not preserved.  `execv` is used precisely because it gives a
completely clean process image — no stale ROOT objects, no leaked Cling JIT
symbols, no open file handles from the previous session.  When you call
`reload` after a rebuild, you want rooture to behave exactly as if it had
just been started for the first time.  The only thing that survives is the
MCP transport (stdin/stdout file descriptors, which `execv` inherits).

---

## Result

After calling `reload`:

1. The current process sends the tool result, flushes, sets the env var, calls `execv`.
2. The new process starts, detects `ROOTURE_MCP_RELOAD`, clears the env var.
3. It sends `notifications/message` (session-reset warning) and
   `notifications/tools/list_changed`.
4. Claude Code receives the notification, calls `tools/list`, gets the fresh
   tool list.
5. All tools are available immediately — no `/mcp` required.
