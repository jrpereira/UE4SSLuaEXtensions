#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT
generated_dir="${build_dir}/generated"
mkdir -p "${generated_dir}"
header="${generated_dir}/EmbeddedLuaAPI.hpp"
# Mirror bridge-events/CMakeLists.txt: as many fixed-size chunks as the API needs.
lua_api="${repo_root}/bridge-events/lua/bridge_api.lua"
chunk_size=7000
lua_size="$(wc -c < "${lua_api}")"
chunk_count=$(( (lua_size + chunk_size - 1) / chunk_size ))
printf '%s\n' \
    '#pragma once' \
    '#include <array>' \
    '#include <string_view>' \
    'namespace UE4SSLuaEventBridge {' \
    "inline constexpr std::array<std::string_view, ${chunk_count}> embedded_lua_api_chunks{" \
    > "${header}"
for (( index = 0; index < chunk_count; index++ )); do
    printf '%s' 'R"UE4SSLEB_LUA(' >> "${header}"
    dd if="${lua_api}" bs="${chunk_size}" skip="${index}" count=1 status=none >> "${header}"
    printf '%s\n' ')UE4SSLEB_LUA",' >> "${header}"
done
printf '%s\n' '};' '}' >> "${header}"

g++ \
    -std=c++20 \
    -D_WIN32 \
    '-D__declspec(x)=' \
    -I"${build_dir}/generated" \
    -I"${repo_root}/bridge-events/include" \
    -I"${repo_root}/bridge-events/contract" \
    -I"${repo_root}/shared/contract" \
    -I"${repo_root}/shared" \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -fsyntax-only \
    "${repo_root}/bridge-events/src/BridgeMod.cpp" \
    "${repo_root}/bridge-events/src/EnhancedInputBackend.cpp"
