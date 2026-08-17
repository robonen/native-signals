# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] — 2026-08-17

First release.

### Added

- **Run-time graph** (`nsig::dynamic`, re-exported into `nsig`) — `signal`,
  `computed`, `effect`, `effect_scope`, `watch`, `batch`, `trigger`,
  `untracked`, `readonly`, `on_cleanup`. The push–pull propagation algorithm is
  a port of [alien-signals](https://github.com/stackblitz/alien-signals) with
  C++ ownership: intrusive refcounting, pooled nodes and links, RAII handles.
- **Compile-time graph** (`nsig::fixed`) — `signal`, `computed`, `fold`,
  `watch`, `batch`. No dependency tracking: one global epoch counter, values
  compared where they leave the graph. Within ~5% of hand-written code on the
  fan-controller benchmark.
- **Expressions** (`nsig/expr.hpp`) — operators over nodes for both graphs,
  plus `pick`, `lift` and `make_array`. Short-circuiting `&&`/`||` preserved;
  measurably zero-cost.
- **Device adapters** (`nsig/adapt.hpp`) — `sensor` and `actuator` for the two
  seams with a driver, `drive`, `sample`, `on_rising`/`on_falling`; lifted
  maths (`clamp`, `min`, `max`, `abs`, `map_range`); idempotent transforms
  (`schmitt`, `peak`, `trough`); sampling filters (`ema`, `slew`, `debounce`,
  `median3`).
- **Coroutines** (`nsig/async.hpp`) — `task<T>`, `co_await changed(...)`,
  `co_await until(...)`, and `resource<T>` with reactive loading/value/error.
- **Packaging** — CMake with `FetchContent` and install/export support, a C++20
  module (`import nsig;`), and a single-header amalgamation.
- **Embedded support** — builds with `-fno-exceptions -fno-rtti`,
  `NSIG_THREAD_LOCAL=` for a single global runtime, and `nsig::reserve()` to
  pre-size the pools so the steady state performs no heap allocation.

### Verified

- Semantics conformance against alien-signals' own suite, including the
  regression for [issue #115](https://github.com/stackblitz/alien-signals/issues/115).
- The reactive and hand-written fan controllers are asserted to emit identical
  board operations before either is benchmarked.
- Clean under AddressSanitizer, UndefinedBehaviorSanitizer and Valgrind on
  Clang 18 and GCC 13.
