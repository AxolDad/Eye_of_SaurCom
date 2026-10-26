# Eye of SaurCom plugin SDK

- `include/saurcom_plugin.h` the stable C ABI (public domain)
- `cmake/SaurComPlugin.cmake` `saurcom_add_plugin()`: builds a plugin and copies its manifest
- `template/` a minimal IQ plugin to copy
- `examples/` tone-detector, burst-counter, log-to-file (built by the repository's CMake into `<build>/plugins/<id>/`)
- `rust/saurcom-plugin` Rust bindings; `rust/examples/level-meter` a Rust plugin

Documentation: `docs/plugins.md`.
