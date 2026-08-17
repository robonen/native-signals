// native-signals — public API: signal / computed / effect / effect_scope.
#pragma once

#include "core.hpp"

#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace nsig {

// ---------------------------------------------------------------------------
// Equality policies
// ---------------------------------------------------------------------------

/// Uses `operator==` when the type has one, otherwise treats every write as a
/// change. Lets `signal<T>` work with non-comparable types without forcing the
/// user to spell out a policy.
struct auto_equal {
    /// Heterogeneous on purpose: `set()` compares before converting, so a
    /// redundant `signal<std::string> s = "same"` never builds a string.
    template <class A, class B>
    [[nodiscard]] constexpr bool operator()(const A& a, const B& b) const {
        if constexpr (requires { static_cast<bool>(a == b); })
            return static_cast<bool>(a == b);
        else return false;
    }
};

/// Every write notifies, even when the value compares equal.
struct never_equal {
    template <class A, class B>
    [[nodiscard]] constexpr bool operator()(const A&, const B&) const noexcept {
        return false;
    }
};

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

template <class F, class T>
concept getter_of = std::invocable<F&> && std::convertible_to<std::invoke_result_t<F&>, T>;

/// A getter that also receives the previous value (`nullptr` on the first run),
/// mirroring alien-signals' `getter(previousValue)` for incremental derivations.
template <class F, class T>
concept incremental_getter_of =
    std::invocable<F&, const T*> && std::convertible_to<std::invoke_result_t<F&, const T*>, T>;

namespace detail {

/// Calls the equality policy with the incoming value as-is when it accepts a
/// heterogeneous comparison, and otherwise converts first. Policies written the
/// obvious way — `bool operator()(float, float)` — keep working unchanged.
template <class T, class Eq, class U>
[[nodiscard]] constexpr bool equal_to(const Eq& eq, const T& current, const U& incoming) {
    if constexpr (std::invocable<const Eq&, const T&, const U&>) return eq(current, incoming);
    else return eq(current, static_cast<T>(incoming));
}

template <class F>
[[nodiscard]] consteval bool returns_cleanup() {
    using R = std::invoke_result_t<F&>;
    if constexpr (std::is_void_v<R>) return false;
    else return std::invocable<std::remove_cvref_t<R>&>;
}

inline void add_cleanup(effect_node* e, auto&& fn) {
    using F = std::remove_cvref_t<decltype(fn)>;
    if (!e->cleanup) {
        e->cleanup = std::move_only_function<void()>{
            [f = F(std::forward<decltype(fn)>(fn))]() mutable { f(); }};
    } else {
        e->cleanup = std::move_only_function<void()>{
            [prev = std::move(e->cleanup), f = F(std::forward<decltype(fn)>(fn))]() mutable {
                f();
                prev();
            }};
    }
}

/// RAII: makes `n` the active tracking target for the enclosing block.
struct tracking_scope {
    runtime& r;
    node* prev;
    explicit tracking_scope(node* n) noexcept : r(rt()), prev(std::exchange(r.active_sub, n)) {}
    ~tracking_scope() { r.active_sub = prev; }
    tracking_scope(const tracking_scope&) = delete;
    tracking_scope& operator=(const tracking_scope&) = delete;
};

// -- signal node ------------------------------------------------------------

template <class T, class Eq>
struct signal_node final : node {
    T current;
    std::optional<T> staged{};  // populated only between a write and its commit
    [[no_unique_address]] Eq eq{};

    template <class... A>
    explicit signal_node(A&&... a)
        : node(&vtable, node_kind::signal, f_mutable), current(std::forward<A>(a)...) {}

    [[nodiscard]] const T& latest() const noexcept { return staged ? *staged : current; }

    static bool update_(node* n) {
        auto* s = static_cast<signal_node*>(n);
        s->flags = f_mutable;
        if (!s->staged) return false;
        const bool changed = !s->eq(s->current, *s->staged);
        s->current = std::move(*s->staged);
        s->staged.reset();
        return changed;
    }

    static const T& read_tracked(node* n) {
        auto* s = static_cast<signal_node*>(n);
        if (s->flags & f_dirty) {
            if (update_(s)) {
                if (link* subs = s->subs; subs != nullptr) shallow_propagate(subs);
            }
        }
        auto& r = rt();
        if (node* sub = r.active_sub; sub != nullptr) link_dep(s, sub, r.cycle);
        return s->current;
    }

    static const T& read_untracked(node* n) {
        auto* s = static_cast<signal_node*>(n);
        if (s->flags & f_dirty) update_(s);
        return s->current;
    }

    static void destroy_(node* n) noexcept { delete static_cast<signal_node*>(n); }

    static constexpr node_vtable vtable{&update_, nullptr, &destroy_};
};

// -- computed node ----------------------------------------------------------

template <class T, class Eq, class G>
struct computed_node final : node {
    std::optional<T> value{};
    [[no_unique_address]] G getter;
    [[no_unique_address]] Eq eq{};

    explicit computed_node(G g)
        : node(&vtable, node_kind::computed, f_none), getter(std::move(g)) {}

    [[nodiscard]] T invoke_getter() {
        if constexpr (incremental_getter_of<G, T>) {
            return static_cast<T>(getter(value ? &*value : nullptr));
        } else {
            return static_cast<T>(getter());
        }
    }

    /// Tracked re-evaluation. Returns true when the memoised value changed.
    static bool update_(node* n) {
        auto* c = static_cast<computed_node*>(n);
        if (c->flags & f_has_child) dispose_child_effects(c);
        c->deps_tail = nullptr;
        c->flags = static_cast<flag_t>(f_mutable | f_recursed_check);

        auto& r = rt();
        node* prev = std::exchange(r.active_sub, c);
        ++r.cycle;
        struct restore {
            runtime& r;
            node* prev;
            computed_node* c;
            ~restore() {
                r.active_sub = prev;
                c->flags = static_cast<flag_t>(c->flags & ~f_recursed_check);
                purge_deps(c);
            }
        } guard{r, prev, c};

        if (!c->value) {
            c->value.emplace(c->invoke_getter());
            return true;
        }
        T next = c->invoke_getter();
        if (c->eq(*c->value, next)) return false;
        *c->value = std::move(next);
        return true;
    }

    static const T& read_tracked(node* n) {
        auto* c = static_cast<computed_node*>(n);
        const flag_t flags = c->flags;

        if ((flags & f_dirty) ||
            ((flags & f_pending) &&
             (check_dirty(c->deps, c) ||
              (c->flags = static_cast<flag_t>(flags & ~f_pending), false)))) {
            if (update_(c)) {
                if (link* subs = c->subs; subs != nullptr) shallow_propagate(subs);
            }
        } else if (flags == f_none) {
            // First evaluation of a computed that nothing is watching yet.
            c->flags = static_cast<flag_t>(f_mutable | f_recursed_check);
            tracking_scope guard{c};
            struct clear {
                computed_node* c;
                ~clear() { c->flags = static_cast<flag_t>(c->flags & ~f_recursed_check); }
            } cl{c};
            c->value.emplace(c->invoke_getter());
        }

        auto& r = rt();
        if (node* sub = r.active_sub; sub != nullptr) link_dep(c, sub, r.cycle);
        return *c->value;
    }

    static const T& read_untracked(node* n) {
        tracking_scope guard{nullptr};
        return read_tracked(n);
    }

    static void destroy_(node* n) noexcept { delete static_cast<computed_node*>(n); }

    static constexpr node_vtable vtable{&update_, nullptr, &destroy_};
};

// -- effect node ------------------------------------------------------------

template <class F>
struct effect_impl final : effect_node {
    [[no_unique_address]] F fn;

    explicit effect_impl(F f)
        : effect_node(&vtable, node_kind::effect,
                      static_cast<flag_t>(f_watching | f_recursed_check)),
          fn(std::move(f)) {}

    static void run_(node* n) {
        auto* self = static_cast<effect_impl*>(n);
        if constexpr (returns_cleanup<F>()) add_cleanup(self, self->fn());
        else static_cast<void>(self->fn());
    }
    static bool update_(node*) { return true; }
    static void destroy_(node* n) noexcept { delete static_cast<effect_impl*>(n); }

    static constexpr node_vtable vtable{&update_, &run_, &destroy_};
};

// -- scope node -------------------------------------------------------------

struct scope_node final : node {
    scope_node() : node(&vtable, node_kind::scope, f_mutable) {}

    static bool update_(node* n) {
        n->flags = f_mutable;
        return true;
    }
    static void destroy_(node* n) noexcept { delete static_cast<scope_node*>(n); }
    static constexpr node_vtable vtable{&update_, nullptr, &destroy_};
};

template <class G>
struct getter_result {
    using type = std::remove_cvref_t<std::invoke_result_t<G&>>;
};

}  // namespace detail

/// The general-purpose graph: dependencies discovered at run time, arbitrary
/// shape, effects, scopes and coroutines. Everything here is also re-exported
/// into `nsig` at the bottom of this file, so `nsig::signal` keeps working;
/// spell out `nsig::dynamic::signal` in a file that uses both graphs.
namespace dynamic {

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

/// Runs every pending effect now.
inline void flush() { detail::flush_now(); }

/// Replaces synchronous flushing with a callback — integrate with a frame
/// loop, an event loop or a task queue. Pass nullptr to restore synchronous
/// behaviour. The callback must eventually call `nsig::flush()`.
inline void set_scheduler(detail::scheduler_fn fn) noexcept { detail::rt().scheduler = fn; }

/// Defers effect flushing until the outermost batch ends.
class [[nodiscard]] batch {
    bool active_ = true;

public:
    batch() noexcept { ++detail::rt().batch_depth; }
    batch(const batch&) = delete;
    batch& operator=(const batch&) = delete;
    ~batch() {
        if (active_ && --detail::rt().batch_depth == 0) detail::request_flush();
    }
    /// Ends the batch early.
    void commit() {
        if (!std::exchange(active_, false)) return;
        if (--detail::rt().batch_depth == 0) detail::request_flush();
    }
};

template <std::invocable F>
decltype(auto) batched(F&& fn) {
    batch b;
    return std::forward<F>(fn)();
}

/// Reads inside `fn` register no dependencies.
template <std::invocable F>
decltype(auto) untracked(F&& fn) {
    detail::tracking_scope guard{nullptr};
    return std::forward<F>(fn)();
}

/// Registers a cleanup on the effect currently running. Cleanups run in
/// reverse registration order, before the next run and on disposal.
template <std::invocable F>
void on_cleanup(F&& fn) {
    auto* sub = detail::rt().active_sub;
    if (sub == nullptr || sub->kind != detail::node_kind::effect) return;
    detail::add_cleanup(static_cast<detail::effect_node*>(sub), std::forward<F>(fn));
}

// ---------------------------------------------------------------------------
// signal<T>
// ---------------------------------------------------------------------------

/// A mutable reactive value. The handle is a cheap, copyable reference to a
/// refcounted node — copying a `signal` shares state, it does not clone it.
template <class T, class Eq = auto_equal>
class signal {
    static_assert(!std::is_reference_v<T>, "signal<T&> is unsupported; use a pointer");
    using node_t = detail::signal_node<T, Eq>;

    node_t* n_;

public:
    using value_type = T;
    using equal_type = Eq;

    signal()
        requires std::default_initializable<T>
        : n_(new node_t()) {}

    explicit signal(T value) : n_(new node_t(std::move(value))) {}

    /// In-place construction: `signal<std::string> s{std::in_place, 8, 'x'};`
    template <class... A>
        requires std::constructible_from<T, A...>
    explicit signal(std::in_place_t, A&&... a) : n_(new node_t(std::forward<A>(a)...)) {}

    signal(const signal& o) noexcept : n_(o.n_) { detail::retain(n_); }
    signal(signal&& o) noexcept : n_(std::exchange(o.n_, nullptr)) {}
    ~signal() {
        if (n_ != nullptr) detail::release(n_);
    }

    /// Rebinds the handle to another signal's node (reference semantics).
    /// To copy a value between signals, write `a.set(b.get())`.
    signal& operator=(const signal& o) noexcept {
        if (this != &o) {
            detail::retain(o.n_);
            if (n_ != nullptr) detail::release(n_);
            n_ = o.n_;
        }
        return *this;
    }
    signal& operator=(signal&& o) noexcept {
        if (this != &o) {
            if (n_ != nullptr) detail::release(n_);
            n_ = std::exchange(o.n_, nullptr);
        }
        return *this;
    }

    /// Value assignment: `count = 5;`
    template <class U>
        requires(!std::same_as<std::remove_cvref_t<U>, signal> && std::convertible_to<U &&, T>)
    signal& operator=(U&& v) {
        set(static_cast<T>(std::forward<U>(v)));
        return *this;
    }

    // -- reads --------------------------------------------------------------

    /// Tracked read: registers a dependency on the active effect/computed.
    const T& get() const { return node_t::read_tracked(n_); }
    const T& operator()() const { return get(); }
    const T& operator*() const { return get(); }
    const T* operator->() const { return &get(); }

    /// Untracked read.
    [[nodiscard]] const T& peek() const { return node_t::read_untracked(n_); }

    // -- writes -------------------------------------------------------------

    /// Writes a new value.
    ///
    /// Accepts anything convertible to `T` and only converts when the value
    /// actually differs, so a redundant write of a heavyweight type costs one
    /// comparison and nothing else. The propagating half lives in a separate
    /// out-of-line function, which keeps the unchanged-write path — by far the
    /// most common one in a control loop — small enough to inline everywhere.
    template <class U = T>
        requires std::constructible_from<T, U &&>
    void set(U&& value) {
        auto* n = n_;
        if (detail::equal_to<T>(n->eq, n->latest(), value)) return;
        commit(n, T(std::forward<U>(value)));
    }

    void operator()(T value) { set(std::move(value)); }

    /// Mutates a copy in place, then writes it back:
    /// `items.modify([](auto& v){ v.push_back(1); });`
    template <class F>
        requires std::invocable<F&, T&>
    void modify(F&& fn) {
        T next = peek();
        std::forward<F>(fn)(next);
        set(std::move(next));
    }

    /// Grants mutable access to the stored value and notifies unconditionally.
    /// Skips the copy `modify` makes, at the cost of change detection.
    template <class F>
        requires std::invocable<F&, T&>
    void mutate(F&& fn) {
        node_t::read_untracked(n_);
        std::forward<F>(fn)(n_->current);
        touch();
    }

    /// Notifies subscribers unconditionally — for values changed behind the
    /// library's back.
    void touch() {
        auto* n = n_;
        n->flags = static_cast<detail::flag_t>(detail::f_mutable | detail::f_dirty);
        if (detail::link* subs = n->subs; subs != nullptr) {
            detail::propagate(subs, detail::rt().run_depth != 0);
            detail::shallow_propagate(subs);
            detail::request_flush();
        }
    }

    // -- identity / diagnostics ---------------------------------------------

    [[nodiscard]] bool same_node_as(const signal& o) const noexcept { return n_ == o.n_; }
    [[nodiscard]] detail::node* raw() const noexcept { return n_; }
    [[nodiscard]] std::size_t subscriber_count() const noexcept {
        std::size_t k = 0;
        for (const detail::link* l = n_->subs; l != nullptr; l = l->next_sub) ++k;
        return k;
    }

private:
    NSIG_NOINLINE static void commit(node_t* n, T value) {
        n->staged.emplace(std::move(value));
        n->flags = static_cast<detail::flag_t>(detail::f_mutable | detail::f_dirty);
        if (detail::link* subs = n->subs; subs != nullptr) {
            detail::propagate(subs, detail::rt().run_depth != 0);
            detail::request_flush();
        }
    }
};

template <class T>
signal(T) -> signal<T>;

// ---------------------------------------------------------------------------
// computed<T>
// ---------------------------------------------------------------------------

/// A lazily evaluated, memoised derivation. Recomputes only when a dependency
/// really changed, and only when someone reads it.
template <class T, class Eq = auto_equal>
class computed {
    detail::node* n_;
    const T& (*read_)(detail::node*);
    const T& (*peek_)(detail::node*);

public:
    using value_type = T;
    using equal_type = Eq;

    template <class G>
        requires(getter_of<G, T> || incremental_getter_of<G, T>)
    explicit computed(G getter) {
        using node_t = detail::computed_node<T, Eq, std::decay_t<G>>;
        n_ = new node_t(std::move(getter));
        read_ = &node_t::read_tracked;
        peek_ = &node_t::read_untracked;
    }

    computed(const computed& o) noexcept : n_(o.n_), read_(o.read_), peek_(o.peek_) {
        detail::retain(n_);
    }
    computed(computed&& o) noexcept
        : n_(std::exchange(o.n_, nullptr)), read_(o.read_), peek_(o.peek_) {}
    ~computed() {
        if (n_ != nullptr) detail::release(n_);
    }

    computed& operator=(const computed& o) noexcept {
        if (this != &o) {
            detail::retain(o.n_);
            if (n_ != nullptr) detail::release(n_);
            n_ = o.n_;
            read_ = o.read_;
            peek_ = o.peek_;
        }
        return *this;
    }
    computed& operator=(computed&& o) noexcept {
        if (this != &o) {
            if (n_ != nullptr) detail::release(n_);
            n_ = std::exchange(o.n_, nullptr);
            read_ = o.read_;
            peek_ = o.peek_;
        }
        return *this;
    }

    const T& get() const { return read_(n_); }
    const T& operator()() const { return get(); }
    const T& operator*() const { return get(); }
    const T* operator->() const { return &get(); }
    [[nodiscard]] const T& peek() const { return peek_(n_); }

    [[nodiscard]] detail::node* raw() const noexcept { return n_; }
    /// Erased accessors, used by `readonly<T>`.
    [[nodiscard]] auto read_fn() const noexcept { return read_; }
    [[nodiscard]] auto peek_fn() const noexcept { return peek_; }
};

template <class G>
    requires std::invocable<G&>
computed(G) -> computed<typename detail::getter_result<G>::type>;

// ---------------------------------------------------------------------------
// readonly<T> — type-erased read-only view over a signal or a computed
// ---------------------------------------------------------------------------

/// Hand this out from a class to expose reactive state without exposing the
/// setter. Holds a strong reference to the underlying node.
template <class T>
class readonly {
    detail::node* n_;
    const T& (*read_)(detail::node*);
    const T& (*peek_)(detail::node*);

public:
    using value_type = T;

    template <class Eq>
    readonly(const signal<T, Eq>& s) noexcept
        : n_(s.raw()),
          read_(&detail::signal_node<T, Eq>::read_tracked),
          peek_(&detail::signal_node<T, Eq>::read_untracked) {
        detail::retain(n_);
    }

    template <class Eq>
    readonly(const computed<T, Eq>& c) noexcept
        : n_(c.raw()), read_(c.read_fn()), peek_(c.peek_fn()) {
        detail::retain(n_);
    }

    readonly(const readonly& o) noexcept : n_(o.n_), read_(o.read_), peek_(o.peek_) {
        detail::retain(n_);
    }
    readonly(readonly&& o) noexcept
        : n_(std::exchange(o.n_, nullptr)), read_(o.read_), peek_(o.peek_) {}
    ~readonly() {
        if (n_ != nullptr) detail::release(n_);
    }
    readonly& operator=(readonly o) noexcept {
        std::swap(n_, o.n_);
        std::swap(read_, o.read_);
        std::swap(peek_, o.peek_);
        return *this;
    }

    const T& get() const { return read_(n_); }
    const T& operator()() const { return get(); }
    const T& operator*() const { return get(); }
    const T* operator->() const { return &get(); }
    [[nodiscard]] const T& peek() const { return peek_(n_); }
};

// ---------------------------------------------------------------------------
// effect
// ---------------------------------------------------------------------------

/// A side effect that re-runs when its tracked dependencies change.
///
/// Ownership follows lexical structure: an effect created at top level is
/// owned by the returned handle (RAII — destroying it stops the effect); an
/// effect created inside another effect or an `effect_scope` is owned by that
/// parent, exactly like alien-signals' nested effects, and the handle is then
/// a non-owning observer.
class [[nodiscard]] effect {
    detail::effect_node* n_ = nullptr;
    bool owning_ = false;

public:
    template <class F>
        requires std::invocable<F&>
    explicit effect(F fn) {
        using impl = detail::effect_impl<F>;
        auto* e = new impl(std::move(fn));
        n_ = e;

        auto& r = detail::rt();
        detail::node* parent = std::exchange(r.active_sub, e);
        owning_ = (parent == nullptr);
        if (parent != nullptr) {
            detail::link_dep(e, parent, 0);
            parent->flags = static_cast<detail::flag_t>(parent->flags | detail::f_has_child);
        }
        ++r.run_depth;
        struct restore {
            detail::runtime& r;
            detail::node* parent;
            detail::effect_node* e;
            ~restore() {
                --r.run_depth;
                r.active_sub = parent;
                e->flags = static_cast<detail::flag_t>(e->flags & ~detail::f_recursed_check);
            }
        } guard{r, parent, e};
        impl::run_(e);
    }

    effect(const effect&) = delete;
    effect& operator=(const effect&) = delete;

    effect(effect&& o) noexcept
        : n_(std::exchange(o.n_, nullptr)), owning_(std::exchange(o.owning_, false)) {}

    effect& operator=(effect&& o) noexcept {
        if (this != &o) {
            reset();
            n_ = std::exchange(o.n_, nullptr);
            owning_ = std::exchange(o.owning_, false);
        }
        return *this;
    }

    ~effect() { reset(); }

    /// Stops the effect and runs its cleanup, regardless of ownership.
    void stop() {
        if (n_ == nullptr) return;
        detail::dispose_effect(n_);
        detail::release(n_);
        n_ = nullptr;
        owning_ = false;
    }

    /// Gives up ownership: the effect now lives as long as its parent scope.
    void detach() noexcept { owning_ = false; }

    [[nodiscard]] bool active() const noexcept {
        return n_ != nullptr && n_->flags != detail::f_none;
    }
    [[nodiscard]] bool owning() const noexcept { return owning_; }

private:
    void reset() noexcept {
        if (n_ == nullptr) return;
        if (owning_) {
            NSIG_TRY {
                detail::dispose_effect(n_);
            }
            NSIG_CATCH_ALL {}
        }
        detail::release(n_);
        n_ = nullptr;
    }
};

/// Runs `sink(value)` when the value of `source` changes — and not merely when
/// something it reads was written.
///
/// `effect` is the "re-run this whenever a dependency changed" primitive;
/// `watch` is the "tell me when this value is different" one. Reach for `watch`
/// at a boundary where repeating an unchanged write costs something: a
/// register, a packet, a log line.
///
///     auto w = nsig::watch([&] { return status_colour(); },
///                          [](Colour c) { set_led(c); });
///
/// Returns an `effect`, so it stops when the handle goes out of scope. The sink
/// runs untracked: reading a signal inside it does not create a dependency.
template <class Source, class Sink, class Eq = auto_equal>
    requires std::invocable<Source&>
[[nodiscard]] effect watch(Source source, Sink sink) {
    using T = std::remove_cvref_t<std::invoke_result_t<Source&>>;
    return effect([source = std::move(source), sink = std::move(sink), eq = Eq{},
                   last = std::optional<T>{}]() mutable {
        T next = source();
        if (last && eq(*last, next)) return;
        last = std::move(next);
        untracked([&] { sink(*last); });
    });
}

/// Creates a child effect owned by the enclosing effect or `effect_scope`.
/// This is the nested-effect form; at top level use `nsig::effect` so the
/// handle can own the effect.
template <class F>
    requires std::invocable<F&>
void spawn(F&& fn) {
    NSIG_ASSERT(detail::rt().active_sub != nullptr &&
                "nsig::spawn requires an enclosing effect or effect_scope");
    effect child{std::forward<F>(fn)};
    child.detach();
}

// ---------------------------------------------------------------------------
// effect_scope
// ---------------------------------------------------------------------------

/// Groups effects so they can be disposed together.
class [[nodiscard]] effect_scope {
    detail::scope_node* n_;

public:
    effect_scope() : n_(new detail::scope_node()) {
        auto& r = detail::rt();
        if (detail::node* parent = r.active_sub; parent != nullptr) {
            detail::link_dep(n_, parent, 0);
            parent->flags = static_cast<detail::flag_t>(parent->flags | detail::f_has_child);
        }
    }

    /// Creates the scope and immediately runs `fn` inside it.
    template <class F>
        requires std::invocable<F&>
    explicit effect_scope(F&& fn) : effect_scope() {
        run(std::forward<F>(fn));
    }

    effect_scope(const effect_scope&) = delete;
    effect_scope& operator=(const effect_scope&) = delete;
    effect_scope(effect_scope&& o) noexcept : n_(std::exchange(o.n_, nullptr)) {}
    effect_scope& operator=(effect_scope&& o) noexcept {
        if (this != &o) {
            reset();
            n_ = std::exchange(o.n_, nullptr);
        }
        return *this;
    }
    ~effect_scope() { reset(); }

    /// Runs `fn` with this scope active, so effects created inside belong to it.
    template <class F>
        requires std::invocable<F&>
    decltype(auto) run(F&& fn) {
        detail::tracking_scope guard{n_};
        return std::forward<F>(fn)();
    }

    void stop() noexcept {
        if (n_ != nullptr) detail::dispose_scope(n_);
    }

    [[nodiscard]] bool active() const noexcept {
        return n_ != nullptr && n_->flags != detail::f_none;
    }

private:
    void reset() noexcept {
        if (n_ == nullptr) return;
        detail::dispose_scope(n_);
        detail::release(n_);
        n_ = nullptr;
    }
};

// ---------------------------------------------------------------------------
// trigger — manual invalidation of whatever is read inside `fn`
// ---------------------------------------------------------------------------

namespace trigger_impl {
inline bool update(detail::node*) { return true; }
inline void destroy(detail::node*) noexcept {}
inline constexpr detail::node_vtable vtable{&update, nullptr, &destroy};
}  // namespace trigger_impl

/// Marks every signal/computed read inside `fn` as changed. Use it after
/// mutating a value in place without going through a setter.
template <std::invocable F>
void trigger(F&& fn) {
    detail::node probe{&trigger_impl::vtable, detail::node_kind::effect,
                       static_cast<detail::flag_t>(detail::f_watching | detail::f_recursed_check)};
    auto& r = detail::rt();
    detail::node* prev = std::exchange(r.active_sub, &probe);
    ++r.batch_depth;

    struct finish {
        detail::runtime& r;
        detail::node* prev;
        detail::node* probe;
        ~finish() {
            r.active_sub = prev;
            probe->flags = detail::f_none;
            detail::link* l = probe->deps;
            while (l != nullptr) {
                detail::node* dep = l->dep;
                detail::retain(dep);
                l = detail::unlink_dep(l, probe);
                if (detail::link* subs = dep->subs; subs != nullptr) {
                    detail::propagate(subs, r.run_depth != 0);
                    detail::shallow_propagate(subs);
                }
                detail::release(dep);
            }
            if (--r.batch_depth == 0) detail::request_flush();
        }
    } guard{r, prev, &probe};

    std::forward<F>(fn)();
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

struct stats {
    std::uint64_t links_created;
    std::uint64_t node_updates;
    std::uint64_t effect_runs;
    std::size_t link_pool_reserved;
};

/// Pre-sizes the pools and the effect queue; see `detail::reserve`.
inline void reserve(std::size_t nodes_bytes, std::size_t links, std::size_t queued_effects) {
    detail::reserve(nodes_bytes, links, queued_effects);
}

[[nodiscard]] inline stats get_stats() noexcept {
    auto& r = detail::rt();
    return {r.stat_links_created, r.stat_node_updates, r.stat_effect_runs,
            r.pool.reserved_links()};
}

inline void reset_stats() noexcept {
    auto& r = detail::rt();
    r.stat_links_created = 0;
    r.stat_node_updates = 0;
    r.stat_effect_runs = 0;
}

}  // namespace dynamic

// ---------------------------------------------------------------------------
// The run-time graph is the default, so `nsig::signal` means `nsig::dynamic::
// signal`. `nsig::fixed` is the opt-in sibling; neither is nested inside the
// other.
// ---------------------------------------------------------------------------

using dynamic::batch;
using dynamic::batched;
using dynamic::computed;
using dynamic::effect;
using dynamic::effect_scope;
using dynamic::flush;
using dynamic::get_stats;
using dynamic::on_cleanup;
using dynamic::readonly;
using dynamic::reset_stats;
using dynamic::reserve;
using dynamic::set_scheduler;
using dynamic::signal;
using dynamic::spawn;
using dynamic::stats;
using dynamic::trigger;
using dynamic::untracked;
using dynamic::watch;

}  // namespace nsig
