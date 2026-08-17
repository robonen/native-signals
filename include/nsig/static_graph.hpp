// native-signals — the statically wired graph.
//
// Same shape as signals.hpp — write a cell, the outputs run — but built for a
// control loop instead of a UI:
//
//     nsig::fixed::signal temp{25.0f};                       // ~ nsig::signal
//     auto duty = nsig::fixed::computed([&] { return duty_for(temp()); });   // ~ computed
//     auto pwm  = nsig::fixed::watch([&] { return duty(); },
//                                 [](int d) { set_pwm(d); });         // ~ effect
//
//     temp = 45.0f;    // pwm runs, if and only if the duty changed
//
//     { nsig::fixed::batch b;  temp = 45.0f;  rpm[0] = 1200; }   // one pass
//
// The difference is underneath. The dynamic graph discovers dependencies at run
// time, which costs a heap node per value, an indirect call per evaluation, and
// a pointer the optimiser cannot see through. This layer does not track
// dependencies at all: one global epoch counter is bumped by every write, an
// expression is re-evaluated when the epoch has moved since it was last looked
// at, and its result is compared with what it produced before.
//
// Not tracking dependencies is what makes this both simpler and faster for a
// small fixed graph. Nothing is type-erased and nothing is allocated, so the
// compiler inlines the whole graph into the caller; closures work normally,
// including branches that read different cells on different runs, which the
// dynamic graph has to re-link for.
//
// The price: a write anywhere re-evaluates every registered output once, so
// work per changed cycle is O(outputs), not O(dirty subgraph). For a controller
// with a dozen arithmetic derivations that is cheaper than the bookkeeping it
// replaces. When it stops being true, `nsig::signal` is the right tool.
//
// Cycles in which nothing was written cost one comparison for the whole graph.
#pragma once

#include "signals.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace nsig::fixed {

using version_t = std::uint32_t;

namespace detail {

/// Intrusive registry of live outputs, kept in creation order.
struct pollable {
    void (*run)(pollable*) = nullptr;
    pollable* prev = nullptr;
    pollable* next = nullptr;
};

struct registry {
    pollable* head = nullptr;
    pollable* tail = nullptr;
    version_t epoch = 1;
    version_t flushed = 0;
    std::uint32_t batch_depth = 0;

    constexpr registry() noexcept = default;
};

inline constinit NSIG_THREAD_LOCAL registry g_reg{};

inline void attach(pollable* p) noexcept {
    p->prev = g_reg.tail;
    p->next = nullptr;
    if (g_reg.tail != nullptr) g_reg.tail->next = p;
    else g_reg.head = p;
    g_reg.tail = p;
}

inline void detach(pollable* p) noexcept {
    if (p->prev != nullptr) p->prev->next = p->next;
    else if (g_reg.head == p) g_reg.head = p->next;
    if (p->next != nullptr) p->next->prev = p->prev;
    else if (g_reg.tail == p) g_reg.tail = p->prev;
    p->prev = p->next = nullptr;
}

/// Runs every output whose value changed. Loops because a sink may write a
/// cell; the bound keeps a pathological feedback cycle from hanging the loop.
NSIG_NOINLINE inline void flush_now() {
    for (int guard = 0; guard < 16 && g_reg.flushed != g_reg.epoch; ++guard) {
        g_reg.flushed = g_reg.epoch;
        for (pollable* p = g_reg.head; p != nullptr;) {
            pollable* next = p->next;  // a sink may detach or destroy itself
            p->run(p);
            p = next;
        }
    }
}

NSIG_ALWAYS_INLINE void notify_write() {
    ++g_reg.epoch;
    if (g_reg.batch_depth == 0) flush_now();
}

}  // namespace detail

/// The current epoch. Compare against a saved copy to answer "has anything at
/// all changed?" without touching a single node.
[[nodiscard]] inline version_t epoch() noexcept { return detail::g_reg.epoch; }

/// Runs any pending outputs now.
inline void flush() { detail::flush_now(); }

/// Defers outputs until the outermost batch ends, so a whole sweep of sensor
/// updates produces one pass over the peripherals.
class [[nodiscard]] batch {
    bool active_ = true;

public:
    batch() noexcept { ++detail::g_reg.batch_depth; }
    batch(const batch&) = delete;
    batch& operator=(const batch&) = delete;
    ~batch() {
        if (active_ && --detail::g_reg.batch_depth == 0) detail::flush_now();
    }
    void commit() {
        if (!std::exchange(active_, false)) return;
        if (--detail::g_reg.batch_depth == 0) detail::flush_now();
    }
};

template <std::invocable F>
decltype(auto) batched(F&& fn) {
    batch b;
    return std::forward<F>(fn)();
}

// ---------------------------------------------------------------------------
// cell — a source value
// ---------------------------------------------------------------------------

template <class T, class Eq = auto_equal>
class signal {
    T value_;
    [[no_unique_address]] Eq eq_{};

public:
    using value_type = T;

    constexpr signal()
        requires std::default_initializable<T>
    = default;
    constexpr explicit signal(T v) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(v)) {}

    [[nodiscard]] constexpr const T& get() const noexcept { return value_; }
    [[nodiscard]] constexpr const T& operator()() const noexcept { return value_; }

    /// Converts only when the value differs, so a redundant write of a
    /// heavyweight type costs one comparison. Equality is where deadbands live:
    /// `cell<float, deadband<250>>` keeps sensor jitter out of the graph.
    template <class U = T>
        requires std::constructible_from<T, U &&>
    constexpr void set(U&& v) {
        if (::nsig::detail::equal_to<T>(eq_, value_, v)) return;
        value_ = T(std::forward<U>(v));
        detail::notify_write();
    }

    template <class U>
        requires(!std::same_as<std::remove_cvref_t<U>, signal> && std::constructible_from<T, U &&>)
    constexpr signal& operator=(U&& v) {
        set(std::forward<U>(v));
        return *this;
    }

    /// Grants mutable access and notifies unconditionally.
    template <class F>
        requires std::invocable<F&, T&>
    constexpr void mutate(F&& fn) {
        std::forward<F>(fn)(value_);
        detail::notify_write();
    }
    constexpr void touch() { detail::notify_write(); }
};

template <class T>
signal(T) -> signal<T>;

namespace detail {

/// Evaluates the expression, passing the previous value when it asks for one:
/// `fn(const T* previous)`, `nullptr` on the first evaluation — the incremental
/// form, for hysteresis and running aggregates.
template <class F, class T>
[[nodiscard]] constexpr T evaluate(F& fn, const std::optional<T>& previous) {
    if constexpr (std::invocable<F&, const T*>)
        return static_cast<T>(fn(previous ? &*previous : nullptr));
    else
        return static_cast<T>(fn());
}

}  // namespace detail

// ---------------------------------------------------------------------------
// memo — a cached intermediate
// ---------------------------------------------------------------------------

/// Evaluates at most once per epoch and caches the result.
///
/// Worth it when an intermediate is expensive or read by several outputs in the
/// same cycle. Cheap arithmetic shared by two outputs usually is not — write
/// the expression twice and let the compiler common it up.
template <class T, class F, class Eq = auto_equal>
class computed_node {
    [[no_unique_address]] mutable F fn_;
    [[no_unique_address]] Eq eq_{};
    mutable std::optional<T> value_{};
    mutable version_t checked_ = 0;

public:
    using value_type = T;

    constexpr explicit computed_node(F fn) : fn_(std::move(fn)) {}

    [[nodiscard]] constexpr const T& get() const {
        if (checked_ != detail::g_reg.epoch) {
            checked_ = detail::g_reg.epoch;
            T next = detail::evaluate<F, T>(fn_, value_);
            if (!value_ || !eq_(*value_, next)) value_ = std::move(next);
        }
        return *value_;
    }
    [[nodiscard]] constexpr const T& operator()() const { return get(); }
};

/// `memo([&] { return a() + b(); })` deduces the value type;
/// `memo<int>([](const int* previous) { ... })` names it, which is how the
/// incremental form is spelled. A factory rather than CTAD, because only a
/// function template lets you fix `T` and still deduce the closure type.
template <class T = void, class Eq = auto_equal, class F>
[[nodiscard]] constexpr auto computed(F fn) {
    if constexpr (std::is_void_v<T>) {
        static_assert(std::invocable<F&>,
                      "an incremental expression needs its value type: computed<T>(fn)");
        return computed_node<std::remove_cvref_t<std::invoke_result_t<F&>>, F, Eq>(std::move(fn));
    } else {
        return computed_node<T, F, Eq>(std::move(fn));
    }
}

// ---------------------------------------------------------------------------
// tracked — a node that is evaluated every cycle
// ---------------------------------------------------------------------------

/// Like `memo`, but registered with the outputs so it advances on every write
/// instead of only when somebody reads it.
///
/// That distinction matters for anything that accumulates. A running maximum
/// written as a `memo` is silently wrong: being lazy, it only ever sees the
/// samples it happened to be read on, so it reports the peak of those rather
/// than of the signal. As a `tracked` it sees every cycle.
///
///     auto worst = nsig::fixed::fold<float>(nsig::peak(coolant));
///
/// Use `memo` for derivations that are pure functions of the current state, and
/// `tracked` when the answer depends on the history of values.
template <class T, class F, class Eq = auto_equal>
class fold_node : detail::pollable {
    [[no_unique_address]] mutable F fn_;
    [[no_unique_address]] Eq eq_{};
    mutable std::optional<T> value_{};
    mutable version_t checked_ = 0;

    static void run_(detail::pollable* p) { static_cast<fold_node*>(p)->refresh(); }

public:
    using value_type = T;

    explicit fold_node(F fn) : fn_(std::move(fn)) {
        this->run = &run_;
        detail::attach(this);
        refresh();
    }
    fold_node(const fold_node&) = delete;
    fold_node& operator=(const fold_node&) = delete;
    fold_node(fold_node&&) = delete;
    fold_node& operator=(fold_node&&) = delete;
    ~fold_node() { detail::detach(this); }

    [[nodiscard]] const T& get() const {
        refresh();
        return *value_;
    }
    [[nodiscard]] const T& operator()() const { return get(); }

private:
    void refresh() const {
        if (checked_ == detail::g_reg.epoch) return;
        checked_ = detail::g_reg.epoch;
        T next = detail::evaluate<F, T>(fn_, value_);
        if (!value_ || !eq_(*value_, next)) value_ = std::move(next);
    }
};

/// `tracked([&]{ ... })` deduces the value type; `tracked<T>(fn)` names it,
/// which is how the incremental form is spelled.
template <class T = void, class Eq = auto_equal, class F>
[[nodiscard]] auto fold(F fn) {
    if constexpr (std::is_void_v<T>) {
        static_assert(std::invocable<F&>,
                      "an incremental expression needs its value type: fold<T>(fn)");
        return fold_node<std::remove_cvref_t<std::invoke_result_t<F&>>, F, Eq>(std::move(fn));
    } else {
        return fold_node<T, F, Eq>(std::move(fn));
    }
}

// ---------------------------------------------------------------------------
// output — the boundary where values leave the graph
// ---------------------------------------------------------------------------

/// Calls `sink(value)` when, and only when, the expression's value changes.
///
/// Registers itself on construction and runs on every write, exactly like an
/// `nsig::effect`. This is where change detection has to happen, and the one
/// place it earns its keep: `sink` is a PWM register, a radio packet, a log
/// line.
///
/// Non-movable, because the registry holds its address. `auto pwm =
/// nsig::fixed::watch(...)` still works: the factory returns a prvalue, so the
/// object is constructed directly in place.
template <class T, class F, class Sink, class Eq = auto_equal>
class watch_node : detail::pollable {
    [[no_unique_address]] F fn_;
    [[no_unique_address]] Sink sink_;
    [[no_unique_address]] Eq eq_{};
    std::optional<T> last_{};
    version_t checked_ = 0;

    static void run_(detail::pollable* p) { static_cast<watch_node*>(p)->poll(); }

public:
    using value_type = T;

    watch_node(F fn, Sink sink) : fn_(std::move(fn)), sink_(std::move(sink)) {
        this->run = &run_;
        detail::attach(this);
        poll();
    }
    watch_node(const watch_node&) = delete;
    watch_node& operator=(const watch_node&) = delete;
    watch_node(watch_node&&) = delete;
    watch_node& operator=(watch_node&&) = delete;
    ~watch_node() { detail::detach(this); }

    /// Re-evaluates if the epoch moved; returns true if the sink ran. Called
    /// for you on every write — reach for it directly only when driving the
    /// graph from a loop that owns the timing.
    bool poll() {
        if (checked_ == detail::g_reg.epoch) return false;
        checked_ = detail::g_reg.epoch;
        T next = detail::evaluate<F, T>(fn_, last_);
        if (last_ && eq_(*last_, next)) return false;
        last_ = std::move(next);
        sink_(*last_);
        return true;
    }

    [[nodiscard]] const std::optional<T>& last() const noexcept { return last_; }

    /// Forces the next run to emit — for re-establishing external state, such
    /// as republishing everything after a broker reconnects.
    void invalidate() noexcept {
        last_.reset();
        checked_ = 0;
    }

    /// Stops this output running on writes; `poll()` still works.
    void stop() noexcept { detail::detach(this); }
};

/// `output([&] { return duty(); }, [](int d) { set_pwm(d); })`.
template <class T = void, class Eq = auto_equal, class F, class Sink>
[[nodiscard]] auto watch(F fn, Sink sink) {
    if constexpr (std::is_void_v<T>) {
        static_assert(std::invocable<F&>,
                      "an incremental expression needs its value type: watch<T>(fn, sink)");
        return watch_node<std::remove_cvref_t<std::invoke_result_t<F&>>, F, Sink, Eq>(
            std::move(fn), std::move(sink));
    } else {
        return watch_node<T, F, Sink, Eq>(std::move(fn), std::move(sink));
    }
}

}  // namespace nsig::fixed

/// A short alias, because the qualified name appears on every line of a
/// controller. `nsig::fixed::signal` and `nsig::fixed::signal` are the same type.
namespace nsig { namespace st = fixed; }
