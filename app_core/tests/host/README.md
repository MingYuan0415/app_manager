# App Manager Host Tests

These tests compile the production static registry, navigation model, command gateway,
mailbox, lifecycle, control, and LVGL adapter sources with only the FreeRTOS/LVGL/hardware
port surface replaced by host shims.

The suites cover app-owned route and shared-definition validation, randomized model invariants, the fixed
24-slot navigation pool, value-copy semantics, callback reentrancy, both timeout paths,
seven-stage lifecycle order, new intent delivery, resident eviction, detached cleanup,
Screen rollback, suspend/resume, and retryable PAUSE/STOP failures. The adapter test
enables its filesystem and FreeType paths and injects failures at every initialization and
cleanup boundary to verify reverse rollback and retry behavior. The control suite also
injects activity at the standby boundary to ensure an accepted wake request cannot be
overwritten by generation setup. It verifies that startup input/PM admission stays closed,
brightness operations execute through the UI dispatcher, and a failed suspend rollback
restarts the inactivity epoch instead of retrying every poll tick. Screen-off brightness
updates, startup touch gating, and startup/deinit admission races are covered explicitly.
The lifecycle suite is built twice and captures `stderr` to verify all seven lifecycle
trace formats when `CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG` is enabled and complete trace
removal when it is disabled, while running the same behavioral assertions in both builds.

The shim headers intentionally retain FreeRTOS and ESP-IDF function names so
the production sources compile without test-only branches beyond `HOST_TEST`.

```sh
cmake -S app_core/tests/host -B /tmp/mt-app-manager-host-tests
cmake --build /tmp/mt-app-manager-host-tests
ctest --test-dir /tmp/mt-app-manager-host-tests --output-on-failure

cmake -S app_core/tests/host -B /tmp/mt-app-manager-host-asan \
    -DAPP_MANAGER_SANITIZER=address
cmake --build /tmp/mt-app-manager-host-asan
ctest --test-dir /tmp/mt-app-manager-host-asan --output-on-failure

cmake -S app_core/tests/host -B /tmp/mt-app-manager-host-tsan \
    -DAPP_MANAGER_SANITIZER=thread
cmake --build /tmp/mt-app-manager-host-tsan
ctest --test-dir /tmp/mt-app-manager-host-tsan --output-on-failure
```

On x86_64 Linux the TSan profile uses `setarch -R` when available. This avoids
the GCC runtime's `unexpected memory mapping` failure under WSL2 ASLR.
