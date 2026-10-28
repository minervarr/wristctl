# wristctl_core instructions

Portable C11 library at `wristctl/libs/firstparty/wristctl_core`.

## Non-negotiables

- No OS SDK headers, no JNI, no `#ifdef _WIN32`/`__ANDROID__`, no filesystem or
  network. Platform needs go in a frontend or `platform/` library.
- C11 only; headers wrapped in `extern "C"`. The C++ consumer test exists to
  keep that true.
- The session is callback-driven (`wc_transport`); do not add a BLE stack, a
  clock or a thread to this library. A frontend owns those.
- Namespace: `wristctl/` headers, `wc_` structs, `wristctl_core` target and
  `wristctl_core::wristctl_core` alias.
- Sources are listed explicitly in `CMakeLists.txt`; no globs.
- Tests are plain `assert()` binaries with `#undef NDEBUG`, Debug-only,
  registered through `cmake/TestHelpers.cmake`.

## Generated files — edit the spec, then regenerate

From the `wristctl/` root:

```sh
make generate   # spec -> Python and C codecs
make test       # regenerate + configure + build + ctest + Python
```

Generated here: `include/wristctl/{generated,models}.h`,
`src/{generated,models}.c`, `tests/generated_vectors.h`. The spec
(`spec/classes.json`, `spec/models.json`, `spec/vectors/*.json`) is
hand-authored.

## Consumers

`py/`, `android/`, `gui/` and `platform/linux/` are outside this library. Do not
edit them from here. `android/` links the CMake target; it does not copy sources.
The BlueZ transport lives in `platform/linux/` and supplies the
`wc_transport` callbacks; the session itself stays portable.

## License

AGPL-3.0.
