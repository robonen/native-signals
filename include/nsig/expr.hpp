// native-signals — lazy expressions over reactive nodes.
//
//     auto load = cpu + gpu;                 // instead of [&]{ return cpu() + gpu(); }
//     auto hot  = temp > 60.0f;
//     auto duty = pick(is_manual, manual_duty, curve);
//
// Works for both graphs: `nsig::signal` / `nsig::computed` and `nsig::fixed::signal`
// / `nsig::fixed::computed`. An expression is a lazily evaluated, *non*-caching node —
// it holds its operands and computes on read. Wrap it in `nsig::computed` or
// `nsig::fixed::computed` when the value should be cached, or hand it to an effect or
// an output when it should drive something.
//
// It is all closures over references, so `a + b` compiles to exactly what
// `[&]{ return a() + b(); }` compiles to. Verified: adding this to the fan
// controller benchmark changes neither its instruction count nor its timings.
//
// Where this stops helping: the moment a derivation needs a branch, a switch,
// or a call to something unlifted, a lambda is clearer than any operator chain.
// That is a boundary worth respecting rather than a gap to fill with more
// overloads — a reactive DSL that tries to swallow control flow ends up worse
// than the language it is hiding. There is deliberately no `operator=` sugar
// for wiring, no pipe syntax, and no lazy `if`.
//
// Lifetime: `nsig::signal` and `nsig::computed` are refcounted handles, so an
// expression copies them and keeps their nodes alive. `nsig::st` nodes are the
// values themselves, so an expression points at them — the same rule as a
// lambda capturing by reference. Sub-expressions are temporaries and are held
// by value, so `(a + b) * c` is safe to store.
#pragma once

#include "signals.hpp"
#include "static_graph.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace nsig {

/// Anything with a `.get()`: a signal, a computed, a cell, a memo, or another
/// expression.
template <class N>
concept readable = requires(const std::remove_cvref_t<N>& n) { n.get(); };

/// Refcounted handles are copied into the expression, so it owns what it reads.
/// Everything else is referred to.
template <class N>
struct expression_holds_by_value : std::false_type {};

template <class T, class Eq>
struct expression_holds_by_value<dynamic::signal<T, Eq>> : std::true_type {};
template <class T, class Eq>
struct expression_holds_by_value<dynamic::computed<T, Eq>> : std::true_type {};
template <class T>
struct expression_holds_by_value<dynamic::readonly<T>> : std::true_type {};

namespace detail {

template <class X>
inline constexpr bool by_pointer_v =
    std::is_lvalue_reference_v<X> && readable<X> &&
    !expression_holds_by_value<std::remove_cvref_t<X>>::value;

/// `decay_t` for the by-value case, so a string literal is held as a pointer
/// rather than as an unreturnable array.
template <class X>
using captured_t =
    std::conditional_t<by_pointer_v<X>, const std::remove_cvref_t<X>*, std::decay_t<X>>;

template <class X>
[[nodiscard]] constexpr captured_t<X> capture(X&& x) {
    if constexpr (by_pointer_v<X>) return &x;
    else return std::forward<X>(x);
}

template <class C>
[[nodiscard]] constexpr decltype(auto) read(const C& c) {
    // A pointer only means "node held by reference" when it points at a node;
    // a decayed string literal is just a value.
    if constexpr (std::is_pointer_v<C> && readable<std::remove_pointer_t<C>>) return c->get();
    else if constexpr (readable<C>) return c.get();
    else return (c);
}

}  // namespace detail

/// A lazily evaluated node. Holds no cache: reading it computes.
template <class F>
class expr {
    [[no_unique_address]] F fn_;

public:
    using value_type = std::remove_cvref_t<std::invoke_result_t<const F&>>;

    constexpr explicit expr(F fn) : fn_(std::move(fn)) {}

    [[nodiscard]] constexpr value_type get() const { return fn_(); }
    [[nodiscard]] constexpr value_type operator()() const { return fn_(); }
};

template <class F>
expr(F) -> expr<F>;

#define NSIG_BINARY_OP(op)                                            \
    template <class A, class B>                                       \
        requires(readable<A> || readable<B>)                          \
    [[nodiscard]] constexpr auto operator op(A&& a, B&& b) {          \
        return expr([ca = detail::capture<A>(std::forward<A>(a)),     \
                     cb = detail::capture<B>(std::forward<B>(b))] {   \
            return detail::read(ca) op detail::read(cb);              \
        });                                                           \
    }

NSIG_BINARY_OP(+)
NSIG_BINARY_OP(-)
NSIG_BINARY_OP(*)
NSIG_BINARY_OP(/)
NSIG_BINARY_OP(%)
NSIG_BINARY_OP(==)
NSIG_BINARY_OP(!=)
NSIG_BINARY_OP(<)
NSIG_BINARY_OP(<=)
NSIG_BINARY_OP(>)
NSIG_BINARY_OP(>=)
NSIG_BINARY_OP(&)
NSIG_BINARY_OP(|)
NSIG_BINARY_OP(^)
// `&&` and `||` keep short-circuiting. The built-in operator still sits between
// the two reads inside the closure, so the right-hand node is only read when the
// left-hand result requires it — which is not true of most overloaded `&&`.
NSIG_BINARY_OP(&&)
NSIG_BINARY_OP(||)

#undef NSIG_BINARY_OP

#define NSIG_UNARY_OP(op)                                                            \
    template <class A>                                                               \
        requires readable<A>                                                         \
    [[nodiscard]] constexpr auto operator op(A&& a) {                                \
        return expr(                                                                 \
            [ca = detail::capture<A>(std::forward<A>(a))] { return op detail::read(ca); }); \
    }

NSIG_UNARY_OP(-)
NSIG_UNARY_OP(!)
NSIG_UNARY_OP(~)

#undef NSIG_UNARY_OP

/// `?:` cannot be overloaded, so this is the selector. Only the chosen branch
/// is read.
///
///     auto duty = pick(is_manual, manual_duty, curve);
template <class C, class A, class B>
    requires(readable<C> || readable<A> || readable<B>)
[[nodiscard]] constexpr auto pick(C&& cond, A&& a, B&& b) {
    return expr([cc = detail::capture<C>(std::forward<C>(cond)),
                 ca = detail::capture<A>(std::forward<A>(a)),
                 cb = detail::capture<B>(std::forward<B>(b))] {
        return detail::read(cc) ? detail::read(ca) : detail::read(cb);
    });
}

/// Turns an ordinary function into one that accepts nodes.
///
///     constexpr auto clamp_duty = nsig::lift([](int v) { return std::clamp(v, 0, 100); });
///     auto duty = clamp_duty(target + trim);
template <class F>
[[nodiscard]] constexpr auto lift(F fn) {
    return [fn = std::move(fn)]<class... Xs>(Xs&&... xs) {
        return expr([fn, ... cs = detail::capture<Xs>(std::forward<Xs>(xs))] {
            return fn(detail::read(cs)...);
        });
    };
}

/// Builds `N` nodes from an index, for per-channel state.
///
///     auto duty = nsig::make_array<5>([&](std::size_t i) {
///         return nsig::computed<int>{[i] { return target() + trim[i](); }};
///     });
///
/// Elements are constructed in place, so this also works for node types that
/// cannot be moved, such as `nsig::fixed::watch`.
///
/// The index is a run-time `std::size_t` on purpose: a generic lambda taking
/// `auto i` would be a separate instantiation per index, so the closures it
/// creates would have different types and could not share an array.
template <std::size_t N, class F>
    requires std::invocable<F&, std::size_t>
[[nodiscard]] constexpr auto make_array(F&& make) {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<std::invoke_result_t<F&, std::size_t>, N>{make(std::size_t{I})...};
    }(std::make_index_sequence<N>{});
}

}  // namespace nsig

namespace nsig::fixed {

// Re-exported so argument-dependent lookup finds them for `nsig::st` nodes too:
// ADL for `nsig::fixed::signal` searches `nsig::st`, not its enclosing namespace.
using ::nsig::expr;
using ::nsig::lift;
using ::nsig::make_array;
using ::nsig::pick;
using ::nsig::readable;

using ::nsig::operator+;
using ::nsig::operator-;
using ::nsig::operator*;
using ::nsig::operator/;
using ::nsig::operator%;
using ::nsig::operator==;
using ::nsig::operator!=;
using ::nsig::operator<;
using ::nsig::operator<=;
using ::nsig::operator>;
using ::nsig::operator>=;
using ::nsig::operator&;
using ::nsig::operator|;
using ::nsig::operator^;
using ::nsig::operator&&;
using ::nsig::operator||;
using ::nsig::operator!;
using ::nsig::operator~;

}  // namespace nsig::fixed

namespace nsig::dynamic {

// Re-exported so argument-dependent lookup finds them for `nsig::dynamic` nodes:
// ADL for `nsig::dynamic::signal` searches `nsig::dynamic`, not `nsig`.
using ::nsig::expr;
using ::nsig::lift;
using ::nsig::make_array;
using ::nsig::pick;
using ::nsig::readable;

using ::nsig::operator+;
using ::nsig::operator-;
using ::nsig::operator*;
using ::nsig::operator/;
using ::nsig::operator%;
using ::nsig::operator==;
using ::nsig::operator!=;
using ::nsig::operator<;
using ::nsig::operator<=;
using ::nsig::operator>;
using ::nsig::operator>=;
using ::nsig::operator&;
using ::nsig::operator|;
using ::nsig::operator^;
using ::nsig::operator&&;
using ::nsig::operator||;
using ::nsig::operator!;
using ::nsig::operator~;

}  // namespace nsig::dynamic
