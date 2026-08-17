# native-signals

[![CI](https://github.com/robonen/native-signals/actions/workflows/ci.yml/badge.svg)](https://github.com/robonen/native-signals/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)

Reactive programming suite for modern C++ (C++23).

A port of the push–pull propagation algorithm from
[stackblitz/alien-signals](https://github.com/stackblitz/alien-signals), rebuilt
around C++ ownership: intrusive refcounting instead of a GC, pooled nodes and
links instead of allocator churn, RAII handles instead of dispose callbacks —
plus a coroutine layer the original doesn't have.

```cpp
#include <nsig/nsig.hpp>

nsig::signal   count{1};
nsig::computed doubled{[&] { return count() * 2; }};

auto log = nsig::effect([&] {
    std::println("count={} doubled={}", count(), doubled());
});                       // count=1 doubled=2

count = 2;                // count=2 doubled=4
```

## Why it exists

alien-signals is the fastest signal implementation in JavaScript, and its
algorithm — doubly-linked dependency lists, graph colouring, iterative
propagation — is language-agnostic. The interesting question is what the
algorithm looks like once you stop paying for a garbage collector and a JIT.

The answer: **1.3–5× faster and 1.8–2.6× smaller**, with the same semantics.

## Benchmarks

Identical graph shapes, identical operation (`src(src() + 1)`), identical
measurement protocol — `bench/propagate.cpp` and `bench/propagate.mjs` are
line-for-line translations of each other. Node runs with full JIT (not the
`--jitless` mode alien-signals' own bench script uses), single core, x86-64.

| benchmark                    | native-signals | alien-signals | speedup |
|------------------------------|----------------|---------------|---------|
| propagate 1x1                |        53.9 ns |      111.5 ns |   2.07x |
| propagate 10x10              |      1885.4 ns |     4573.2 ns |   2.43x |
| propagate 100x100            |    234111.7 ns |  1160860.5 ns |   4.96x |
| propagate 1000x10            |    294450.9 ns |  1143398.0 ns |   3.88x |
| broadcast 1→1000             |     23332.5 ns |    43272.5 ns |   1.85x |
| deep-unchanged d=100         |      1174.0 ns |     1646.9 ns |   1.40x |
| deep-unchanged d=1000        |     16101.9 ns |    21446.0 ns |   1.33x |
| cached computed read         |         2.9 ns |        5.7 ns |   1.97x |
| create+destroy triple        |        76.3 ns |      119.7 ns |   1.57x |
| unstable deps (16 sources)   |        37.3 ns |      113.1 ns |   3.03x |

| memory / node | native-signals | alien-signals | ratio |
|---------------|----------------|---------------|-------|
| signal        |           67 B |         120 B | 1.79x |
| computed      |           90 B |         232 B | 2.59x |
| effect        |          167 B |         392 B | 2.35x |

The gap widens with graph size: V8's object headers and write barriers cost
more the more nodes you touch, while `sizeof(node)` here is 48 bytes and
`sizeof(link)` is 56, both carved from per-thread pools.

Two honest notes. `deep-unchanged` is the narrowest win because that path is
dominated by pointer chasing in `check_dirty`, where a JIT does fine. And
`create+destroy` was a *tie* until nodes got their own pool allocator — V8's
bump-allocating nursery is genuinely good at short-lived objects, and a
general-purpose `malloc` is not.

Reproduce:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/nsig_bench
node --expose-gc bench/propagate.mjs /path/to/alien-signals/esm/index.mjs
```

## Correctness

`tests/conformance.cpp` ports alien-signals' own suite — `effect.spec.ts`,
`effectScope.spec.ts`, `trigger.spec.ts`, including the regression for
[issue #115](https://github.com/stackblitz/alien-signals/issues/115) — plus the
classic reactivity cases: diamonds, dynamic dependencies, deep memoised chains,
batching, laziness. 42 tests, 93 assertions, clean under ASan, UBSan and
Valgrind (`0 errors, 0 leaks, 134 total allocations` for the whole suite).

```sh
ctest --test-dir build --output-on-failure
```

## Installation

### Namespaces

```
nsig::            shared vocabulary — equality policies, expressions and
                  operators, lifted maths (clamp/min/max/map_range), idempotent
                  transforms (schmitt/peak/trough), filter functors
                  (ema/slew/debounce/median3)

nsig::dynamic::   the run-time graph — signal, computed, effect, effect_scope,
                  watch, batch, trigger, and the coroutine layer

nsig::fixed::     the compile-time graph — signal, computed, fold, watch, batch,
                  plus the device seams: sensor, actuator, drive, sample,
                  on_rising/on_falling
```

Neither graph is nested inside the other. `nsig::dynamic` is re-exported into
`nsig`, so `nsig::signal` is the run-time one and stays the default; spell out
`nsig::dynamic::signal` in a file that uses both. `nsig::st` is a short alias
for `nsig::fixed`.

The device seams live in `nsig::fixed` because that is what they are built on: a
`sensor` holds a `fixed::signal` and an `actuator` notifies the fixed graph. In
the run-time graph the equivalents already exist — a sensor is a `nsig::signal`
you assign from a driver read, and an actuator is a `nsig::watch` whose sink
writes the register.

Header-only. Pick whichever fits:

```cmake
# FetchContent
include(FetchContent)
FetchContent_Declare(native-signals
    GIT_REPOSITORY https://github.com/robonen/native-signals.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(native-signals)
target_link_libraries(app PRIVATE native-signals::native-signals)
```

```cpp
#include <nsig/nsig.hpp>          // everything
#include <nsig/signals.hpp>       // core only — no <coroutine> dependency
#include <nsig/static_graph.hpp>  // compile-time wiring, for hot control loops
#include <nsig/core.hpp>          // the raw graph, to build your own surface API
```

```cpp
import nsig;                   // -DNSIG_BUILD_MODULE=ON, needs Ninja + a
                               // module-capable toolchain
```

Or drop `single_include/nsig.hpp` (63 KB, one file) into your tree.

Requires C++23. Tested on Clang 18 and GCC 13.

## API

### signal

```cpp
nsig::signal count{1};                       // CTAD → signal<int>
nsig::signal<std::string> name{"ann"};
nsig::signal<std::string> buf{std::in_place, 8, 'x'};

count();          // tracked read (also .get(), *count)
count.peek();     // untracked read
count = 2;        // write (also .set(2), count(2))

items.modify([](auto& v) { v.push_back(1); });   // copy, mutate, compare, write
items.mutate([](auto& v) { v.push_back(1); });   // mutate in place, notify unconditionally
count.touch();                                   // notify without changing anything
```

Reads return `const T&`, so `signal<std::string>` costs nothing extra to read.
The handle is a refcounted reference: copying a `signal` **shares** the node, it
does not clone the value. `a = b` between two signals rebinds the handle; to
copy a value write `a.set(b.get())`.

Change detection is pluggable:

```cpp
nsig::signal<float, nsig::never_equal> ticks{0};   // every write notifies
nsig::signal<Config, MyEquals> config{...};
```

`auto_equal` (the default) uses `operator==` when the type has one and falls
back to "always changed" when it doesn't, so `signal<T>` works with types that
aren't comparable.

### computed

```cpp
nsig::computed total{[&] { return price() * qty(); }};   // CTAD from the lambda

// Incremental form — receives the previous value, nullptr on the first run:
nsig::computed<int> running_max{[&](const int* prev) {
    int v = sample();
    return prev ? std::max(v, *prev) : v;
}};
```

Lazy and memoised: a `computed` nobody reads is never evaluated, and one whose
inputs changed to an equal value does not propagate further.

### effect

```cpp
auto e = nsig::effect([&] {
    render(count());
    return [] { teardown(); };      // optional cleanup, runs before the next
});                                 // run and on disposal

e.stop();                           // explicit
                                    // …or just let the handle go out of scope
```

Ownership follows lexical structure, which is the one place this library has to
make a decision JavaScript never faces:

- an effect created **at top level** is owned by its handle — RAII, destroying
  the handle stops the effect;
- an effect created **inside** another effect or an `effect_scope` is owned by
  that parent, exactly like alien-signals' nested effects. Use `nsig::spawn`
  there, which makes ownership explicit and returns nothing to forget:

```cpp
auto outer = nsig::effect([&] {
    if (show()) {
        nsig::spawn([&] { std::println("{}", count()); });   // owned by outer
    }
});
```

Cleanups can also be registered imperatively, and run in reverse order:

```cpp
nsig::effect([&] {
    auto* h = subscribe(topic());
    nsig::on_cleanup([h] { unsubscribe(h); });
});
```

### effect_scope

```cpp
nsig::effect_scope scope{[&] {
    nsig::spawn([&] { /* … */ });
    nsig::spawn([&] { /* … */ });
}};
scope.stop();     // disposes children depth-first, in reverse creation order
```

### Batching and scheduling

```cpp
{
    nsig::batch b;      // effects flush once, when the outermost batch ends
    x = 1;
    y = 2;
}

nsig::batched([&] { x = 1; y = 2; });

nsig::set_scheduler([] { queue_microtask(nsig::flush); });   // defer flushing
```

### watch

`effect` re-runs whenever a dependency changed. `watch` runs its sink only when
the *value* it computes is different — the distinction Vue draws between
`watchEffect` and `watch`, and the one that matters at a hardware boundary:

```cpp
auto w = nsig::watch([&] { return status_colour(); },
                     [](Colour c) { set_led(c); });   // no write for an unchanged colour
```

It exists in both layers under the same name and returns an RAII handle. The
sink runs untracked, so reading a signal inside it creates no dependency.

### Lazy expressions

`#include <nsig/expr.hpp>` adds operators over nodes — for both graphs:

```cpp
auto load     = cpu + gpu;                      // instead of [&]{ return cpu() + gpu(); }
auto hot      = temp > 60.0f;
auto stalled  = armed && duty > 15 && rpm < 100;
auto colour   = nsig::pick(fault, red, nsig::pick(duty > 70, amber, green));

nsig::computed total{price * qty};              // an expression is just an invocable
```

An expression is lazily evaluated and does **not** cache — wrap it in
`nsig::computed` or `nsig::fixed::computed` when the value should be memoised, or hand
it to an effect or an output when it should drive something. It is closures over
references all the way down, so it inlines away: adding the sugar to the fan
controller benchmark moved its instruction count from 176.0 to 176.1 per sweep.

`&&` and `||` keep short-circuiting, unlike most overloaded versions — the
built-in operator still sits between the two reads inside the closure. `pick`
exists because `?:` cannot be overloaded, and it evaluates only the branch it
selects. `nsig::lift` turns an ordinary function into one that takes nodes, and
`nsig::make_array<N>` builds per-channel arrays, including of node types that
cannot be moved:

```cpp
constexpr auto clamp_duty = nsig::lift([](int v) { return std::clamp(v, 0, 100); });

auto duty = nsig::make_array<5>([](std::size_t i) {
    return nsig::computed<int>{clamp_duty(target_duty + trim[i])};
});
```

What is deliberately missing: there is no pipe syntax, no `operator=` wiring and
no lazy `if`. The moment a derivation needs a branch or a switch, a lambda is
clearer than any operator chain — a reactive DSL that tries to swallow control
flow ends up worse than the language it is hiding. In the fan controller the
sugar improves about half the derivations and the other half stay lambdas, which
is the honest split.

Lifetime: `signal` and `computed` are refcounted handles, so an expression copies
them and keeps their nodes alive. `nsig::fixed` nodes are the values themselves, so
an expression points at them — the same rule as a lambda capturing by reference.

### Adapters: ordinary functions, filters, driver I/O

`#include <nsig/adapt.hpp>`. Everything a microcontroller touches is an
imperative function; this is the bridge, and it draws one line that matters.

**Pure functions lift straight onto nodes.** `nsig::clamp`, `min`, `max`, `abs`,
`map_range`, or anything through `nsig::lift`:

```cpp
auto setpoint = nsig::fixed::computed(nsig::map_range(knob, 0, 4095, 20.0f, 90.0f));
auto duty     = nsig::clamp((coolant - setpoint) * 8.0f + 30.0f, 30.0f, 100.0f);
```

**Stateful blocks split in two, by idempotence.** A node may be re-evaluated
whenever the epoch moves — which happens on *any* write, not just to its own
input. So a derivation is only safe in the graph if evaluating it twice with the
same input gives the same answer:

| | | |
|---|---|---|
| `schmitt(temp, 28, 30)` | `f(f(x)) == f(x)` | safe as a `memo` |
| `peak(temp)` | idempotent | safe, but see `fold` below |
| `ema(temp, 0.2f)` | moves every call | **not** safe — it would drift |
| `slew(target, 5)` | moves every call | **not** safe |
| `debounce(button, 3)` | counts calls | **not** safe |

The second group belongs on the sampling edge, where it advances exactly once
per reading. That is what `sensor` is for — it wraps the `T read()` your driver
already exposes:

```cpp
auto coolant = nsig::fixed::sensor(ds18b20_read, nsig::ema{0.3f});
auto knob    = nsig::fixed::sensor(adc_read,     nsig::median3<int>{});
auto button  = nsig::fixed::sensor(read_pin,     nsig::debounce<bool>{3});

nsig::fixed::sample(coolant, knob, button);   // one batch, one pass over the outputs
```

Values that arrive by another route — an ISR counter, an MQTT payload — go in
through `push()` (filtered) or `force()` (raw).

**Running aggregates need `fold`, not `computed`.** A lazy `memo` is only
evaluated when something reads it, so a running maximum written that way reports
the peak of whichever samples it happened to be read on. `nsig::fixed::fold` registers with the watchers and therefore sees every cycle:

```cpp
auto worst = nsig::fixed::fold<float>(nsig::peak(coolant));      // right
auto wrong = nsig::fixed::computed<float>(nsig::peak(coolant));  // silently misses spikes
```

Use `computed` when the answer is a pure function of the current state, and
`fold` when it depends on the history — the name says which. There is a test that demonstrates the
difference rather than asserting it.

**Actuators are the other direction: a signal you assign, that drives a pin.**

```cpp
nsig::fixed::actuator relay{false, [](bool on) { gpio_set_level(GPIO_NUM_5, on); }};

relay = true;        // the pin goes high, right here
relay.toggle();
if (relay()) ...     // it is also an ordinary node, so rules can read it
relay.refresh();     // re-assert after a peripheral reset, without touching the graph
```

The driver call happens only when the value actually changes, so assigning the
same state twice costs a comparison — what you want for a pin, and what you need
for a relay or a valve. And `nsig::fixed::drive` hands it to the graph so it follows a
rule instead of an assignment:

```cpp
auto binding = nsig::fixed::drive(valve, cooling && !fault);
```

Both styles coexist: a command from MQTT can write the pin directly while other
pins follow expressions. That is the whole shape of a controller — `sensor` on
the way in, `actuator` on the way out, pure rules in between.

**Edges** are just outputs, since an output only runs on change:

```cpp
auto alarm = nsig::fixed::on_rising(coolant > 95.0f, [] { sound_alarm(); });
```

`examples/controller_io.cpp` puts the whole thing together — driver calls,
filters, commands arriving from MQTT, reactive rules, peripherals and telemetry
out — and runs as a host simulation under `ctest`.

### untracked / trigger

```cpp
nsig::untracked([&] { return config(); });      // read without subscribing

nsig::trigger([&] { src1(); src2(); });         // invalidate everything read here
```

### readonly

Expose reactive state from a class without handing out the setter:

```cpp
class Cart {
    nsig::signal<std::vector<Item>> items_{};
    nsig::computed<int> total_{[this] { return sum(items_()); }};
public:
    nsig::readonly<int> total() const { return total_; }
};
```

### Coroutines

`#include <nsig/async.hpp>`.

```cpp
// Resume when a signal next changes
co_await nsig::changed(count);

// Resume when a reactive predicate becomes true (immediately if it already is)
co_await nsig::until([&] { return ready() && !loading(); });
```

`nsig::resource<T>` is an async derivation with reactive `loading`/`value`/
`error`. The reactive part and the async part are separate on purpose — a lazy
coroutine registers no dependencies, so tracking the fetcher directly would be
silently wrong:

```cpp
nsig::signal<int> user_id{1};

auto user = nsig::make_resource(
    [&] { return user_id(); },                       // tracked, cheap
    [](int id) -> nsig::task<User> {                 // async, never tracked
        co_return co_await http_get(id);
    });

auto view = nsig::effect([&] {
    if (user.loading())      show_spinner();
    else if (user.error())   show_error(user.error());
    else if (user.value())   show(*user.value());
});

user_id = 2;   // refetches; a stale in-flight response is discarded
```

Resumptions are queued and run *after* the effect flush completes, so a
coroutine can never re-enter the scheduler mid-propagation.

### Diagnostics

```cpp
auto s = nsig::get_stats();   // links_created, node_updates, effect_runs,
                              // link_pool_reserved
```

## Embedded targets (ESP32 / ESP-IDF)

The whole point of a fine-grained reactive graph is doing less work, which is
worth more on a microcontroller than on a desktop: a recomputation avoided is a
register write avoided and a radio packet avoided.

`examples/esp32_fan_controller.cpp` is a five-channel PWM fan controller for an
ESP32-C6 — temperature curve with hysteresis, per-channel trim and stall
detection, MQTT telemetry — and it also builds as a host simulation, so the
reactive logic is exercised by `ctest` with the embedded flags on.

What the target build needs:

```cmake
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++23)
target_compile_definitions(${COMPONENT_LIB} PRIVATE NSIG_THREAD_LOCAL=)
```

- **No exceptions, no RTTI.** `-fno-exceptions -fno-rtti` is supported and
  tested; the graph never needs exceptions itself, it only guards user
  callbacks that might throw.
- **One runtime, one task.** Define `NSIG_THREAD_LOCAL` to nothing so the
  runtime is a single global, and touch signals from exactly one task. ISRs
  must not enter the graph — have them bump a counter or post to a queue, and
  let the control task write the signal. `nsig::set_scheduler` covers the
  variant where other tasks write directly: point it at
  `xTaskNotifyGive(control_task)` and no flush runs on a writer's stack.
- **No steady-state allocation.** Call `nsig::reserve(node_bytes, links,
  queued_effects)` once at boot; after that the pools serve everything and the
  heap is never touched again. `nsig::get_stats()` confirms it.
- **Deadbands belong in the equality policy.** A DS18B20 in 12-bit mode dithers
  by a sixteenth of a degree; a `signal<float, deadband<250>>` stops that from
  reaching the graph at all, rather than filtering it at every consumer.

### What the graph costs, and how to get to parity

`bench/embedded.cpp` runs the fan controller three ways — the dynamic graph, the
statically wired one, and a hand-written imperative controller with the same
deadbands, the same hysteresis, the same "only write the register if it changed"
discipline *and* an early-out when no input moved. All three are asserted to
emit identical board operations before any of them is timed.

Per sweep: 7 inputs written, every output settled, over a workload where most
sweeps see only sensor noise.

| | dynamic graph | **static graph** | hand-written |
|---|---|---|---|
| instructions (callgrind, ISA-neutral) | 325 | **176** | 169 |
| — ratio | 1.92x | **1.04x** | 1.00x |
| wall time, mixed workload | 30–32 ns | **18–19 ns** | 16–17 ns |
| — ratio | 1.83–2.01x | **1.07–1.17x** | 1.00x |
| quiet sweep (nothing crossed a deadband) | 11.0 ns | **7.0 ns** | 6.0 ns |

The dynamic graph costs about +156 instructions per control cycle. On a 160 MHz
ESP32-C6 that is a couple of microseconds per fully-settling sweep — against a
1 Hz loop whose DS18B20 conversion alone takes 750 ms, below the noise floor.
The profile says almost all of it is the write path: a signal is a heap node
behind a pointer, so the optimiser reloads it on every access and cannot keep it
in a register.

`nsig::fixed` (`<nsig/static_graph.hpp>`) removes exactly that while keeping the
same shape — write a cell, the outputs run:

```cpp
nsig::fixed::signal temp{25.0f};

auto pwm = nsig::fixed::watch([&] { return duty_for(temp()); },
                            [](int d) { set_pwm(d); });

temp = 45.0f;   // pwm runs, if and only if the duty changed

{ nsig::fixed::batch b;  temp = 45.0f;  rpm[0] = 1200; }   // one pass
```

The two layers share one vocabulary: `signal`, `computed`, `effect`, `watch`,
`batch` mean the same thing in `nsig` and in `nsig::fixed`, so moving a
controller between them is mostly a namespace change. Handles are RAII in both.

Underneath it does not track dependencies at all. One global epoch counter is
bumped by every write; an expression is re-evaluated when the epoch has moved
since it was last looked at, and its result is compared with what it produced
before. That is what makes it both simpler and faster for a small fixed graph:
no dependency lists, no per-node version arrays, nothing type-erased or
allocated, so the compiler inlines the whole graph. Closures work normally,
including branches that read different cells on different runs — which the
dynamic graph has to re-link for.

Note the two ratios disagree: 1.04x in instructions but 1.07–1.17x in wall time.
The difference is the one indirect call per output that automatic execution
needs — it costs cycles rather than instructions. Driving the outputs yourself
(`output::stop()`, then `poll()` from your loop) buys that back and lands at
1.05x wall time, at the price of the thing you actually wanted.

The real price is elsewhere: a write anywhere re-evaluates every registered
output once, so work per changed cycle is O(outputs), not O(dirty subgraph). For
a controller with a dozen arithmetic derivations that is cheaper than the
bookkeeping it replaces. When it stops being true, `nsig::signal` is the right
tool.

One line does most of the work: the epoch check at the top of the flush. A cycle
in which no input moved costs one comparison for the whole graph. An earlier
version of this layer tracked dependencies properly with per-node version stamps
and no epoch counter, and measured **nine times slower** than the push-pull
graph on a quiet sweep, because a pull-only design has no way to know that
nothing has happened. That is the problem push-pull exists to solve; the epoch
counter buys it back for a graph whose shape is fixed.

What you give up: `memo` and `output` stored as class members need one
`decltype` on a factory function, because their type contains a closure — build
the graph as locals in the control task, which is the natural shape anyway, and
that disappears. There is no `effect_scope`, no scheduler and no coroutine
layer here.

Rule of thumb: `nsig::signal` when the graph's shape is discovered at run time
(a UI, a document, a scene) or when the dirty subgraph is much smaller than the
whole; `nsig::fixed` when it is fixed, small and the loop is hot. Neither belongs
inside an ISR or a control loop above ~10 kHz. On ESP32 also watch
instruction-cache pressure before instruction count — a flash cache miss costs
far more than these instructions do.

Footprint, GCC `-Os`:

| | |
|---|---|
| code, signal + computed + effect | ~9 KB |
| runtime state (static) | 368 B |
| `signal<int>` / `computed<int>` node | 64 B each |
| effect node | 96 B |
| dependency edge | 56 B |

The fan controller's entire graph — 15 signals, 13 computeds, 16 effects, ~86
edges — is roughly 8 KB of RAM.

## Design notes

**Ownership without a GC.** Every graph edge points subscriber → dependency,
and a `link` holds a strong reference to its `dep` only. The strong-reference
graph is therefore a DAG — no cycles, so plain non-atomic refcounting is
sufficient and no tracing collector is needed. Handles hold references too, so
a `signal` captured in a lambda keeps its node alive, which is the JavaScript
mental model preserved without JavaScript's runtime.

**Two pools.** `link` is trivially copyable and allocated in 512-element chunks
with a free list; nodes go through segregated free lists over 64 KiB chunks.
Between them, the whole 42-test conformance suite performs 134 heap
allocations.

**Dispatch.** A node carries a 3-entry vtable pointer (`update` / `run` /
`destroy`) rather than a C++ vtable, so the core dispatches on `node_kind`
first and only pays an indirect call where the user's own code has to run
anyway. `sizeof(node)` stays at 48 bytes.

**Algorithmic fidelity.** `propagate`, `check_dirty`, `shallow_propagate`,
`link` and `unlink` are direct translations of `alien-signals/src/system.ts`,
keeping its constraints: no `Array`/`Set`/`Map` in the hot path and no
recursion — traversal uses an explicit stack with 32 inline slots and heap
spill beyond that.

**Threading.** All runtime state is `thread_local` and constant-initialised, so
each thread gets an independent graph with no locking and no TLS guard variable
on access. Nodes must not be shared across threads; define
`NSIG_THREAD_LOCAL` to nothing for a single-threaded build.

## Layout

```
include/nsig/core.hpp          graph algorithm, pools, refcounting
include/nsig/signals.hpp       signal/computed/effect/scope/batch
include/nsig/static_graph.hpp  compile-time wired graph (nsig::fixed)
include/nsig/expr.hpp          operator sugar for both graphs
include/nsig/adapt.hpp         driver adapters, filters, lifted math
include/nsig/async.hpp         task/changed/until/resource
single_include/nsig.hpp     amalgamation (tools/amalgamate.py)
src/nsig.cppm               `import nsig;`
tests/                      conformance + async + single-header
bench/                      propagate.cpp / propagate.mjs
```

## Contributing

Issues and pull requests are welcome. Two ground rules the codebase relies on:

- Anything that changes propagation must keep `tests/conformance.cpp` green —
  it is the port of alien-signals' own suite and defines the semantics.
- Anything that touches the fixed graph must keep
  `bench/embedded.cpp --verify` green — it asserts that the reactive and
  hand-written controllers emit identical board operations.

`ctest --test-dir build` runs both, plus the sanitizer and single-header jobs
CI runs on every push.

## License

MIT. The core algorithm derives from alien-signals (MIT, © Johnson Chu).
