#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

c++ -std=c++20 -Wall -Wextra -Werror -Ishared shared/dispatch/tests/DispatchBudgetTests.cpp -o "${build_dir}"/dispatch-budget-tests
"${build_dir}"/dispatch-budget-tests

c++ -std=c++20 -Wall -Wextra -Werror -pthread \
    -Ibridge-events/include -Ishared/contract -Ishared \
    bridge-events/tests/SessionAliasIndexTests.cpp \
    -o "${build_dir}"/lua-event-bridge-tests

"${build_dir}"/lua-event-bridge-tests

c++ -std=c++20 -Wall -Wextra -Werror -Ibridge-events/include -Ishared/contract -Ishared bridge-events/tests/BindingSnapshotTests.cpp -o "${build_dir}"/binding-snapshot-tests
"${build_dir}"/binding-snapshot-tests

c++ -std=c++20 -Wall -Wextra -Werror -Ibridge-events/include -Ishared/contract -Ishared bridge-events/tests/WeakObjectPtrTests.cpp -o "${build_dir}"/weak-object-tests
"${build_dir}"/weak-object-tests

c++ -std=c++20 -Wall -Wextra -Werror -pthread -Ishared shared/dispatch/tests/QueueBuffersTests.cpp -o "${build_dir}"/queue-buffer-tests
"${build_dir}"/queue-buffer-tests

c++ -std=c++20 -Wall -Wextra -Werror -Ishared shared/dispatch/tests/QueueDispatchScheduleTests.cpp -o "${build_dir}"/queue-schedule-tests
"${build_dir}"/queue-schedule-tests

c++ -std=c++20 -Wall -Wextra -Werror -pthread -Ibridge-events/include -Ishared/contract -Ishared bridge-events/tests/LifecycleRegistryTests.cpp -o "${build_dir}"/lifecycle-registry-tests
"${build_dir}"/lifecycle-registry-tests

c++ -std=c++20 -Wall -Wextra -Werror -pthread -Ibridge-events/include -Ishared/contract -Ishared bridge-events/tests/LifetimeProbeTests.cpp -o "${build_dir}"/lifetime-probe-tests
"${build_dir}"/lifetime-probe-tests

c++ -std=c++20 -Wall -Wextra -Werror -Ishared/bootstrap/include shared/bootstrap/tests/BootstrapSelectionTests.cpp -o "${build_dir}"/bootstrap-selection-tests
"${build_dir}"/bootstrap-selection-tests
