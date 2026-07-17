# App Manager Host Tests

These tests compile the production mailbox, lifecycle, control, and LVGL adapter sources
with only the FreeRTOS/LVGL/hardware port surface replaced by host shims. The adapter test
enables its filesystem and FreeType paths and injects failures at every initialization and
cleanup boundary to verify reverse rollback and retry behavior.
The control suite also injects activity at the standby boundary to ensure an
accepted wake request cannot be overwritten by generation setup.

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
