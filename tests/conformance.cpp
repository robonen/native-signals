// Semantics conformance: ports of stackblitz/alien-signals' own test suite
// (effect.spec.ts, effectScope.spec.ts, trigger.spec.ts) plus the classic
// reactivity cases: diamonds, dynamic dependencies, laziness, batching.
#include "nsig/signals.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
const char* g_current = "";

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cout << "  FAIL [" << g_current << "] " << what << "\n";
    }
}

template <class A, class B>
void eq(const A& a, const B& b, const std::string& what) {
    ++g_checks;
    if (!(a == b)) {
        ++g_failures;
        std::cout << "  FAIL [" << g_current << "] " << what << ": got " << a << ", want " << b
                  << "\n";
    }
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += v[i];
    }
    return s;
}

struct test_case {
    const char* name;
    void (*fn)();
};

std::vector<test_case>& registry() {
    static std::vector<test_case> r;
    return r;
}

struct reg {
    reg(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

#define TEST(name)                                     \
    void test_##name();                                \
    const reg reg_##name{#name, &test_##name};         \
    void test_##name()

// ---------------------------------------------------------------------------
// Basics
// ---------------------------------------------------------------------------

TEST(basic_signal_computed_effect) {
    nsig::signal count{1};
    nsig::computed doubled{[&] { return count() * 2; }};
    std::vector<std::string> log;

    auto e = nsig::effect([&] { log.push_back("count=" + std::to_string(count())); });
    eq(doubled(), 2, "initial computed");
    count = 2;
    eq(doubled(), 4, "computed after write");
    eq(join(log), "count=1,count=2", "effect log");
}

TEST(computed_is_lazy_and_memoised) {
    nsig::signal a{1};
    int evals = 0;
    nsig::computed c{[&] {
        ++evals;
        return a() * 10;
    }};
    eq(evals, 0, "not evaluated before first read");
    eq(c(), 10, "first read");
    eq(evals, 1, "evaluated once");
    eq(c(), 10, "cached read");
    eq(evals, 1, "still once");
    a = 2;
    eq(evals, 1, "write alone does not recompute an unwatched computed");
    eq(c(), 20, "recomputed on read");
    eq(evals, 2, "evaluated twice total");
}

TEST(equal_value_write_does_not_notify) {
    nsig::signal a{1};
    int runs = 0;
    auto e = nsig::effect([&] {
        a();
        ++runs;
    });
    eq(runs, 1, "initial run");
    a = 1;
    eq(runs, 1, "no run for identical value");
    a = 2;
    eq(runs, 2, "run for changed value");
}

TEST(never_equal_policy_always_notifies) {
    nsig::signal<int, nsig::never_equal> a{1};
    int runs = 0;
    auto e = nsig::effect([&] {
        a();
        ++runs;
    });
    a = 1;
    a = 1;
    eq(runs, 3, "every write notifies");
}

TEST(memoised_computed_stops_propagation) {
    nsig::signal a{2};
    nsig::computed parity{[&] { return a() % 2; }};
    int runs = 0;
    auto e = nsig::effect([&] {
        parity();
        ++runs;
    });
    eq(runs, 1, "initial");
    a = 4;  // parity unchanged
    eq(runs, 1, "effect does not re-run when the computed value is unchanged");
    a = 5;
    eq(runs, 2, "effect re-runs when the computed value changes");
}

TEST(diamond_runs_effect_once) {
    nsig::signal a{1};
    nsig::computed b{[&] { return a() + 1; }};
    nsig::computed c{[&] { return a() * 2; }};
    int runs = 0;
    int last = 0;
    auto e = nsig::effect([&] {
        last = b() + c();
        ++runs;
    });
    eq(runs, 1, "initial");
    eq(last, 4, "initial value");
    a = 2;
    eq(runs, 2, "one run per write, not two");
    eq(last, 7, "value after write");
}

TEST(deep_chain_check_dirty) {
    nsig::signal src{0};
    nsig::computed c1{[&] { return src(); }};
    nsig::computed c2{[&] { return c1(); }};
    nsig::computed c3{[&] { return c2(); }};
    nsig::computed c4{[&] { return c3(); }};
    // c5 flattens the value away: downstream must not re-run.
    nsig::computed c5{[&] { return c4() >= 0 ? 1 : 0; }};
    int runs = 0;
    auto e = nsig::effect([&] {
        c5();
        ++runs;
    });
    eq(runs, 1, "initial");
    src = 1;
    src = 2;
    src = 3;
    eq(runs, 1, "check_dirty resolves the chain without re-running the effect");
}

TEST(dynamic_dependencies) {
    nsig::signal use_a{true};
    nsig::signal a{1};
    nsig::signal b{100};
    int runs = 0;
    int last = 0;
    auto e = nsig::effect([&] {
        last = use_a() ? a() : b();
        ++runs;
    });
    eq(last, 1, "reads a");
    b = 200;
    eq(runs, 1, "b is not a dependency yet");
    use_a = false;
    eq(last, 200, "switched to b");
    a = 5;
    eq(runs, 2, "a is no longer a dependency");
    b = 300;
    eq(runs, 3, "b is now a dependency");
    eq(last, 300, "value from b");
}

TEST(batching_coalesces_runs) {
    nsig::signal a{0};
    nsig::signal b{0};
    int runs = 0;
    auto e = nsig::effect([&] {
        a();
        b();
        ++runs;
    });
    eq(runs, 1, "initial");
    {
        nsig::batch guard;
        a = 1;
        b = 1;
        eq(runs, 1, "deferred inside the batch");
    }
    eq(runs, 2, "single run after the batch");
}

TEST(untracked_reads_do_not_subscribe) {
    nsig::signal a{0};
    nsig::signal b{0};
    int runs = 0;
    auto e = nsig::effect([&] {
        a();
        nsig::untracked([&] { return b(); });
        ++runs;
    });
    b = 1;
    eq(runs, 1, "untracked read is not a dependency");
    a = 1;
    eq(runs, 2, "tracked read still is");
}

TEST(peek_does_not_subscribe) {
    nsig::signal a{0};
    nsig::signal b{0};
    int runs = 0;
    auto e = nsig::effect([&] {
        a();
        (void)b.peek();
        ++runs;
    });
    b = 1;
    eq(runs, 1, "peek is not a dependency");
}

// ---------------------------------------------------------------------------
// Cleanup ordering — ports of alien-signals/tests/effect.spec.ts
// ---------------------------------------------------------------------------

TEST(cleanup_order_on_outer_rerun) {
    std::vector<std::string> log;
    nsig::signal a{0};

    auto outer = nsig::effect([&] {
        a();
        log.push_back("outer:run");
        nsig::spawn([&] {
            log.push_back("inner:run");
            return [&] { log.push_back("inner:cleanup"); };
        });
        return [&] { log.push_back("outer:cleanup"); };
    });
    eq(join(log), "outer:run,inner:run", "initial");

    log.clear();
    a = 1;
    eq(join(log), "inner:cleanup,outer:cleanup,outer:run,inner:run", "after write");
}

TEST(cleanup_order_on_dispose_inner_before_outer) {
    std::vector<std::string> log;
    auto outer = nsig::effect([&] {
        log.push_back("outer:run");
        nsig::spawn([&] {
            log.push_back("inner:run");
            return [&] { log.push_back("inner:cleanup"); };
        });
        return [&] { log.push_back("outer:cleanup"); };
    });
    log.clear();
    outer.stop();
    eq(join(log), "inner:cleanup,outer:cleanup", "dispose order");
}

TEST(sibling_cleanup_lifo_on_dispose) {
    std::vector<std::string> log;
    auto outer = nsig::effect([&] {
        nsig::spawn([&] { return [&] { log.push_back("inner1:cleanup"); }; });
        nsig::spawn([&] { return [&] { log.push_back("inner2:cleanup"); }; });
        nsig::spawn([&] { return [&] { log.push_back("inner3:cleanup"); }; });
        return [&] { log.push_back("outer:cleanup"); };
    });
    outer.stop();
    eq(join(log), "inner3:cleanup,inner2:cleanup,inner1:cleanup,outer:cleanup", "LIFO");
}

TEST(sibling_cleanup_lifo_on_outer_rerun) {
    std::vector<std::string> log;
    nsig::signal a{0};
    auto outer = nsig::effect([&] {
        a();
        nsig::spawn([&] { return [&] { log.push_back("inner1:cleanup"); }; });
        nsig::spawn([&] { return [&] { log.push_back("inner2:cleanup"); }; });
        nsig::spawn([&] { return [&] { log.push_back("inner3:cleanup"); }; });
        return [&] { log.push_back("outer:cleanup"); };
    });
    log.clear();
    a = 1;
    std::vector<std::string> first4(log.begin(), log.begin() + 4);
    eq(join(first4), "inner3:cleanup,inner2:cleanup,inner1:cleanup,outer:cleanup", "LIFO on rerun");
}

TEST(three_level_nested_cleanup_depth_first_reverse) {
    std::vector<std::string> log;
    auto outer = nsig::effect([&] {
        nsig::spawn([&] {
            nsig::spawn([&] { return [&] { log.push_back("grandchild:cleanup"); }; });
            return [&] { log.push_back("child:cleanup"); };
        });
        return [&] { log.push_back("outer:cleanup"); };
    });
    outer.stop();
    eq(join(log), "grandchild:cleanup,child:cleanup,outer:cleanup", "depth-first reverse");
}

TEST(computed_unwatched_cleans_child_effects_lifo) {
    std::vector<std::string> log;
    nsig::computed c{[&] {
        nsig::spawn([&] { return [&] { log.push_back("e1"); }; });
        nsig::spawn([&] { return [&] { log.push_back("e2"); }; });
        nsig::spawn([&] { return [&] { log.push_back("e3"); }; });
        return 0;
    }};
    auto e = nsig::effect([&] { c(); });
    log.clear();
    e.stop();
    eq(join(log), "e3,e2,e1", "unwatched computed cleans up LIFO");
}

TEST(effect_in_computed_old_cleanup_before_new_setup) {
    nsig::signal a{0};
    std::vector<std::string> log;

    nsig::computed c{[&] {
        log.push_back("computed:eval");
        nsig::spawn([&] {
            log.push_back("inner:run");
            return [&] { log.push_back("inner:cleanup"); };
        });
        return a();
    }};

    auto e = nsig::effect([&] { c(); });
    log.clear();
    a = 1;
    eq(join(log), "inner:cleanup,computed:eval,inner:run", "order across computed re-eval");
}

TEST(cleanup_order_after_prior_inner_only_rerun) {
    nsig::signal a{0};
    nsig::signal b{0};
    std::vector<std::string> log;

    auto outer = nsig::effect([&] {
        a();
        log.push_back("outer:run");
        nsig::spawn([&] {
            b();
            log.push_back("inner:run");
            return [&] { log.push_back("inner:cleanup"); };
        });
        return [&] { log.push_back("outer:cleanup"); };
    });

    b = 1;  // inner re-runs alone
    log.clear();
    a = 1;
    eq(join(log), "inner:cleanup,outer:cleanup,outer:run,inner:run", "has_child survives");
}

TEST(outer_keeps_responding_after_inner_reruns) {
    // https://github.com/stackblitz/alien-signals/issues/115
    nsig::signal a{0};
    nsig::signal b{0};
    int outer_runs = 0;
    int inner_runs = 0;

    auto outer = nsig::effect([&] {
        a();
        ++outer_runs;
        nsig::spawn([&] {
            b();
            ++inner_runs;
        });
    });
    eq(outer_runs, 1, "outer initial");
    eq(inner_runs, 1, "inner initial");

    b = 1;
    eq(outer_runs, 1, "outer untouched");
    check(inner_runs >= 2, "inner re-ran");

    a = 1;
    eq(outer_runs, 2, "outer still responds to its own dep");
}

// ---------------------------------------------------------------------------
// effect_scope — ports of alien-signals/tests/effectScope.spec.ts
// ---------------------------------------------------------------------------

TEST(scope_dispose_runs_child_cleanup) {
    std::vector<std::string> log;
    nsig::effect_scope scope{[&] {
        nsig::spawn([&] { return [&] { log.push_back("inner:cleanup"); }; });
    }};
    scope.stop();
    eq(join(log), "inner:cleanup", "scope dispose");
}

TEST(scope_dispose_siblings_lifo) {
    std::vector<std::string> log;
    nsig::effect_scope scope{[&] {
        nsig::spawn([&] { return [&] { log.push_back("e1:cleanup"); }; });
        nsig::spawn([&] { return [&] { log.push_back("e2:cleanup"); }; });
        nsig::spawn([&] { return [&] { log.push_back("e3:cleanup"); }; });
    }};
    scope.stop();
    eq(join(log), "e3:cleanup,e2:cleanup,e1:cleanup", "scope LIFO");
}

TEST(scope_nested_cleanup_depth_first_reverse) {
    std::vector<std::string> log;
    nsig::effect_scope scope{[&] {
        nsig::spawn([&] {
            nsig::spawn([&] { return [&] { log.push_back("grandchild:cleanup"); }; });
            return [&] { log.push_back("child:cleanup"); };
        });
    }};
    scope.stop();
    eq(join(log), "grandchild:cleanup,child:cleanup", "nested scope dispose");
}

TEST(scope_as_intermediate_parent) {
    nsig::signal a{0};
    std::vector<std::string> log;

    auto outer = nsig::effect([&] {
        a();
        log.push_back("outer:run");
        nsig::effect_scope inner_scope{[&] {
            nsig::spawn([&] {
                log.push_back("inner:run");
                return [&] { log.push_back("inner:cleanup"); };
            });
        }};
        inner_scope.stop();  // handle is local; hand the graph its ownership
        return [&] { log.push_back("outer:cleanup"); };
    });
    log.clear();
    a = 1;
    check(!log.empty(), "outer re-ran");
}

TEST(scope_stops_effects_from_reacting) {
    nsig::signal count{1};
    int runs = 0;
    nsig::effect_scope scope{[&] {
        nsig::spawn([&] {
            count();
            ++runs;
        });
    }};
    eq(runs, 1, "initial");
    count = 2;
    eq(runs, 2, "reacts while alive");
    scope.stop();
    count = 3;
    eq(runs, 2, "silent after stop");
}

// ---------------------------------------------------------------------------
// trigger — ports of alien-signals/tests/trigger.spec.ts
// ---------------------------------------------------------------------------

TEST(trigger_with_no_dependencies) {
    nsig::trigger([] {});
    check(true, "no throw");
}

TEST(trigger_updates_dependent_computed) {
    nsig::signal<std::vector<int>> arr{};
    nsig::computed length{[&] { return static_cast<int>(arr().size()); }};
    eq(length(), 0, "initial");
    arr.mutate([](auto& v) { v.push_back(1); });
    eq(length(), 1, "after in-place mutate + touch");
}

TEST(trigger_second_source_signal) {
    nsig::signal<std::vector<int>> src1{};
    nsig::signal<std::vector<int>> src2{};
    nsig::computed length{[&] { return static_cast<int>(src2().size()); }};
    eq(length(), 0, "initial");
    const_cast<std::vector<int>&>(src2.peek()).push_back(1);
    nsig::trigger([&] {
        src1();
        src2();
    });
    eq(length(), 1, "trigger propagated");
}

TEST(trigger_reruns_effect_once) {
    nsig::signal<std::vector<int>> src1{};
    nsig::signal<std::vector<int>> src2{};
    int triggers = 0;
    auto e = nsig::effect([&] {
        ++triggers;
        src1();
        src2();
    });
    eq(triggers, 1, "initial");
    nsig::trigger([&] {
        src1();
        src2();
    });
    eq(triggers, 2, "exactly one extra run");
}

TEST(trigger_does_not_notify_its_own_probe) {
    nsig::signal<std::vector<int>> src1{};
    nsig::computed src2{[&] { return static_cast<int>(src1().size()); }};
    auto e = nsig::effect([&] {
        src1();
        src2();
    });
    nsig::trigger([&] {
        src1();
        src2();
    });
    check(true, "no crash / no infinite loop");
}

TEST(trigger_allows_writing_after_reading) {
    nsig::signal src1{1};
    nsig::trigger([&] {
        src1();
        src1.set(src1() + 1);
    });
    eq(src1(), 2, "write inside trigger applied");
}

TEST(trigger_reruns_effect_once_when_writing_after_reading) {
    nsig::signal src1{1};
    int triggers = 0;
    auto e = nsig::effect([&] {
        ++triggers;
        src1();
    });
    eq(triggers, 1, "initial");
    nsig::trigger([&] {
        src1();
        src1.set(src1() + 1);
    });
    eq(triggers, 2, "one extra run");
    eq(src1(), 2, "value");
}

// ---------------------------------------------------------------------------
// C++-specific behaviour
// ---------------------------------------------------------------------------

TEST(raii_effect_stops_on_scope_exit) {
    nsig::signal a{0};
    int runs = 0;
    {
        auto e = nsig::effect([&] {
            a();
            ++runs;
        });
        a = 1;
        eq(runs, 2, "alive");
    }
    a = 2;
    eq(runs, 2, "stopped after the handle died");
}

TEST(on_cleanup_registers_imperatively) {
    nsig::signal a{0};
    std::vector<std::string> log;
    auto e = nsig::effect([&] {
        a();
        nsig::on_cleanup([&] { log.push_back("first"); });
        nsig::on_cleanup([&] { log.push_back("second"); });
    });
    a = 1;
    eq(join(log), "second,first", "reverse registration order");
}

TEST(incremental_getter_receives_previous_value) {
    nsig::signal a{1};
    nsig::computed<int> running_max{[&](const int* prev) {
        const int v = a();
        return prev ? (v > *prev ? v : *prev) : v;
    }};
    eq(running_max(), 1, "initial");
    a = 5;
    eq(running_max(), 5, "grew");
    a = 3;
    eq(running_max(), 5, "kept the max via the previous value");
}

TEST(string_signal_and_modify) {
    nsig::signal<std::string> s{std::in_place, 3, 'a'};
    eq(s(), std::string("aaa"), "in-place construction");
    int runs = 0;
    auto e = nsig::effect([&] {
        s();
        ++runs;
    });
    s.modify([](std::string& v) { v += "b"; });
    eq(s(), std::string("aaab"), "modify");
    eq(runs, 2, "notified once");
    s.modify([](std::string&) {});
    eq(runs, 2, "no notification for an unchanged value");
}

TEST(readonly_view) {
    nsig::signal a{1};
    nsig::computed c{[&] { return a() * 3; }};
    nsig::readonly<int> ra = a;
    nsig::readonly<int> rc = c;
    int runs = 0;
    auto e = nsig::effect([&] {
        ra();
        ++runs;
    });
    eq(rc(), 3, "readonly over computed");
    a = 2;
    eq(rc(), 6, "tracks updates");
    eq(runs, 2, "readonly reads still subscribe");
}

TEST(copied_handle_shares_state) {
    nsig::signal a{1};
    nsig::signal b = a;  // shares the node
    check(a.same_node_as(b), "same node");
    b = 7;
    eq(a(), 7, "write through the copy is visible");
}

TEST(nested_batches) {
    nsig::signal a{0};
    int runs = 0;
    auto e = nsig::effect([&] {
        a();
        ++runs;
    });
    {
        nsig::batch outer;
        a = 1;
        {
            nsig::batch inner;
            a = 2;
        }
        eq(runs, 1, "inner batch does not flush");
    }
    eq(runs, 2, "outermost batch flushes once");
}

TEST(custom_scheduler_defers_flush) {
    static int scheduled = 0;
    scheduled = 0;
    nsig::set_scheduler([] { ++scheduled; });

    nsig::signal a{0};
    int runs = 0;
    auto e = nsig::effect([&] {
        a();
        ++runs;
    });
    a = 1;
    eq(runs, 1, "effect deferred to the scheduler");
    eq(scheduled, 1, "scheduler invoked");
    nsig::flush();
    eq(runs, 2, "ran on explicit flush");

    nsig::set_scheduler(nullptr);
}

#if NSIG_HAS_EXCEPTIONS
TEST(exception_in_effect_propagates_and_keeps_graph_usable) {
    nsig::signal a{0};
    int runs = 0;
    bool caught = false;
    auto e = nsig::effect([&] {
        ++runs;
        if (a() == 1) throw std::runtime_error("boom");
    });
    try {
        a = 1;
    } catch (const std::runtime_error&) {
        caught = true;
    }
    check(caught, "exception surfaced to the writer");
    a = 2;
    eq(runs, 3, "graph still live after the throw");
}
#endif

TEST(watch_fires_only_when_the_value_changes) {
    nsig::signal a{2};
    std::vector<std::string> log;
    auto w = nsig::watch([&] { return a() % 2 == 0 ? "even" : "odd"; },
                         [&](const char* s) { log.push_back(s); });
    eq(join(log), "even", "initial");
    a = 4;
    eq(join(log), "even", "dependency changed, value did not");
    a = 5;
    eq(join(log), "even,odd", "value changed");
    a = 7;
    eq(join(log), "even,odd", "still odd");
}

TEST(subscriber_count_drops_to_zero) {
    nsig::signal a{0};
    {
        auto e = nsig::effect([&] { a(); });
        eq(a.subscriber_count(), std::size_t{1}, "subscribed");
    }
    eq(a.subscriber_count(), std::size_t{0}, "unsubscribed after dispose");
}

}  // namespace

int main() {
    for (const auto& tc : registry()) {
        g_current = tc.name;
        const int before = g_failures;
        tc.fn();
        if (g_failures == before) std::cout << "  ok  " << tc.name << "\n";
    }
    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed across "
              << registry().size() << " tests\n";
    if (g_failures != 0) std::cout << g_failures << " FAILURES\n";
    return g_failures == 0 ? 0 : 1;
}
