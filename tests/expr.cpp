// Lazy expressions: operators, pick, lift, make_array — over both graphs.
#include "nsig/expr.hpp"

#include <algorithm>
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

void test_static_operators() {
    nsig::st::signal cpu{10};
    nsig::st::signal gpu{20};
    auto load = cpu + gpu;
    eq(load(), 30, "operator+");
    cpu = 15;
    eq(load(), 35, "re-read is lazy");

    auto hot = load > 40;
    check(!hot(), "comparison false");
    gpu = 40;
    check(hot(), "comparison true");

    auto nested = (cpu + gpu) * 2;
    eq(nested(), 110, "sub-expression held by value");

    auto negated = -cpu;
    eq(negated(), -15, "unary minus");
    std::cout << "  ok  static_operators\n";
}

void test_short_circuit_is_preserved() {
    nsig::st::signal flag{false};
    int right_reads = 0;
    auto counted = nsig::st::computed([&] {
        ++right_reads;
        return true;
    });
    auto both = flag && counted;

    check(!both(), "false && _ is false");
    eq(right_reads, 0, "right-hand node never read");
    flag = true;
    check(both(), "true && true");
    eq(right_reads, 1, "right-hand node read once");
    std::cout << "  ok  short_circuit_is_preserved\n";
}

void test_pick_evaluates_one_branch() {
    nsig::st::signal manual{false};
    int a_reads = 0, b_reads = 0;
    auto a = nsig::st::computed([&] {
        ++a_reads;
        return 1;
    });
    auto b = nsig::st::computed([&] {
        ++b_reads;
        return 2;
    });
    auto sel = nsig::pick(manual, a, b);

    eq(sel(), 2, "false branch");
    eq(a_reads, 0, "the other branch is not evaluated");
    manual = true;
    eq(sel(), 1, "true branch");
    std::cout << "  ok  pick_evaluates_one_branch\n";
}

void test_string_literals_in_pick() {
    nsig::st::signal fault{false};
    nsig::st::signal duty{50};
    auto colour = nsig::pick(fault, "red", nsig::pick(duty > 70, "amber", "green"));
    eq(std::string(colour()), std::string("green"), "green");
    duty = 90;
    eq(std::string(colour()), std::string("amber"), "amber");
    fault = true;
    eq(std::string(colour()), std::string("red"), "red");
    std::cout << "  ok  string_literals_in_pick\n";
}

void test_lift() {
    constexpr auto clamp_duty = nsig::lift([](int v) { return std::clamp(v, 0, 100); });
    nsig::st::signal raw{250};
    auto duty = clamp_duty(raw + 0);
    eq(duty(), 100, "clamped high");
    raw = -5;
    eq(duty(), 0, "clamped low");
    std::cout << "  ok  lift\n";
}

void test_expression_drives_an_output() {
    nsig::st::signal cpu{10};
    nsig::st::signal gpu{20};
    std::vector<int> writes;
    auto o = nsig::st::watch(cpu * 2, [&](int v) { writes.push_back(v); });

    eq(writes.size(), std::size_t{1}, "ran on construction");
    eq(writes[0], 20, "initial value");
    cpu = 16;
    eq(writes.size(), std::size_t{2}, "re-ran");
    eq(writes[1], 32, "new value");
    gpu = 41;
    eq(writes.size(), std::size_t{2}, "gpu is not part of that expression");
    (void)o;
    std::cout << "  ok  expression_drives_an_output\n";
}

void test_dynamic_graph_operators() {
    nsig::signal price{10};
    nsig::signal qty{3};
    nsig::computed total{price * qty};  // an expression is just an invocable
    eq(total(), 30, "computed from an expression");

    int runs = 0;
    auto e = nsig::effect([&] {
        total();
        ++runs;
    });
    qty = 4;
    eq(total(), 40, "recomputed");
    eq(runs, 2, "effect re-ran once");
    std::cout << "  ok  dynamic_graph_operators\n";
}

void test_expression_keeps_signal_alive() {
    // Handles are refcounted, so an expression copies them: the node survives
    // even after the original handle goes away.
    auto make = [] {
        nsig::signal<int> a{7};
        return a * 2;
    };
    auto doubled = make();
    eq(doubled(), 14, "expression owns the signal it read");
    std::cout << "  ok  expression_keeps_signal_alive\n";
}

void test_make_array() {
    nsig::st::signal base{100};
    std::vector<int> writes;
    auto outs = nsig::make_array<4>([&](std::size_t i) {
        return nsig::st::watch([&, i] { return base() + static_cast<int>(i); },
                                [&](int v) { writes.push_back(v); });
    });
    eq(writes.size(), std::size_t{4}, "four non-movable outputs constructed in place");
    base = 200;
    eq(writes.size(), std::size_t{8}, "all four re-ran");
    eq(writes.back(), 203, "last channel");
    (void)outs;
    std::cout << "  ok  make_array\n";
}

}  // namespace

int main() {
    test_static_operators();
    test_short_circuit_is_preserved();
    test_pick_evaluates_one_branch();
    test_string_literals_in_pick();
    test_lift();
    test_expression_drives_an_output();
    test_dynamic_graph_operators();
    test_expression_keeps_signal_alive();
    test_make_array();
    std::cout << "\n" << (checks - failures) << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
