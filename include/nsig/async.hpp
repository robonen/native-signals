// native-signals — coroutine layer: task<T>, reactive awaitables and
// resource<T> (an async derivation with loading/error state).
//
// Nothing here is required by the core; include it only if you want signals to
// interoperate with C++20 coroutines.
#pragma once

#include "signals.hpp"

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace nsig::dynamic {

// ---------------------------------------------------------------------------
// task<T> — a lazy coroutine with symmetric transfer
// ---------------------------------------------------------------------------

namespace detail {

struct task_promise_base {
    std::coroutine_handle<> continuation{};
    std::exception_ptr error{};

    struct final_awaiter {
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
            auto c = h.promise().continuation;
            return c ? c : std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };

    std::suspend_always initial_suspend() noexcept { return {}; }
    final_awaiter final_suspend() noexcept { return {}; }
    void unhandled_exception() noexcept {
#if NSIG_HAS_EXCEPTIONS
        error = std::current_exception();
#endif
    }
};

}  // namespace detail

template <class T = void>
class [[nodiscard]] task {
public:
    struct promise_type : detail::task_promise_base {
        std::optional<T> value{};
        task get_return_object() {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        template <class U = T>
        void return_value(U&& v) {
            value.emplace(std::forward<U>(v));
        }
    };
    using handle_type = std::coroutine_handle<promise_type>;

    task() noexcept = default;
    explicit task(handle_type h) noexcept : h_(h) {}
    task(task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    task& operator=(task&& o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    ~task() {
        if (h_) h_.destroy();
    }

    [[nodiscard]] bool done() const noexcept { return !h_ || h_.done(); }

    auto operator co_await() && noexcept {
        struct awaiter {
            handle_type h;
            [[nodiscard]] bool await_ready() const noexcept { return !h || h.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
                h.promise().continuation = c;
                return h;  // symmetric transfer: no extra stack frame
            }
            T await_resume() {
#if NSIG_HAS_EXCEPTIONS
                if (h.promise().error) std::rethrow_exception(h.promise().error);
#endif
                return std::move(*h.promise().value);
            }
        };
        return awaiter{h_};
    }

    [[nodiscard]] handle_type handle() const noexcept { return h_; }

private:
    handle_type h_{};
};

template <>
class [[nodiscard]] task<void> {
public:
    struct promise_type : detail::task_promise_base {
        task get_return_object() {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        void return_void() noexcept {}
    };
    using handle_type = std::coroutine_handle<promise_type>;

    task() noexcept = default;
    explicit task(handle_type h) noexcept : h_(h) {}
    task(task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    task& operator=(task&& o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    ~task() {
        if (h_) h_.destroy();
    }

    [[nodiscard]] bool done() const noexcept { return !h_ || h_.done(); }

    auto operator co_await() && noexcept {
        struct awaiter {
            handle_type h;
            [[nodiscard]] bool await_ready() const noexcept { return !h || h.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
                h.promise().continuation = c;
                return h;
            }
            void await_resume() {
#if NSIG_HAS_EXCEPTIONS
                if (h.promise().error) std::rethrow_exception(h.promise().error);
#endif
            }
        };
        return awaiter{h_};
    }

    [[nodiscard]] handle_type handle() const noexcept { return h_; }

private:
    handle_type h_{};
};

// ---------------------------------------------------------------------------
// Detached execution
// ---------------------------------------------------------------------------

namespace detail {

/// An eagerly started coroutine that frees itself when it finishes.
struct detached_task {
    struct promise_type {
        detached_task get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };
};

template <class T, class Fn>
detached_task run_and_report(task<T> t, Fn on_done) {
    std::exception_ptr err;
    if constexpr (std::is_void_v<T>) {
#if NSIG_HAS_EXCEPTIONS
        try {
            co_await std::move(t);
        } catch (...) {
            err = std::current_exception();
        }
#else
        co_await std::move(t);
        err = t.handle() ? t.handle().promise().error : err;
#endif
        on_done(err);
    } else {
        std::optional<T> value;
#if NSIG_HAS_EXCEPTIONS
        try {
            value.emplace(co_await std::move(t));
        } catch (...) {
            err = std::current_exception();
        }
#else
        value.emplace(co_await std::move(t));
#endif
        on_done(std::move(value), err);
    }
}

/// Defers work until the current effect flush has finished. Resuming a
/// coroutine from inside a flush would re-enter the scheduler; this keeps
/// resumption strictly outside it.
inline void post_after_flush(std::move_only_function<void()> fn) {
    ::nsig::detail::rt().post_flush.push_back(std::move(fn));
}

}  // namespace detail

/// Starts `t` and reports the outcome. For `task<T>` the callback receives
/// `(std::optional<T>, std::exception_ptr)`; for `task<void>` just the
/// exception pointer. Named `launch` rather than `spawn` because
/// `spawn` already creates child effects.
template <class T, class Fn>
void launch(task<T> t, Fn on_done) {
    detail::run_and_report<T, Fn>(std::move(t), std::move(on_done));
}

/// Fire-and-forget: starts `t` and drops the result. An escaping exception
/// terminates, so handle errors inside the coroutine or use the two-argument
/// overload.
template <class T>
void launch(task<T> t) {
    const auto rethrow = [](std::exception_ptr e) {
#if NSIG_HAS_EXCEPTIONS
        if (e) std::rethrow_exception(e);
#else
        if (e) std::terminate();
#endif
    };
    if constexpr (std::is_void_v<T>) {
        detail::run_and_report<T>(std::move(t), rethrow);
    } else {
        detail::run_and_report<T>(
            std::move(t), [rethrow](std::optional<T>, std::exception_ptr e) { rethrow(e); });
    }
}

// ---------------------------------------------------------------------------
// Reactive awaitables
// ---------------------------------------------------------------------------

namespace detail {

template <class Readable>
struct changed_awaiter {
    Readable src;
    std::optional<effect> watcher{};
    bool primed = false;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> c) {
        watcher.emplace([this, c] {
            src.get();  // subscribe
            if (!primed) {
                primed = true;
                return;
            }
            post_after_flush([this, c] {
                watcher.reset();
                c.resume();
            });
        });
    }

    void await_resume() const noexcept {}
};

template <class Pred>
struct until_awaiter {
    Pred pred;
    std::optional<effect> watcher{};

    [[nodiscard]] bool await_ready() {
        return untracked([this] { return static_cast<bool>(pred()); });
    }

    void await_suspend(std::coroutine_handle<> c) {
        watcher.emplace([this, c] {
            if (!static_cast<bool>(pred())) return;
            post_after_flush([this, c] {
                watcher.reset();
                c.resume();
            });
        });
    }

    void await_resume() const noexcept {}
};

}  // namespace detail

/// `co_await nsig::changed(count);` — resumes on the next change of `count`.
template <class Readable>
[[nodiscard]] auto changed(Readable src) {
    return detail::changed_awaiter<Readable>{std::move(src)};
}

/// `co_await nsig::until([&]{ return ready(); });` — resumes when the reactive
/// predicate becomes true (immediately if it already is).
template <std::invocable Pred>
[[nodiscard]] auto until(Pred pred) {
    return detail::until_awaiter<Pred>{std::move(pred)};
}

// ---------------------------------------------------------------------------
// resource<T> — an async derivation
// ---------------------------------------------------------------------------

/// Splits the reactive part from the async part, the way SolidJS'
/// `createResource` does: `source` is tracked and cheap, `fetcher` is async and
/// never tracked. That keeps dependency tracking exact — a lazily started
/// coroutine would otherwise register no dependencies at all.
///
///     nsig::signal<int> user_id{1};
///     nsig::resource user = nsig::make_resource(
///         [&] { return user_id(); },
///         [](int id) -> nsig::task<User> { co_return co_await fetch_user(id); });
///
///     user.loading();  // reactive
///     user.value();    // reactive, empty until the first load lands
template <class T>
class resource {
public:
    template <class Source, class Fetcher>
    resource(Source source, Fetcher fetcher)
        : tracker_(make_tracker(std::move(source), std::move(fetcher))) {}

    // The tracking effect captures `this`, so the object must not move.
    // `make_resource` still works: its return is a prvalue, elided by C++17.
    resource(const resource&) = delete;
    resource& operator=(const resource&) = delete;
    resource(resource&&) = delete;
    resource& operator=(resource&&) = delete;

    /// Reactive: the last successfully loaded value, empty until one arrives.
    const std::optional<T>& value() const { return value_.get(); }
    /// Reactive: true while a fetch is in flight.
    bool loading() const { return loading_.get(); }
    /// Reactive: the error from the most recent failed fetch, if any.
    std::exception_ptr error() const { return error_.get(); }
    /// Reactive: true once a value has ever landed.
    bool ready() const { return value_.get().has_value(); }

    /// Untracked snapshot, for use inside an effect that should not re-run.
    const std::optional<T>& peek_value() const { return value_.peek(); }

private:
    signal<std::optional<T>, never_equal> value_{std::optional<T>{}};
    signal<bool> loading_{false};
    signal<std::exception_ptr, never_equal> error_{nullptr};
    std::shared_ptr<std::uint64_t> generation_ = std::make_shared<std::uint64_t>(0);
    effect tracker_;

    template <class Source, class Fetcher>
    auto make_tracker(Source source, Fetcher fetcher) {
        return effect([this, source = std::move(source), fetcher = std::move(fetcher)]() mutable {
            auto input = source();  // the only tracked read
            const std::uint64_t gen = ++*generation_;
            auto value = value_;
            auto loading = loading_;
            auto error = error_;
            auto generation = generation_;

            loading.set(true);
            error.set(nullptr);

            launch(fetcher(std::move(input)),
                        [gen, generation, value, loading, error](std::optional<T> result,
                                                                 std::exception_ptr err) mutable {
                            if (gen != *generation) return;  // a newer fetch superseded this one
                            batch guard;
                            loading.set(false);
                            if (err) error.set(err);
                            else if (result) value.set(std::move(result));
                        });
        });
    }
};

/// Deduces `resource<T>` from the fetcher's `task<T>`.
template <class Source, class Fetcher>
auto make_resource(Source source, Fetcher fetcher) {
    using input_t = std::invoke_result_t<Source&>;
    using task_t = std::invoke_result_t<Fetcher&, input_t>;
    using value_t = decltype(std::declval<task_t&&>().operator co_await().await_resume());
    return resource<std::remove_cvref_t<value_t>>(std::move(source), std::move(fetcher));
}

// ---------------------------------------------------------------------------
// A minimal awaitable promise, so the async layer is usable (and testable)
// without dragging in an executor.
// ---------------------------------------------------------------------------

/// One-shot, settable from the outside: `co_await ev` suspends until `ev.set(v)`.
template <class T>
class async_event {
    struct state {
        std::optional<T> value{};
        std::exception_ptr error{};
        std::coroutine_handle<> waiter{};
        bool settled = false;
    };
    std::shared_ptr<state> s_ = std::make_shared<state>();

public:
    void set(T v) {
        if (s_->settled) return;
        s_->settled = true;
        s_->value.emplace(std::move(v));
        if (auto w = std::exchange(s_->waiter, {})) w.resume();
    }
    void fail(std::exception_ptr e) {
        if (s_->settled) return;
        s_->settled = true;
        s_->error = e;
        if (auto w = std::exchange(s_->waiter, {})) w.resume();
    }

    auto operator co_await() const noexcept {
        struct awaiter {
            std::shared_ptr<state> s;
            [[nodiscard]] bool await_ready() const noexcept { return s->settled; }
            void await_suspend(std::coroutine_handle<> c) noexcept { s->waiter = c; }
            T await_resume() {
#if NSIG_HAS_EXCEPTIONS
                if (s->error) std::rethrow_exception(s->error);
#endif
                return std::move(*s->value);
            }
        };
        return awaiter{s_};
    }
};

}  // namespace nsig::dynamic

namespace nsig {

using dynamic::async_event;
using dynamic::changed;
using dynamic::launch;
using dynamic::make_resource;
using dynamic::resource;
using dynamic::task;
using dynamic::until;

}  // namespace nsig
