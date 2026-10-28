# wristctl_core

Portable **C11** implementation of the G-SHOCK protocol: frame codecs, the model
and capability registry, and the transport-agnostic session state machine. No
OS headers, no JNI, no platform `#ifdef`s.

The wire protocol is described once in `spec/`; the Python and C codecs are
generated from it.

## Build standalone

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests are assert-based, Debug-only, registered with CTest. A Release configure
builds the library and no tests.

## Consume from a parent project

```cmake
set(WRISTCTL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/firstparty/wristctl_core wristctl_core_build)
target_link_libraries(your_target PRIVATE wristctl_core::wristctl_core)
```

Public headers live under `include/wristctl/` and are wrapped in `extern "C"`.
The target is position-independent, so it can be linked into a shared JNI
library.

## Generated sources

Committed, so a build never needs Python:

- `include/wristctl/generated.h`, `src/generated.c`
- `include/wristctl/models.h`, `src/models.c`
- `tests/generated_vectors.h`

Regenerate from the repository's `wristctl/` root with `make generate`. The
generators in `gen/` also write the Python codecs into `py/`; that path is a
consumer of this library, not part of its build.

## Session

`include/wristctl/session.h` owns request/response correlation, the parked
frame queue and the connection handshake. A frontend supplies a `wc_transport`
with blocking `write`/`read` callbacks; the core never touches BLE. Tests drive
it with a recorded-frame fake (`tests/test_session.c`).

## Spec

`spec/classes.json` defines frame layouts. `spec/models.json` is extracted from
the vendor app's `CasioLibUtil.DeviceType` table. `spec/vectors/*.json` are the
golden contract shared by the Python and C implementations. `gen/` is dev
tooling; the build does not invoke it.

## License

AGPL-3.0. See `LICENSE`.
