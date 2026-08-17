// Five-channel PWM fan controller on ESP32-C6-WROOM-1U.
//
// Hardware: 5x 4-pin fan headers (PWM through a 2N7002 open-drain stage, tach
// pulled up 10k to 3V3), DS18B20 on GPIO1, 12 V -> 5 V -> 3V3.
//
// The control logic below has no state machine, no "did it change?" compares
// and no update ordering. It declares what each output *is*, and the graph
// works out what to recompute — which matters twice over on a microcontroller,
// because a recomputation avoided is a register write avoided and a radio
// packet avoided.
//
// The file is deliberately split in two halves:
//
//   * pure state — signals and computeds. No side effects, safe to construct
//     at static-init time, before any peripheral exists.
//   * outputs — effects that touch hardware. Created inside one `effect_scope`
//     after `board::init()`, and disposable as a group.
//
// Build for the board (ESP-IDF 5.3+, GCC 13):
//     idf.py build
//   with, in your component's CMakeLists.txt:
//     target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++23)
//     target_compile_definitions(${COMPONENT_LIB} PRIVATE NSIG_THREAD_LOCAL=)
//
// Build the host simulation:
//     c++ -std=c++2b -Os -fno-exceptions -fno-rtti -DNSIG_THREAD_LOCAL= \
//         -Iinclude examples/esp32_fan_controller.cpp

#include <nsig/expr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

// ===========================================================================
// Board layer — the only part that differs between the target and the host.
// ===========================================================================

namespace board {

inline constexpr int channels = 5;
inline constexpr std::uint32_t red = 0xff0000, amber = 0xff8000, green = 0x00ff00;

#ifdef ESP_PLATFORM
#  include "driver/ledc.h"
#  include "esp_log.h"

inline void set_pwm(int ch, int percent) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(ch), percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(ch));
}
inline void set_led(std::uint32_t rgb) { ws2812_write(rgb); }
inline void publish(const std::string& topic, const std::string& payload) {
    esp_mqtt_client_publish(mqtt, topic.c_str(), payload.c_str(), 0, 0, /*retain=*/1);
}
inline void log(const std::string& msg) { ESP_LOGI("fan", "%s", msg.c_str()); }
#else
// Host stubs, so the reactive logic can be exercised without hardware. Each
// call is counted, which is how the simulation shows that an output is written
// only when it genuinely changes.
inline int pwm_writes = 0, publishes = 0;
inline void set_pwm(int ch, int percent) {
    ++pwm_writes;
    std::printf("      PWM  ch%d -> %3d%%\n", ch, percent);
}
inline void set_led(std::uint32_t rgb) { std::printf("      LED  #%06x\n", rgb); }
inline void publish(const std::string& topic, const std::string& payload) {
    ++publishes;
    std::printf("      MQTT %-22s %s\n", topic.c_str(), payload.c_str());
}
inline void log(const std::string& msg) { std::printf("      LOG  %s\n", msg.c_str()); }
#endif

inline std::string fmt1(float v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(v));
    return buf;
}

}  // namespace board

// ===========================================================================
// Change-detection policies
// ===========================================================================

/// A deadband, so sensor jitter never reaches the graph. A DS18B20 in 12-bit
/// mode dithers by a sixteenth of a degree; without this, every conversion
/// would invalidate the fan curve and rewrite five PWM registers.
template <int Milli>
struct deadband {
    [[nodiscard]] bool operator()(float a, float b) const noexcept {
        return std::fabs(a - b) < Milli / 1000.0f;
    }
};

/// A few RPM of tach jitter is not news either.
struct rpm_equal {
    [[nodiscard]] bool operator()(int a, int b) const noexcept { return std::abs(a - b) < 30; }
};

// ===========================================================================
// Part 1 — pure state
// ===========================================================================

namespace app {

enum class Mode { automatic, silent, manual };

// --- inputs, written only by the control task ------------------------------

nsig::signal<float, deadband<250>> temp_c{25.0f};  // DS18B20 on GPIO1
nsig::signal<bool> temp_ok{true};                  // 1-Wire presence + CRC
std::array<nsig::signal<int, rpm_equal>, board::channels> rpm{};
std::array<nsig::signal<int>, board::channels> trim{};  // per-connector offset, %
nsig::signal<Mode> mode{Mode::automatic};
nsig::signal<int> manual_duty{0};
nsig::signal<bool> mqtt_online{false};

/// Stall detection needs a time base, and the graph deliberately has none — it
/// reacts, it does not schedule. The control task arms this a couple of seconds
/// after a spin-up, which turns "has it had time to spin up?" into an ordinary
/// input and keeps the rule below a plain conjunction.
nsig::signal<bool> tach_armed{false};

// --- derivations -----------------------------------------------------------

/// The fan curve, with hysteresis.
///
/// Hysteresis is a stateful function: what it returns depends on what it
/// returned last time. Normally that means a member variable and the discipline
/// to keep it in sync. Here the previous value is just an argument — the
/// incremental `computed` form passes it in (`nullptr` on the first
/// evaluation) and the function stays pure.
nsig::computed<int> curve_duty{[](const int* previous) -> int {
    constexpr float lo = 30.0f, hi = 60.0f, hysteresis = 2.0f;
    const float t = temp_c();
    const bool running = previous != nullptr && *previous > 0;
    if (t <= (running ? lo - hysteresis : lo)) return 0;  // sticky once spinning
    const int pct = static_cast<int>(std::lround((t - lo) / (hi - lo) * 100.0f));
    return std::clamp(pct, 25, 100);                      // 25% keeps them turning
}};

/// What the fans should be doing.
///
/// In manual mode `curve_duty` is never read, so it is not a dependency — a
/// temperature change then costs exactly nothing. Dependencies are whatever the
/// last evaluation actually touched, not whatever the function mentions.
nsig::computed<int> target_duty{[] {
    if (!temp_ok()) return 100;  // sensor lost: fail towards cooling
    switch (mode()) {
        case Mode::manual: return manual_duty();
        case Mode::silent: return std::min(curve_duty(), 40);
        case Mode::automatic: break;
    }
    return curve_duty();
}};

constexpr auto clamp_duty = nsig::lift([](int v) { return std::clamp(v, 0, 100); });

auto duty = nsig::make_array<board::channels>(
    [](std::size_t i) { return nsig::computed<int>{clamp_duty(target_duty + trim[i])}; });

/// Reads as the rule it is: armed, commanded to spin, and not spinning.
auto stalled = nsig::make_array<board::channels>([](std::size_t i) {
    return nsig::computed<bool>{tach_armed && duty[i] > 15 && rpm[i] < 100};
});

nsig::computed<bool> any_fault{[] {
    return !temp_ok() || std::ranges::any_of(stalled, [](auto& s) { return s(); });
}};

/// Memoise at the value that actually reaches the peripheral, not at the one it
/// is derived from. Written as `any_fault() ? red : ...` inside the effect, the
/// LED would be rewritten on every duty change — the effect re-runs, sees the
/// same colour, and writes it anyway. As a `computed`, an unchanged colour
/// stops the propagation before the effect is ever queued.
nsig::computed<std::uint32_t> led_colour{
    nsig::pick(any_fault, board::red, nsig::pick(target_duty > 70, board::amber, board::green))};

/// Same reasoning for the log: a summary that only changes when the set of
/// faulty channels changes.
nsig::computed<std::string> fault_text{[] {
    if (!temp_ok()) return std::string("DS18B20 not responding");
    std::string s;
    for (std::size_t i = 0; i < stalled.size(); ++i)
        if (stalled[i]()) s += (s.empty() ? "stalled: ch" : ", ch") + std::to_string(i);
    return s;
}};

// ===========================================================================
// Part 2 — outputs
// ===========================================================================

/// Every effect that touches a peripheral lives here, so nothing runs before
/// `board::init()` and everything can be stopped at once (OTA, deep sleep, a
/// self-test mode).
nsig::effect_scope outputs;

void start_outputs() {
    outputs.run([] {
        // The only place a PWM register is ever written. Each of these runs
        // when *its own* channel's duty changes, not when the temperature does.
        [](auto... i) {
            (nsig::spawn([i] { board::set_pwm(i, duty[i]()); }), ...);
        }(std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4});

        nsig::spawn([] { board::set_led(led_colour()); });

        nsig::spawn([] {
            if (!fault_text().empty()) board::log(fault_text());
        });

        // Telemetry exists only while the broker is reachable. When
        // `mqtt_online` goes false this effect re-runs, and the graph disposes
        // every publisher below it — running their cleanups first — before the
        // body starts again. Reconnecting rebuilds the subtree. There is no
        // subscribe/unsubscribe bookkeeping anywhere in this file.
        nsig::spawn([] {
            if (!mqtt_online()) return;

            nsig::spawn([] { board::publish("fan/temperature", board::fmt1(temp_c())); });
            nsig::spawn([] { board::publish("fan/duty", std::to_string(target_duty())); });
            nsig::spawn([] { board::publish("fan/fault", any_fault() ? "ON" : "OFF"); });
            [](auto... i) {
                (nsig::spawn([i] {
                     board::publish("fan/ch" + std::to_string(i) + "/rpm",
                                    std::to_string(rpm[i]()));
                 }),
                 ...);
            }(std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4});

            nsig::on_cleanup([] { board::log("telemetry torn down"); });
        });
    });
}

}  // namespace app

// ===========================================================================
// Threading
// ===========================================================================
//
// All runtime state lives in one `nsig` runtime. Build with
// `-DNSIG_THREAD_LOCAL=` so it is a single global, and touch signals from
// exactly one task — the control task below.
//
// Everything else hands values over rather than writing them:
//
//   * The tach ISR does nothing but `++pulses[ch]` on a volatile counter. It
//     must not enter the graph: propagation allocates from a pool, calls user
//     code and is not reentrant.
//   * The Wi-Fi/MQTT task posts events to a FreeRTOS queue.
//   * The control task drains both, writes the signals inside one `nsig::batch`
//     and flushes once. A whole sweep of sensor updates therefore produces a
//     single pass over the PWM registers, however many inputs moved.
//
// `nsig::set_scheduler` is the hook for the variant where other tasks may write
// directly: point it at `xTaskNotifyGive(control_task)` and no flush will ever
// run on a writer's stack.

#ifdef ESP_PLATFORM
extern "C" void app_main() {
    // Size the pools once. Static initialisation has already built the pure
    // half of the graph, so this covers everything from here on: after it, the
    // steady state performs no heap allocation at all. Check with
    // nsig::get_stats().
    nsig::reserve(/*node bytes=*/4096, /*links=*/128, /*queued effects=*/32);

    board::init_ledc();
    board::init_tach_isr();
    app::start_outputs();  // hardware exists now, so effects may run

    std::uint32_t spin_up_at = 0;
    while (true) {
        {
            nsig::batch sweep;
            if (auto t = board::read_ds18b20()) {
                app::temp_c = *t;
                app::temp_ok = true;
            } else {
                app::temp_ok = false;
            }
            for (int i = 0; i < board::channels; ++i) app::rpm[i] = board::sample_rpm(i);

            const bool spinning = app::target_duty.peek() > 0;
            if (!spinning) spin_up_at = 0, app::tach_armed = false;
            else if (spin_up_at == 0) spin_up_at = board::now_ms();
            else app::tach_armed = board::now_ms() - spin_up_at > 3000;

            drain_event_queue();
        }  // <- one flush here: one PWM pass, one batch of publishes
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#else

// ---------------------------------------------------------------------------
// Host simulation: drive the inputs and watch what the graph decides to do.
// ---------------------------------------------------------------------------

namespace {

int step = 0;

void sweep(const char* what, auto&& body) {
    std::printf("\n%2d. %s\n", ++step, what);
    nsig::batch b;
    body();
}

}  // namespace

int main() {
    nsig::reserve(4096, 128, 32);
    app::start_outputs();
    std::printf("\nboot: %d PWM writes, %d publishes\n", board::pwm_writes, board::publishes);

    sweep("cold start at 25 C — below the curve", [] { app::temp_c = 25.0f; });
    sweep("28.9 C — still below the 30 C knee", [] { app::temp_c = 28.9f; });
    sweep("warming to 45 C", [] { app::temp_c = 45.0f; });
    sweep("45.1 C — inside the 0.25 C deadband, nothing happens", [] { app::temp_c = 45.1f; });
    sweep("fans have had 3 s to spin up; all healthy", [] {
        app::tach_armed = true;
        for (int i = 0; i < board::channels; ++i) app::rpm[i] = 1200;
    });
    sweep("broker connects — the telemetry subtree is built", [] { app::mqtt_online = true; });
    sweep("tach says channel 2 has stopped", [] { app::rpm[2] = 0; });
    sweep("channel 2 recovers", [] { app::rpm[2] = 1150; });
    sweep("silent mode — duty capped at 40%", [] { app::mode = app::Mode::silent; });
    sweep("70 C in silent mode — the cap holds, no PWM write", [] { app::temp_c = 70.0f; });
    sweep("back to automatic", [] { app::mode = app::Mode::automatic; });
    sweep("manual at 55% — the curve stops being a dependency", [] {
        app::mode = app::Mode::manual;
        app::manual_duty = 55;
    });
    sweep("temperature swings hard; manual mode ignores it", [] { app::temp_c = 20.0f; });
    sweep("...and again", [] { app::temp_c = 65.0f; });
    sweep("channel 3 has a dirty connector: -10% trim", [] { app::trim[3] = -10; });
    sweep("DS18B20 drops off the bus — fail to full cooling", [] {
        app::mode = app::Mode::automatic;
        app::temp_ok = false;
    });
    sweep("broker drops — publishers are disposed", [] { app::mqtt_online = false; });
    sweep("sensor returns at 32 C", [] {
        app::temp_ok = true;
        app::temp_c = 32.0f;
    });
    sweep("cooling to 29 C — hysteresis keeps them running below 30",
          [] { app::temp_c = 29.0f; });
    sweep("27 C — past the hysteresis band, fans stop", [] { app::temp_c = 27.0f; });

    const auto s = nsig::get_stats();
    std::printf("\n---\n%d PWM writes, %d publishes across %d input sweeps\n", board::pwm_writes,
                board::publishes, step);
    std::printf("graph: %llu links, %llu node updates, %llu effect runs\n",
                static_cast<unsigned long long>(s.links_created),
                static_cast<unsigned long long>(s.node_updates),
                static_cast<unsigned long long>(s.effect_runs));
}
#endif
