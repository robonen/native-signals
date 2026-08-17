// Coroutine layer: task<T>, co_await changed/until, resource<T>.
#include "nsig/async.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

template <class A, class B>
void eq(const A& a, const B& b, const std::string& what) {
    ++checks;
    if (!(a == b)) {
        ++failures;
        std::cout << "  FAIL " << what << ": got " << a << ", want " << b << "\n";
    }
}

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "  FAIL " << what << "\n";
    }
}

nsig::task<int> add(int a, int b) { co_return a + b; }

nsig::task<int> nested(int n) {
    int acc = 0;
    for (int i = 0; i < n; ++i) acc = co_await add(acc, i);
    co_return acc;
}

void test_task_basics() {
    std::optional<int> got;
    nsig::launch(add(2, 3), [&](std::optional<int> v, std::exception_ptr) { got = v; });
    check(got.has_value() && *got == 5, "task returns a value");

    got.reset();
    nsig::launch(nested(5), [&](std::optional<int> v, std::exception_ptr) { got = v; });
    eq(got.value_or(-1), 10, "nested awaits");
    std::cout << "  ok  task_basics\n";
}

#if NSIG_HAS_EXCEPTIONS
nsig::task<int> boom() {
    throw std::runtime_error("nope");
    co_return 1;
}

void test_task_exception() {
    std::exception_ptr err;
    nsig::launch(boom(), [&](std::optional<int>, std::exception_ptr e) { err = e; });
    check(static_cast<bool>(err), "exception propagated to the callback");
    std::cout << "  ok  task_exception\n";
}
#endif

void test_co_await_changed() {
    nsig::signal<int> count{0};
    std::vector<int> seen;
    bool finished = false;

    auto body = [&]() -> nsig::task<void> {
        for (int i = 0; i < 3; ++i) {
            co_await nsig::changed(count);
            seen.push_back(count.peek());
        }
        finished = true;
    };
    nsig::launch(body());

    count = 1;
    count = 2;
    count = 3;

    eq(seen.size(), std::size_t{3}, "resumed once per change");
    eq(seen[0], 1, "first");
    eq(seen[2], 3, "third");
    check(finished, "coroutine ran to completion");
    std::cout << "  ok  co_await_changed\n";
}

void test_co_await_until() {
    nsig::signal<int> n{0};
    bool done = false;

    auto body = [&]() -> nsig::task<void> {
        co_await nsig::until([&] { return n() >= 3; });
        done = true;
    };
    nsig::launch(body());

    eq(done, false, "not yet");
    n = 1;
    eq(done, false, "still not");
    n = 3;
    eq(done, true, "predicate satisfied");
    std::cout << "  ok  co_await_until\n";
}

void test_until_already_true() {
    nsig::signal<int> n{10};
    bool done = false;
    auto body = [&]() -> nsig::task<void> {
        co_await nsig::until([&] { return n() >= 3; });
        done = true;
    };
    nsig::launch(body());
    eq(done, true, "resolves without suspending");
    std::cout << "  ok  until_already_true\n";
}

void test_resource() {
    nsig::signal<int> user_id{1};
    std::vector<nsig::async_event<std::string>> pending;
    pending.reserve(8);

    auto fetch = [&](int id) -> nsig::task<std::string> {
        pending.emplace_back();
        auto ev = pending.back();
        std::string name = co_await ev;
        co_return "user" + std::to_string(id) + ":" + name;
    };

    auto user = nsig::make_resource([&] { return user_id(); }, fetch);

    std::vector<std::string> log;
    auto watch = nsig::effect([&] {
        if (user.loading()) log.push_back("loading");
        else if (user.value()) log.push_back(*user.value());
    });

    check(user.loading(), "loading right after construction");
    pending[0].set("ann");
    check(!user.loading(), "settled");
    eq(user.value().value_or(""), std::string("user1:ann"), "value landed");

    user_id = 2;
    check(user.loading(), "refetch on source change");
    pending[1].set("bob");
    eq(user.value().value_or(""), std::string("user2:bob"), "second value");

    eq(log.size(), std::size_t{4}, "loading/value/loading/value");
    std::cout << "  ok  resource\n";
}

void test_resource_discards_stale_results() {
    nsig::signal<int> id{1};
    std::vector<nsig::async_event<int>> pending;
    pending.reserve(8);

    auto fetch = [&](int v) -> nsig::task<int> {
        pending.emplace_back();
        auto ev = pending.back();
        int delay_token = co_await ev;
        co_return v * 100 + delay_token;
    };

    auto r = nsig::make_resource([&] { return id(); }, fetch);
    id = 2;  // second fetch starts before the first resolves

    pending[1].set(0);  // newer one lands first
    eq(r.value().value_or(-1), 200, "newest value applied");

    pending[0].set(0);  // stale result arrives late
    eq(r.value().value_or(-1), 200, "stale result discarded");
    check(!r.loading(), "not stuck in loading");
    std::cout << "  ok  resource_discards_stale_results\n";
}

#if NSIG_HAS_EXCEPTIONS
void test_resource_error() {
    nsig::signal<int> id{1};
    auto fetch = [](int) -> nsig::task<int> {
        throw std::runtime_error("network");
        co_return 0;
    };
    auto r = nsig::make_resource([&] { return id(); }, fetch);
    check(static_cast<bool>(r.error()), "error surfaced reactively");
    check(!r.loading(), "loading cleared on failure");
    std::cout << "  ok  resource_error\n";
}
#endif

}  // namespace

int main() {
    test_task_basics();
#if NSIG_HAS_EXCEPTIONS
    test_task_exception();
#endif
    test_co_await_changed();
    test_co_await_until();
    test_until_already_true();
    test_resource();
    test_resource_discards_stale_results();
#if NSIG_HAS_EXCEPTIONS
    test_resource_error();
#endif
    std::cout << "\n" << (checks - failures) << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}