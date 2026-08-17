// Adapters: lifted pure functions, idempotent derivations, sampling filters.
#include "nsig/adapt.hpp"

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

void test_lifted_math() {
    nsig::st::signal target{50};
    nsig::st::signal trim{0};
    auto duty = nsig::clamp(target + trim, 0, 100);
    eq(duty(), 50, "in range");
    trim = 80;
    eq(duty(), 100, "clamped high");
    target = -30;
    trim = 0;
    eq(duty(), 0, "clamped low");

    nsig::st::signal a{3};
    nsig::st::signal b{7};
    eq(nsig::min(a, b)(), 3, "min");
    eq(nsig::max(a, b)(), 7, "max");
    eq(nsig::abs(a - b)(), 4, "abs");

    nsig::st::signal adc{2048};
    auto volts = nsig::map_range(adc, 0, 4095, 0.0f, 3.3f);
    check(volts() > 1.64f && volts() < 1.66f, "map_range");
    std::cout << "  ok  lifted_math\n";
}

void test_schmitt_is_idempotent_in_the_graph() {
    nsig::st::signal temp{20.0f};
    nsig::st::signal unrelated{0};
    auto fans_on = nsig::st::computed<bool>(nsig::schmitt(temp, 28.0f, 30.0f));

    check(!fans_on(), "cold");
    temp = 29.0f;
    check(!fans_on(), "inside the band, stays off");
    temp = 31.0f;
    check(fans_on(), "above the upper threshold");
    temp = 29.0f;
    check(fans_on(), "inside the band, stays on");

    // The point of idempotence: an unrelated write moves the epoch and forces a
    // re-evaluation, which must not change the answer.
    for (int i = 0; i < 10; ++i) {
        unrelated = i;
        check(fans_on(), "unaffected by unrelated writes");
    }
    temp = 27.0f;
    check(!fans_on(), "below the lower threshold");
    std::cout << "  ok  schmitt_is_idempotent_in_the_graph\n";
}

void test_peak_and_trough() {
    nsig::st::signal v{5};
    nsig::st::signal unrelated{0};
    auto hi = nsig::st::computed<int>(nsig::peak(v));
    auto lo = nsig::st::computed<int>(nsig::trough(v));

    eq(hi(), 5, "peak initial");
    v = 12;
    eq(hi(), 12, "peak grows");
    v = 3;
    eq(hi(), 12, "peak holds");
    eq(lo(), 3, "trough drops");
    unrelated = 1;
    eq(hi(), 12, "peak survives an unrelated write");
    eq(lo(), 3, "trough survives an unrelated write");
    std::cout << "  ok  peak_and_trough\n";
}

void test_tracked_sees_every_cycle_memo_does_not() {
    nsig::st::signal<float> temp{10.0f};

    // Both wrap the same running maximum. The lazy one only sees the values it
    // is read on; the tracked one sees every cycle.
    auto lazy = nsig::st::computed<float>(nsig::peak(temp));
    auto watched = nsig::st::fold<float>(nsig::peak(temp));

    temp = 90.0f;  // nobody reads `lazy` here
    temp = 20.0f;

    eq(watched(), 90.0f, "tracked caught the spike");
    eq(lazy(), 20.0f, "the lazy memo missed it — this is why `tracked` exists");
    std::cout << "  ok  tracked_sees_every_cycle_memo_does_not\n";
}

void test_sensor_reads_a_plain_function() {
    int hardware = 100;
    int reads = 0;
    auto adc = nsig::fixed::sensor([&] {
        ++reads;
        return hardware;
    });

    eq(reads, 1, "one read to seed the cell");
    eq(adc(), 100, "initial value");

    std::vector<int> writes;
    auto o = nsig::st::watch([&] { return adc(); }, [&](int v) { writes.push_back(v); });
    eq(writes.size(), std::size_t{1}, "output ran");

    hardware = 200;
    eq(adc(), 100, "the graph does not see hardware until it is sampled");
    adc.sample();
    eq(adc(), 200, "sampled");
    eq(writes.size(), std::size_t{2}, "output followed");
    (void)o;
    std::cout << "  ok  sensor_reads_a_plain_function\n";
}

void test_ema_advances_once_per_sample() {
    float hardware = 0.0f;
    nsig::st::signal<int> unrelated{0};
    auto temp = nsig::fixed::sensor([&] { return hardware; }, nsig::ema{0.5f});

    eq(temp(), 0.0f, "seeded with the raw reading");
    hardware = 100.0f;
    temp.sample();
    eq(temp(), 50.0f, "halfway after one sample");

    // The filter must not drift when the graph re-evaluates for other reasons.
    for (int i = 0; i < 20; ++i) unrelated = i;
    eq(temp(), 50.0f, "unrelated writes do not advance the filter");

    temp.sample();
    eq(temp(), 75.0f, "and the next sample does");
    std::cout << "  ok  ema_advances_once_per_sample\n";
}

void test_slew_ramps() {
    int commanded = 0;
    auto duty = nsig::fixed::sensor([&] { return commanded; }, nsig::slew{20});
    commanded = 100;
    duty.sample();
    eq(duty(), 20, "first step");
    duty.sample();
    eq(duty(), 40, "second step");
    for (int i = 0; i < 5; ++i) duty.sample();
    eq(duty(), 100, "arrives and stops");
    std::cout << "  ok  slew_ramps\n";
}

void test_debounce() {
    bool pin = false;
    auto button = nsig::fixed::sensor([&] { return pin; }, nsig::debounce<bool>{3});
    eq(button(), false, "released");

    pin = true;
    button.sample();
    eq(button(), false, "one bounce is not a press");
    button.sample();
    eq(button(), false, "two");
    button.sample();
    eq(button(), true, "three in a row is a press");

    pin = false;
    button.sample();
    button.sample();
    eq(button(), true, "and the release needs three too");
    button.sample();
    eq(button(), false, "released");
    std::cout << "  ok  debounce\n";
}

void test_median3_kills_a_spike() {
    int raw = 10;
    auto clean = nsig::fixed::sensor([&] { return raw; }, nsig::median3<int>{});
    clean.sample();
    clean.sample();
    eq(clean(), 10, "steady");
    raw = 900;  // a single spike
    clean.sample();
    eq(clean(), 10, "spike rejected");
    raw = 10;
    clean.sample();
    eq(clean(), 10, "still steady");
    std::cout << "  ok  median3_kills_a_spike\n";
}

void test_sample_is_one_pass() {
    int a = 0, b = 0;
    auto sa = nsig::fixed::sensor([&] { return a; });
    auto sb = nsig::fixed::sensor([&] { return b; });
    int runs = 0;
    auto o = nsig::st::watch([&] { return sa() + sb(); }, [&](int) { ++runs; });

    eq(runs, 1, "initial");
    a = 1;
    b = 1;
    nsig::fixed::sample(sa, sb);
    eq(runs, 2, "two sensors, one pass over the outputs");
    (void)o;
    std::cout << "  ok  sample_is_one_pass\n";
}

void test_actuator_drives_hardware_on_assignment() {
    std::vector<int> pin;
    nsig::fixed::actuator led{false, [&](bool on) { pin.push_back(on ? 1 : 0); }};

    eq(pin.size(), std::size_t{1}, "left in a known state at construction");
    eq(pin[0], 0, "low");

    led = true;
    eq(pin.size(), std::size_t{2}, "assignment drives the pin");
    eq(pin[1], 1, "high");
    led = true;
    eq(pin.size(), std::size_t{2}, "no driver call for an unchanged value");
    led.toggle();
    eq(pin.back(), 0, "toggle");
    led.refresh();
    eq(pin.size(), std::size_t{4}, "refresh re-asserts without touching the graph");
    std::cout << "  ok  actuator_drives_hardware_on_assignment\n";
}

void test_actuator_is_a_node() {
    nsig::fixed::actuator led{false, [](bool) {}};
    nsig::st::signal<bool> fault{false};
    auto usable = nsig::st::computed([&] { return led() && !fault(); });

    check(!usable(), "off");
    led = true;
    check(usable(), "the commanded state is readable in the graph");
    fault = true;
    check(!usable(), "and reacts");
    std::cout << "  ok  actuator_is_a_node\n";
}

void test_drive_binds_an_actuator_to_an_expression() {
    std::vector<int> valve;
    nsig::fixed::actuator v{false, [&](bool open) { valve.push_back(open ? 1 : 0); }};
    nsig::st::signal<float> temp{20.0f};
    auto binding = nsig::fixed::drive(v, temp > 30.0f);

    eq(valve.size(), std::size_t{1}, "still shut");
    temp = 40.0f;
    check(v(), "follows the expression");
    eq(valve.back(), 1, "hardware followed too");
    temp = 25.0f;
    check(!v(), "and back");
    (void)binding;
    std::cout << "  ok  drive_binds_an_actuator_to_an_expression\n";
}

void test_edges() {
    nsig::st::signal temp{20.0f};
    int alarms = 0, all_clears = 0;
    auto hot = nsig::st::computed<bool>(nsig::schmitt(temp, 60.0f, 70.0f));
    auto a = nsig::fixed::on_rising([&] { return hot(); }, [&] { ++alarms; });
    auto c = nsig::fixed::on_falling([&] { return hot(); }, [&] { ++all_clears; });

    eq(alarms, 0, "no alarm while cold");
    temp = 75.0f;
    eq(alarms, 1, "alarm on the rising edge");
    temp = 65.0f;
    eq(alarms, 1, "no repeat inside the band");
    temp = 55.0f;
    eq(all_clears, 2, "all-clear fires (once at construction, once on the edge)");
    (void)a;
    (void)c;
    std::cout << "  ok  edges\n";
}

void test_push_from_outside() {
    // An ISR counter or an MQTT payload arrives by another route; the same
    // filter still applies.
    auto rpm = nsig::fixed::sensor([] { return 0; }, nsig::ema{0.5f});
    rpm.push(1000);
    eq(rpm(), 500, "filtered on the way in");
    rpm.force(1200);
    eq(rpm(), 1200, "force bypasses the filter");
    std::cout << "  ok  push_from_outside\n";
}

}  // namespace

int main() {
    test_lifted_math();
    test_schmitt_is_idempotent_in_the_graph();
    test_peak_and_trough();
    test_tracked_sees_every_cycle_memo_does_not();
    test_sensor_reads_a_plain_function();
    test_ema_advances_once_per_sample();
    test_slew_ramps();
    test_debounce();
    test_median3_kills_a_spike();
    test_sample_is_one_pass();
    test_actuator_drives_hardware_on_assignment();
    test_actuator_is_a_node();
    test_drive_binds_an_actuator_to_an_expression();
    test_edges();
    test_push_from_outside();
    std::cout << "\n" << (checks - failures) << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
