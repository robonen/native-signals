// native-signals — adapters between ordinary C++ and the graph.
//
// Two different problems live here, and keeping them apart is the whole point.
//
// 1. PURE functions of their inputs — `std::clamp`, a lookup table, a scaling
//    formula. These lift straight onto nodes and stay memoisable.
//
// 2. STATEFUL blocks — a filter, a rate limiter, a debouncer. These look like
//    they belong in the graph, and putting them there is a bug.
//
// The rule is idempotence. A node may be re-evaluated whenever the epoch moves,
// which happens on *any* write, not just to its own input. So a derivation is
// only safe in the graph if evaluating it twice with the same input gives the
// same answer:
//
//     schmitt(temp, 28, 30)   f(f(x)) == f(x)   safe as a memo
//     peak(temp)              idempotent        safe as a memo
//     ema(temp, 0.2f)         moves every call  NOT safe — it would drift
//     slew(target, 5)         moves every call  NOT safe
//     debounce(button, 3)     counts calls      NOT safe
//
// Anything in the second group belongs on the sampling edge, where it advances
// exactly once per reading. That is what `sampler` is for: it wraps the plain
// `T read()` your driver already exposes, applies the filter, and writes the
// result into a signal — once per `sample()`, no matter what else the graph did.
#pragma once

#include "expr.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace nsig {

// ---------------------------------------------------------------------------
// Pure functions lifted onto nodes
// ---------------------------------------------------------------------------

/// `nsig::clamp(target + trim, 0, 100)` — any argument may be a node or a plain
/// value.
template <class X, class Lo, class Hi>
    requires(readable<X> || readable<Lo> || readable<Hi>)
[[nodiscard]] constexpr auto clamp(X&& x, Lo&& lo, Hi&& hi) {
    return expr([cx = detail::capture<X>(std::forward<X>(x)),
                 clo = detail::capture<Lo>(std::forward<Lo>(lo)),
                 chi = detail::capture<Hi>(std::forward<Hi>(hi))] {
        auto v = detail::read(cx);
        const auto l = detail::read(clo);
        const auto h = detail::read(chi);
        if (v < l) return static_cast<decltype(v)>(l);
        if (h < v) return static_cast<decltype(v)>(h);
        return v;
    });
}

template <class A, class B>
    requires(readable<A> || readable<B>)
[[nodiscard]] constexpr auto min(A&& a, B&& b) {
    return expr([ca = detail::capture<A>(std::forward<A>(a)),
                 cb = detail::capture<B>(std::forward<B>(b))] {
        auto x = detail::read(ca);
        auto y = detail::read(cb);
        return y < x ? static_cast<decltype(x)>(y) : x;
    });
}

template <class A, class B>
    requires(readable<A> || readable<B>)
[[nodiscard]] constexpr auto max(A&& a, B&& b) {
    return expr([ca = detail::capture<A>(std::forward<A>(a)),
                 cb = detail::capture<B>(std::forward<B>(b))] {
        auto x = detail::read(ca);
        auto y = detail::read(cb);
        return x < y ? static_cast<decltype(x)>(y) : x;
    });
}

template <class A>
    requires readable<A>
[[nodiscard]] constexpr auto abs(A&& a) {
    return expr([ca = detail::capture<A>(std::forward<A>(a))] {
        auto v = detail::read(ca);
        return v < decltype(v){} ? static_cast<decltype(v)>(-v) : v;
    });
}

/// Linear rescale, the calibration curve every analogue input needs.
/// `nsig::map_range(adc, 0, 4095, 0.0f, 3.3f)`
template <class X, class A, class B, class C, class D>
    requires readable<X>
[[nodiscard]] constexpr auto map_range(X&& x, A in_lo, B in_hi, C out_lo, D out_hi) {
    return expr([cx = detail::capture<X>(std::forward<X>(x)), in_lo, in_hi, out_lo, out_hi] {
        const auto t = (static_cast<double>(detail::read(cx)) - static_cast<double>(in_lo)) /
                       (static_cast<double>(in_hi) - static_cast<double>(in_lo));
        return static_cast<C>(static_cast<double>(out_lo) +
                              t * (static_cast<double>(out_hi) - static_cast<double>(out_lo)));
    });
}

// ---------------------------------------------------------------------------
// Idempotent stateful derivations — safe inside the graph
// ---------------------------------------------------------------------------

namespace detail {

/// Wraps `fn(previous, inputs...)` so it can be handed to `memo`/`computed`.
template <class T, class F, class... Caps>
struct incremental {
    [[no_unique_address]] F fn;
    std::tuple<Caps...> caps;

    T operator()(const T* previous) const {
        return std::apply([&](const Caps&... cs) { return fn(previous, read(cs)...); }, caps);
    }
};

}  // namespace detail

/// A Schmitt trigger: goes true above `high`, false below `low`, and holds in
/// between. Idempotent — re-evaluating with an unchanged input cannot flip it.
///
///     auto fans_on = nsig::fixed::computed<bool>(nsig::schmitt(temp, 28.0f, 30.0f));
template <class X, class L, class H>
    requires readable<X>
[[nodiscard]] constexpr auto schmitt(X&& x, L low, H high) {
    return [cx = detail::capture<X>(std::forward<X>(x)), low, high](const bool* previous) {
        const auto v = detail::read(cx);
        if (v >= high) return true;
        if (v <= low) return false;
        return previous != nullptr && *previous;
    };
}

/// The running maximum. Idempotent.
template <class X>
    requires readable<X>
[[nodiscard]] constexpr auto peak(X&& x) {
    using T = std::remove_cvref_t<decltype(std::declval<const std::remove_cvref_t<X>&>().get())>;
    return [cx = detail::capture<X>(std::forward<X>(x))](const T* previous) -> T {
        const T v = detail::read(cx);
        return previous != nullptr && *previous > v ? *previous : v;
    };
}

/// The running minimum. Idempotent.
template <class X>
    requires readable<X>
[[nodiscard]] constexpr auto trough(X&& x) {
    using T = std::remove_cvref_t<decltype(std::declval<const std::remove_cvref_t<X>&>().get())>;
    return [cx = detail::capture<X>(std::forward<X>(x))](const T* previous) -> T {
        const T v = detail::read(cx);
        return previous != nullptr && *previous < v ? *previous : v;
    };
}

// ---------------------------------------------------------------------------
// Non-idempotent filters — for the sampling edge only
// ---------------------------------------------------------------------------

/// Exponential moving average. `alpha` is the weight of the new reading.
struct ema {
    float alpha;
    template <class T>
    [[nodiscard]] constexpr T operator()(const T& previous, const T& sample) const {
        return static_cast<T>(previous + alpha * (sample - previous));
    }
};

/// Rate limiter: the value moves at most `step` per sample. Turns a step change
/// into a ramp — a soft-start for a fan or a valve.
template <class Step>
struct slew {
    Step step;
    template <class T>
    [[nodiscard]] constexpr T operator()(const T& previous, const T& sample) const {
        const auto delta = sample - previous;
        if (delta > step) return static_cast<T>(previous + step);
        if (delta < -step) return static_cast<T>(previous - step);
        return sample;
    }
};

template <class Step>
slew(Step) -> slew<Step>;

/// Only accepts a new value after it has been read `count` times in a row —
/// the classic contact debouncer, and equally good against a flapping sensor.
template <class T>
class debounce {
    T candidate_{};
    std::uint32_t seen_ = 0;
    std::uint32_t count_;

public:
    constexpr explicit debounce(std::uint32_t count) : count_(count) {}

    [[nodiscard]] constexpr T operator()(const T& previous, const T& sample) {
        if (sample == previous) {
            seen_ = 0;
            return previous;
        }
        if (sample != candidate_) {
            candidate_ = sample;
            seen_ = 1;
            return previous;
        }
        return ++seen_ >= count_ ? sample : previous;
    }
};

/// Median of the last three readings: removes single-sample spikes without the
/// lag of an average. The obvious first defence for a noisy ADC.
template <class T>
class median3 {
    T a_{}, b_{};
    bool primed_ = false;

public:
    [[nodiscard]] constexpr T operator()(const T& /*previous*/, const T& sample) {
        if (!primed_) {
            a_ = b_ = sample;
            primed_ = true;
            return sample;
        }
        const T x = a_, y = b_, z = sample;
        a_ = b_;
        b_ = sample;
        return x < y ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y));
    }
};

/// Passes the reading through unchanged — the default for `sampler`.
struct passthrough {
    template <class T>
    [[nodiscard]] constexpr const T& operator()(const T&, const T& sample) const {
        return sample;
    }
};

}  // namespace nsig

// ===========================================================================
// The device seams live in `nsig::fixed`, because that is what they are built
// on: a `sensor` holds a `fixed::signal` and an `actuator` notifies the fixed
// graph. The run-time graph already has the equivalents — a sensor there is a
// `nsig::signal` you assign from a driver read, and an actuator is a
// `nsig::watch` whose sink writes the register.
// ===========================================================================

namespace nsig::fixed {

// ---------------------------------------------------------------------------
// sampler — the bridge from a driver call into the graph
// ---------------------------------------------------------------------------

/// Wraps the `T read()` your driver already has. `sample()` calls it, runs the
/// filter, and writes the result into a cell — exactly once per call, whatever
/// else the graph is doing.
///
///     auto temp = nsig::sensor(ds18b20_read, nsig::ema{0.2f});
///     ...
///     temp.sample();      // once per control cycle
///     temp()              // read it like any other node
template <class Read, class Filter = passthrough, class Eq = auto_equal>
class sensor {
public:
    using value_type = std::remove_cvref_t<std::invoke_result_t<Read&>>;

private:
    [[no_unique_address]] mutable Read read_;
    [[no_unique_address]] mutable Filter filter_;
    st::signal<value_type, Eq> value_;

public:
    explicit sensor(Read read, Filter filter = {})
        : read_(std::move(read)), filter_(std::move(filter)), value_(read_()) {}

    /// Takes one reading. Call it from the control loop, inside a batch with
    /// the other samplers.
    void sample() { value_.set(filter_(value_.get(), read_())); }

    [[nodiscard]] const value_type& get() const noexcept { return value_.get(); }
    [[nodiscard]] const value_type& operator()() const noexcept { return value_.get(); }

    /// For a reading that arrived by some other route — an ISR counter, an MQTT
    /// message — with the same filtering applied.
    void push(const value_type& v) { value_.set(filter_(value_.get(), v)); }

    /// Escape hatch for the rare case where the graph should see a value with
    /// no filtering at all.
    void force(const value_type& v) { value_.set(v); }
};

template <class Read, class Filter>
sensor(Read, Filter) -> sensor<Read, Filter>;
template <class Read>
sensor(Read) -> sensor<Read>;

/// Takes one reading from every sensor inside a single batch, so a whole sweep
/// produces one pass over the outputs.
///
///     nsig::sample(temp, humidity, rpm0, rpm1);
template <class... S>
void sample(S&... sensors) {
    batch guard;
    (sensors.sample(), ...);
}

// ---------------------------------------------------------------------------
// actuator — a cell that writes through to hardware
// ---------------------------------------------------------------------------

/// The other direction from `sampler`: assigning it drives the peripheral.
///
///     nsig::actuator relay{false, [](bool on) { gpio_set_level(GPIO_NUM_5, on); }};
///
///     relay = true;          // pin goes high, right here
///     relay.toggle();
///     if (relay()) ...       // it is also an ordinary node
///
/// The driver call happens only when the value actually changes, so assigning
/// the same state twice costs a comparison — which is what you want for a pin,
/// and what you need for a relay or a valve.
///
/// It is a node like any other, so other rules can depend on the commanded
/// state: `auto pump_needed = cooling && !relay();`
///
/// Instead of assigning it by hand, `nsig::drive` binds it to an expression and
/// it follows the graph.
template <class T, class Write, class Eq = auto_equal>
class actuator {
    T value_;
    [[no_unique_address]] mutable Write write_;
    [[no_unique_address]] Eq eq_{};

public:
    using value_type = T;

    actuator(T initial, Write write) : value_(std::move(initial)), write_(std::move(write)) {
        write_(value_);  // leave the peripheral in a known state
    }

    [[nodiscard]] const T& get() const noexcept { return value_; }
    [[nodiscard]] const T& operator()() const noexcept { return value_; }

    template <class U = T>
        requires std::constructible_from<T, U &&>
    void set(U&& v) {
        if (::nsig::detail::equal_to<T>(eq_, value_, v)) return;
        value_ = T(std::forward<U>(v));
        write_(value_);
        detail::notify_write();
    }

    template <class U>
        requires(!std::same_as<std::remove_cvref_t<U>, actuator> &&
                 std::constructible_from<T, U &&>)
    actuator& operator=(U&& v) {
        set(std::forward<U>(v));
        return *this;
    }

    void toggle()
        requires std::same_as<T, bool>
    {
        set(!value_);
    }

    /// Re-asserts the current value on the hardware without touching the graph
    /// — after a peripheral reset, a brown-out, or an expander reconnecting.
    void refresh() const { write_(value_); }
};

template <class T, class Write>
actuator(T, Write) -> actuator<T, Write>;

/// Binds an actuator to an expression: it now follows the graph.
///
///     auto b = nsig::drive(relay, cooling && !fault);
///
/// Keep the returned handle alive — it is an `output`, and dropping it unbinds.
/// Assigning the actuator by hand while it is bound works, but the next
/// evaluation puts the expression back in charge.
template <class A, class Expr>
[[nodiscard]] auto drive(A& target, Expr&& e) {
    // Qualified: an unqualified call would also find `nsig::watch` by ADL on
    // the expression type, and the two overloads are equally viable.
    return ::nsig::fixed::watch(std::forward<Expr>(e),
                      [&target](const typename A::value_type& v) { target.set(v); });
}

// ---------------------------------------------------------------------------
// Edge helpers
// ---------------------------------------------------------------------------

/// Runs `fn` when the expression becomes true. An `output` already fires only on
/// change, so a `true` arriving *is* the rising edge.
template <class Expr, class Fn>
[[nodiscard]] auto on_rising(Expr&& e, Fn fn) {
    // Qualified: an unqualified call would also find `nsig::watch` by ADL on
    // the expression type, and the two overloads are equally viable.
    return ::nsig::fixed::watch(std::forward<Expr>(e), [fn = std::move(fn)](bool v) mutable {
        if (v) fn();
    });
}

template <class Expr, class Fn>
[[nodiscard]] auto on_falling(Expr&& e, Fn fn) {
    // Qualified: an unqualified call would also find `nsig::watch` by ADL on
    // the expression type, and the two overloads are equally viable.
    return ::nsig::fixed::watch(std::forward<Expr>(e), [fn = std::move(fn)](bool v) mutable {
        if (!v) fn();
    });
}

}  // namespace nsig

namespace nsig::fixed {

using ::nsig::abs;
using ::nsig::clamp;
using ::nsig::debounce;
using ::nsig::ema;
using ::nsig::map_range;
using ::nsig::max;
using ::nsig::median3;
using ::nsig::min;
using ::nsig::peak;
using ::nsig::schmitt;
using ::nsig::slew;
using ::nsig::trough;

}  // namespace nsig::fixed

