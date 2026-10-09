#!/usr/bin/env bash
# Compare freshly emitted JIT code against a reference worktree. The Python
# driver builds isolated instrumented executables and preserves failure logs.
set -euo pipefail
exec python3 "$(dirname "$0")/jit_emit_audit/audit.py" "$@"
