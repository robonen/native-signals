// The statically wired graph: cells, memoised expressions, change-detecting
// outputs, and the epoch short-circuit.
#include "nsig/static_graph.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0, checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "  FAIL " << what << "\n";
    }
}

template <class A, class B>
void eq(const A& a, const B& b, const std::string& what) {
    ++checks;
    if (!(a == b)) {
        ++failures;
        std::cout << "  FAIL " << what << ": got " << a << ", want " << b << "\n";
    }
}

void test_memo_evaluates_once_per_epoch() {
    nsig::st::signal temp{25.0f};
    nsig::st::signal offset{0};
    int evals = 0;
    auto scaled = nsig::st::computed([&] {
        ++evals;
        return static_cast<int>(temp()) + offset();
    });

    eq(evals, 0, "not evaluated before the first read");
    eq(scaled.get(), 25, "first value");
    eq(scaled.get(), 25, "cached");
    eq(scaled.get(), 25, "still cached");
    eq(evals, 1, "one evaluation per epoch, however many reads");

    offset = 5;
    eq(scaled.get(), 30, "picks up the write");
    eq(evals, 2, "one more evaluation");
    std::cout << "  ok  memo_evaluates_once_per_epoch\n";
}

void test_output_runs_on_write() {
    nsig::st::signal temp{25.0f};
    std::vector<int> writes;
    auto pwm = nsig::st::watch([&] { return static_cast<int>(temp()); },
                                [&](int v) { writes.push_back(v); });

    eq(writes.size(), std::size_t{1}, "runs once on construction");
    temp = 25.9f;  // int() is unchanged
    eq(writes.size(), std::size_t{1}, "a write that does not change the value stays inside");
    temp = 31.0f;
    eq(writes.size(), std::size_t{2}, "a real change reaches the sink — no poll() anywhere");
    eq(writes[1], 31, "second write");

    pwm.invalidate();
    pwm.poll();
    eq(writes.size(), std::size_t{3}, "invalidate + poll forces a re-emit");
    std::cout << "  ok  output_runs_on_write\n";
}

void test_batch_coalesces() {
    nsig::st::signal a{0};
    nsig::st::signal b{0};
    int runs = 0;
    auto o = nsig::st::watch([&] { return a() + b(); }, [&](int) { ++runs; });
    eq(runs, 1, "initial");
    {
        nsig::st::batch guard;
        a = 1;
        b = 1;
        eq(runs, 1, "deferred inside the batch");
    }
    eq(runs, 2, "one pass after the batch");
    (void)o;
    std::cout << "  ok  batch_coalesces\n";
}

void test_output_stops_at_end_of_scope() {
    nsig::st::signal a{0};
    int runs = 0;
    {
        auto o = nsig::st::watch([&] { return a(); }, [&](int) { ++runs; });
        a = 1;
        eq(runs, 2, "alive");
        (void)o;
    }
    a = 2;
    eq(runs, 2, "unregistered when the handle died");
    std::cout << "  ok  output_stops_at_end_of_scope\n";
}

void test_sink_writing_a_cell_settles() {
    nsig::st::signal input{0};
    nsig::st::signal mirror{-1};
    int runs = 0;
    auto o = nsig::st::watch([&] { return input(); }, [&](int v) {
        ++runs;
        mirror = v;  // a sink may write; the flush loops until it settles
    });
    input = 7;
    eq(mirror.get(), 7, "mirror followed");
    eq(runs, 2, "no runaway");
    (void)o;
    std::cout << "  ok  sink_writing_a_cell_settles\n";
}

void test_deadband_keeps_jitter_out() {
    struct deadband {
        bool operator()(float a, float b) const noexcept { return std::fabs(a - b) < 0.5f; }
    };
    nsig::st::signal<float, deadband> t{20.0f};
    int evals = 0;
    auto d = nsig::st::computed([&] {
        ++evals;
        return t();
    });
    eq(d.get(), 20.0f, "initial");
    t = 20.2f;
    eq(d.get(), 20.0f, "inside the deadband, the cell did not move");
    eq(evals, 1, "no re-evaluation at all");
    t = 21.0f;
    eq(d.get(), 21.0f, "outside the deadband");
    eq(evals, 2, "re-evaluated");
    std::cout << "  ok  deadband_keeps_jitter_out\n";
}

void test_incremental_expression() {
    nsig::st::signal sample{5.0f};
    auto peak = nsig::st::computed<int>([&](const int* previous) {
        const int x = static_cast<int>(sample());
        return previous != nullptr && *previous > x ? *previous : x;
    });

    eq(peak.get(), 5, "initial");
    sample = 12.0f;
    eq(peak.get(), 12, "grows");
    sample = 3.0f;
    eq(peak.get(), 12, "holds via the previous value");
    std::cout << "  ok  incremental_expression\n";
}

void test_dynamic_branches_need_no_relinking() {
    // The dynamic graph re-links its dependency set when a branch changes.
    // Here there is no dependency set at all, so a branch is just a branch.
    nsig::st::signal use_a{true};
    nsig::st::signal a{1};
    nsig::st::signal b{100};
    std::vector<int> writes;
    auto o = nsig::st::watch([&] { return use_a() ? a() : b(); },
                              [&](int v) { writes.push_back(v); });

    eq(writes.back(), 1, "reads a");
    b = 200;
    eq(writes.size(), std::size_t{1}, "b is not being read, so nothing is emitted");
    use_a = false;
    eq(writes.back(), 200, "switched to b");
    a = 5;
    eq(writes.size(), std::size_t{2}, "a is no longer read");
    (void)o;
    std::cout << "  ok  dynamic_branches_need_no_relinking\n";
}

void test_quiet_flush_short_circuits() {
    nsig::st::signal a{0};
    int hits = 0;
    auto o = nsig::st::watch([&] { return a() * 2; }, [&](int) { ++hits; });

    eq(hits, 1, "ran on construction");
    const auto before = nsig::st::epoch();
    nsig::st::flush();
    nsig::st::flush();
    eq(hits, 1, "flushing with nothing written does nothing");
    eq(nsig::st::epoch(), before, "and does not move the epoch");
    a = 1;
    eq(hits, 2, "change picked up");
    (void)o;
    std::cout << "  ok  quiet_flush_short_circuits\n";
}

void test_shared_memo_evaluated_once_for_many_outputs() {
    nsig::st::signal a{1};
    int evals = 0;
    auto shared = nsig::st::computed([&] {
        ++evals;
        return a() * 10;
    });
    int left = 0, right = 0;
    auto lo = nsig::st::watch([&] { return shared.get(); }, [&](int v) { left = v; });
    auto ro = nsig::st::watch([&] { return shared.get() + 1; }, [&](int v) { right = v; });

    eq(evals, 1, "one evaluation for both outputs");
    eq(left, 10, "left");
    eq(right, 11, "right");
    a = 2;
    eq(evals, 2, "one more evaluation for both");
    (void)lo;
    (void)ro;
    std::cout << "  ok  shared_memo_evaluated_once_for_many_outputs\n";
}

void test_redundant_write_costs_a_comparison() {
    nsig::st::signal<std::string> name{"ann"};
    const auto before = nsig::st::epoch();
    name = "ann";  // compared against a const char* — no std::string is built
    eq(nsig::st::epoch(), before, "a redundant write does not move the epoch");
    name = "bob";
    check(nsig::st::epoch() != before, "a real write does");
    eq(name.get(), std::string("bob"), "value applied");
    std::cout << "  ok  redundant_write_costs_a_comparison\n";
}

}  // namespace

int main() {
    test_memo_evaluates_once_per_epoch();
    test_output_runs_on_write();
    test_batch_coalesces();
    test_output_stops_at_end_of_scope();
    test_sink_writing_a_cell_settles();
    test_deadband_keeps_jitter_out();
    test_incremental_expression();
    test_dynamic_branches_need_no_relinking();
    test_quiet_flush_short_circuits();
    test_shared_memo_evaluated_once_for_many_outputs();
    test_redundant_write_costs_a_comparison();
    std::cout << "\n" << (checks - failures) << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
