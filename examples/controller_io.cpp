// Talking to a controller: ordinary driver calls in, reactive rules in the
// middle, peripherals and telemetry out.
//
// Everything a microcontroller touches is an imperative function — `adc_read()`,
// `ledc_set_duty()`, an MQTT callback, an ISR counter. This example is about the
// two seams where those meet the graph, and about which side of the seam each
// piece of logic belongs on.
//
//   c++ -std=c++2b -Os -fno-exceptions -fno-rtti -DNSIG_THREAD_LOCAL= \
//       -Iinclude examples/controller_io.cpp

#include <nsig/adapt.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

// ===========================================================================
// The driver layer — plain functions, exactly as ESP-IDF hands them to you
// ===========================================================================

namespace hw {

// Simulated hardware state, so the example runs on a host.
inline float coolant_c = 20.0f;
inline int pot_raw = 0;         // 12-bit setpoint knob
inline bool button_pin = false;
inline int tach_pulses = 0;

inline float read_ds18b20() { return coolant_c; }
inline int read_adc() { return pot_raw; }
inline bool read_button() { return button_pin; }
inline int take_tach_count() { return std::exchange(tach_pulses, 0); }

inline void set_pwm(int percent) { std::printf("      PWM   %3d%%\n", percent); }
inline void gpio_set_level(int pin, bool high) {
    std::printf("      GPIO%-2d %s\n", pin, high ? "HIGH" : "LOW");
}
inline void alarm(const char* what) { std::printf("      ALARM %s\n", what); }
inline void publish(const std::string& topic, const std::string& payload) {
    std::printf("      MQTT  %-16s %s\n", topic.c_str(), payload.c_str());
}

}  // namespace hw

// ===========================================================================
// Seam 1 — readings in
// ===========================================================================
//
// A `sensor` owns the driver call and a filter. `sample()` takes exactly one
// reading and advances the filter exactly once, whatever else the graph is
// doing. That is why filters live here and not in the graph: an EMA inside a
// `memo` would drift every time an unrelated cell was written.

namespace app {

/// 12-bit DS18B20 dithers by a sixteenth of a degree; smooth it, then let the
/// cell's deadband stop what is left from reaching the graph.
template <int Milli>
struct deadband {
    bool operator()(float a, float b) const noexcept {
        const float d = a - b;
        return (d < 0 ? -d : d) < Milli / 1000.0f;
    }
};

nsig::fixed::sensor<float (*)(), nsig::ema, deadband<100>> coolant{&hw::read_ds18b20, nsig::ema{0.3f}};
nsig::fixed::sensor<int (*)(), nsig::median3<int>> knob{&hw::read_adc, nsig::median3<int>{}};
nsig::fixed::sensor<bool (*)(), nsig::debounce<bool>> button{&hw::read_button, nsig::debounce<bool>{3}};
nsig::fixed::sensor<int (*)()> tach{&hw::take_tach_count};

// ===========================================================================
// Commands in — the other way values arrive
// ===========================================================================
//
// An MQTT callback or a serial parser runs on someone else's task. It must not
// touch the graph directly; it writes a cell, and the control task owns when
// that happens. `push`/`force` on a sensor do the same for filtered inputs.

enum class Mode { automatic, manual, off };

nsig::st::signal<Mode> mode{Mode::automatic};
nsig::st::signal<int> manual_duty{0};
nsig::st::signal<bool> online{false};

void on_command(const std::string& topic, const std::string& payload);


// ===========================================================================
// The rules — pure, and the only part worth reading twice
// ===========================================================================

/// The knob is a setpoint in degrees, not an ADC count.
auto setpoint = nsig::st::computed(nsig::map_range(knob, 0, 4095, 20.0f, 90.0f));

/// Two thresholds, not one: without hysteresis the pump would chatter at the
/// crossing point. `schmitt` is idempotent, so it is safe inside the graph.
auto cooling = nsig::st::computed<bool>(nsig::schmitt(coolant - setpoint, -1.0f, 1.0f));

/// Proportional above the setpoint, floored so the fan keeps turning.
auto curve = nsig::st::computed(nsig::clamp((coolant - setpoint) * 8.0f + 30.0f, 30.0f, 100.0f));

auto target = nsig::st::computed<int>([] {
    switch (mode()) {
        case Mode::off: return 0;
        case Mode::manual: return manual_duty();
        case Mode::automatic: break;
    }
    return cooling() ? static_cast<int>(curve()) : 0;
});

/// The highest coolant temperature this run.
///
/// `tracked`, not `memo`: a running aggregate has to see every cycle. As a lazy
/// `memo` this would report the peak of whichever samples something happened to
/// read it on — which is nothing at all if the only reader is a final printf.
auto worst = nsig::st::fold<float>(nsig::peak(coolant));

auto stalled = nsig::st::computed(target > 15 && tach < 5);

// ===========================================================================
// Seam 2 — values out
// ===========================================================================
//
// An `output` runs when its value changes and not otherwise. That is the only
// change detection in the whole program: no `if (duty != last_duty)` anywhere.

auto pwm = nsig::st::watch(target, [](int d) { hw::set_pwm(d); });

/// An actuator is a pin you can assign. The driver call happens only when the
/// value changes, so `= true` twice costs a comparison.
nsig::fixed::actuator valve{false, [](bool open) { hw::gpio_set_level(5, open); }};
nsig::fixed::actuator beeper{false, [](bool on) { hw::gpio_set_level(6, on); }};

/// ...and `drive` hands it to the graph, so it follows a rule instead.
auto valve_binding = nsig::fixed::drive(valve, cooling);

auto stall_alarm = nsig::fixed::on_rising(stalled, [] { hw::alarm("fan stalled"); });
auto overheat = nsig::fixed::on_rising(coolant > 95.0f, [] { hw::alarm("overheat"); });

/// The beeper is left imperative on purpose: a command can sound it, and so can
/// a rule. Mixing the two is fine — the last writer wins until the next cycle.
auto beep_on_overheat = nsig::fixed::on_rising(coolant > 95.0f, [] { beeper = true; });

/// Telemetry carries the connection flag in the value it compares, so a
/// reconnect changes the pair and everything is republished — no bookkeeping.
auto pub_temp = nsig::st::watch(
    [] { return std::pair{online(), coolant()}; },
    [](const std::pair<bool, float>& g) {
        if (g.first) hw::publish("state/coolant", std::to_string(g.second));
    });

auto pub_duty = nsig::st::watch([] { return std::pair{online(), target()}; },
                                 [](const std::pair<bool, int>& g) {
                                     if (g.first) hw::publish("state/duty", std::to_string(g.second));
                                 });

// ===========================================================================
// The control loop
// ===========================================================================

void on_command(const std::string& topic, const std::string& payload) {
    nsig::st::batch guard;  // a command that sets two things is still one pass
    if (topic == "cmd/mode") {
        mode = payload == "manual" ? Mode::manual : payload == "off" ? Mode::off : Mode::automatic;
    } else if (topic == "cmd/duty") {
        manual_duty = std::stoi(payload);
        mode = Mode::manual;
    } else if (topic == "cmd/beep") {
        beeper = payload == "on";  // straight at the pin
    }
}

void silence() { beeper = false; }

void tick() {
    // One batch: four driver calls, one pass over the peripherals.
    nsig::fixed::sample(coolant, knob, button, tach);
}

}  // namespace app

// ===========================================================================
// Simulation
// ===========================================================================

namespace {

int step = 0;
void phase(const char* what) { std::printf("\n%2d. %s\n", ++step, what); }

}  // namespace

int main() {
    std::printf("boot\n");

    phase("idle at 20 C, knob at minimum");
    app::tick();

    phase("broker connects");
    app::online = true;

    phase("coolant climbs to 60 C — the EMA needs a few samples to follow");
    hw::coolant_c = 60.0f;
    for (int i = 0; i < 6; ++i) app::tick();

    phase("a single ADC spike on the knob is rejected by the median filter");
    hw::pot_raw = 4000;
    app::tick();
    hw::pot_raw = 0;
    app::tick();

    phase("operator turns the knob to ~55 C");
    hw::pot_raw = 2048;
    for (int i = 0; i < 3; ++i) app::tick();

    phase("fan is commanded but the tach reports nothing");
    app::tick();

    phase("tach recovers and keeps reporting");
    hw::tach_pulses = 40;
    app::tick();
    hw::tach_pulses = 40;

    phase("MQTT command: manual 45%");
    app::on_command("cmd/duty", "45");

    phase("MQTT command straight at a pin: beeper on, then off");
    app::on_command("cmd/beep", "on");
    app::on_command("cmd/beep", "off");

    phase("contact bounce on the mode button is ignored");
    hw::button_pin = true;
    hw::tach_pulses = 40;
    app::tick();
    hw::button_pin = false;
    hw::tach_pulses = 40;
    app::tick();

    phase("back to automatic");
    app::on_command("cmd/mode", "auto");

    phase("coolant spikes to 99 C");
    hw::coolant_c = 99.0f;
    for (int i = 0; i < 8; ++i) {
        hw::tach_pulses = 40;
        app::tick();
    }

    phase("broker drops, coolant falls to 30 C");
    app::online = false;
    hw::coolant_c = 30.0f;
    for (int i = 0; i < 10; ++i) {
        hw::tach_pulses = 40;
        app::tick();
    }

    std::printf("\n---\nworst coolant seen: %.1f C\n", static_cast<double>(app::worst()));
}
