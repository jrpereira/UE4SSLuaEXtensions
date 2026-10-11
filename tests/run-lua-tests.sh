#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if command -v lua5.4 >/dev/null 2>&1; then
    lua_bin=lua5.4
elif command -v lua >/dev/null 2>&1; then
    lua_bin=lua
elif command -v texlua >/dev/null 2>&1; then
    lua_bin=texlua
else
    echo "Lua 5.4 interpreter not found" >&2
    exit 1
fi

"${lua_bin}" bridge-events/tests/LuaHelperTests.lua
"${lua_bin}" bridge-events/tests/LifecycleIntegrationTests.lua
"${lua_bin}" tests/LoadTimeVerificationTests.lua
