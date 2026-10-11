#!/usr/bin/env bash
# The portable test runner (Linux; g++, lua5.4, python3).
#
#   tests/run.sh [--only cpp|lua|python|checks] [extension...]
#
# 1. Shared code: every shared/<part>/tests/*Tests.cpp.
# 2. For each extension (default: every bridge-*/), the shared checks from
#    shared/tests (product_checks.py, abi_syntax.py, verify_gate.lua), then the
#    extension's own suites in <extension>/tests:
#      *Tests.cpp   compiled with the common flags below and run
#      *Tests.lua   run with lua5.4 from the repository root
#      test_*.py    run with python3 -m unittest
#
# Suites named *WinTests.cpp need Windows; CMake builds them in the Windows CI
# job. The repository's tooling tests (tests/test_*.py, *.ps1) run separately.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

only=""
extensions=()
while (($#)); do
    case "$1" in
        --only) only="$2"; shift 2 ;;
        --only=*) only="${1#--only=}"; shift ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) extensions+=("${1%/}"); shift ;;
    esac
done
case "${only}" in ""|cpp|lua|python|checks) ;; *) echo "unknown --only ${only}" >&2; exit 2 ;; esac
if ((${#extensions[@]} == 0)); then
    for folder in bridge-*/; do extensions+=("${folder%/}"); done
fi
for extension in "${extensions[@]}"; do
    [[ -f "${extension}/CMakeLists.txt" && -d "${extension}/tests" ]] \
        || { echo "${extension}: not an extension folder" >&2; exit 2; }
done

if ! command -v lua5.4 >/dev/null 2>&1; then
    echo "lua5.4 not found" >&2
    exit 1
fi

build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT
cxx_flags=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread
    -Ishared -Ishared/contract -Ishared/ue4ss-abi -Ishared/native -Ishared/tests)

wants() { [[ -z "${only}" || "${only}" == "$1" ]]; }

run_cpp() { # <include-dir>... -- <suite>...
    local includes=() suite
    while [[ "$1" != "--" ]]; do includes+=("-I$1"); shift; done
    shift
    for suite in "$@"; do
        [[ "${suite}" == *WinTests.cpp ]] && continue
        local binary="${build_dir}/$(basename "${suite}" .cpp)"
        c++ "${cxx_flags[@]}" "${includes[@]}" "${suite}" -o "${binary}"
        "${binary}"
    done
}

shopt -s nullglob

if wants cpp; then
    for tests in shared/*/tests; do
        [[ "${tests}" == shared/tests ]] && continue
        part="$(dirname "${tests}")"
        echo "== ${part}"
        run_cpp "${part}/include" -- "${tests}"/*Tests.cpp
    done
fi

for extension in "${extensions[@]}"; do
    echo "== ${extension}"
    if wants checks; then
        python3 shared/tests/product_checks.py "${extension}"
        python3 shared/tests/abi_syntax.py "${extension}"
        lua5.4 shared/tests/verify_gate.lua "${extension}"
    fi
    if wants cpp; then
        run_cpp "${extension}/include" "${extension}/contract" -- "${extension}"/tests/*Tests.cpp
    fi
    if wants lua; then
        for suite in "${extension}"/tests/*Tests.lua; do lua5.4 "${suite}"; done
    fi
    if wants python; then
        python3 -m unittest discover -s "${extension}/tests" -p 'test_*.py'
    fi
done
