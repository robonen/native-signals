// What does the reactive graph actually cost on a microcontroller?
//
// This runs the fan controller from examples/esp32_fan_controller.cpp twice:
// once as a reactive graph, once as a hand-written imperative controller that a
// careful embedded developer would produce — same deadband, same hysteresis,
// same "only write the register if the value changed" discipline.
//
// The two are asserted to emit byte-identical output traces first, then timed.
// Instruction counts (callgrind) matter more than host nanoseconds here,
// because they carry over to a different ISA far better than wall time does.
#include "nsig/signals.hpp"
#include "nsig/expr.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int channels = 5;
enum class Mode { automatic, silent, manual };

/// One reading of every input, as the control task would gather it.
struct Sample {
    float temp;
    bool temp_ok;
    std::array<int, channels> rpm;
    Mode mode;
    int manual_duty;
    bool mqtt;
    bool armed;
};

// A trace entry is what the board layer would have been told to do.
using Trace = std::vector<std::string>;

// ---------------------------------------------------------------------------
// Shared rules, so neither implementation can cheat
// ---------------------------------------------------------------------------

constexpr float temp_deadband = 0.25f;
constexpr int rpm_deadband = 30;

int curve_from(float t, int previous) {
    constexpr float lo = 30.0f, hi = 60.0f, hysteresis = 2.0f;
    const bool running = previous > 0;
    if (t <= (running ? lo - hysteresis : lo)) return 0;
    return std::clamp(static_cast<int>(std::lround((t - lo) / (hi - lo) * 100.0f)), 25, 100);
}

std::string fmt1(float v) {
    char b[16];
    std::snprintf(b, sizeof b, "%.1f", static_cast<double>(v));
    return b;
}

// ---------------------------------------------------------------------------
// Reactive implementation
// ---------------------------------------------------------------------------

template <int Milli>
struct deadband {
    bool operator()(float a, float b) const noexcept {
        return std::fabs(a - b) < Milli / 1000.0f;
    }
};
struct rpm_equal {
    bool operator()(int a, int b) const noexcept { return std::abs(a - b) < rpm_deadband; }
};

struct Reactive {
    Trace* out;

    nsig::signal<float, deadband<250>> temp_c{25.0f};
    nsig::signal<bool> temp_ok{true};
    std::array<nsig::signal<int, rpm_equal>, channels> rpm{};
    nsig::signal<Mode> mode{Mode::automatic};
    nsig::signal<int> manual_duty{0};
    nsig::signal<bool> mqtt_online{false};
    nsig::signal<bool> tach_armed{false};

    nsig::computed<int> curve_duty{
        [this](const int* prev) { return curve_from(temp_c(), prev ? *prev : 0); }};

    nsig::computed<int> target_duty{[this] {
        if (!temp_ok()) return 100;
        switch (mode()) {
            case Mode::manual: return manual_duty();
            case Mode::silent: return std::min(curve_duty(), 40);
            case Mode::automatic: break;
        }
        return curve_duty();
    }};

    std::array<nsig::computed<int>, channels> duty;
    std::array<nsig::computed<bool>, channels> stalled;
    nsig::computed<bool> any_fault{[this] {
        return !temp_ok() || std::ranges::any_of(stalled, [](auto& s) { return s(); });
    }};

    /// The LED effect must re-run only when the *colour* changes, not whenever
    /// the duty it is derived from moves. Memoising at the value that reaches
    /// the peripheral is the whole discipline in one line.
    nsig::computed<const char*> led_colour{[this] {
        return any_fault() ? "red" : target_duty() > 70 ? "amber" : "green";
    }};

    nsig::effect_scope outputs;

    explicit Reactive(Trace* t)
        : out(t),
          duty(make_duty(std::make_index_sequence<channels>{})),
          stalled(make_stalled(std::make_index_sequence<channels>{})) {
        outputs.run([this] {
            build(std::make_index_sequence<channels>{});
        });
    }

    template <std::size_t... I>
    std::array<nsig::computed<int>, channels> make_duty(std::index_sequence<I...>) {
        return {nsig::computed<int>{[this] {
            (void)I;
            return target_duty();
        }}...};
    }
    template <std::size_t... I>
    std::array<nsig::computed<bool>, channels> make_stalled(std::index_sequence<I...>) {
        return {nsig::computed<bool>{
            [this] { return tach_armed() && duty[I]() > 15 && rpm[I]() < 100; }}...};
    }

    template <std::size_t... I>
    void build(std::index_sequence<I...>) {
        (nsig::spawn([this] {
             out->push_back("pwm " + std::to_string(I) + " " + std::to_string(duty[I]()));
         }),
         ...);
        nsig::spawn([this] { out->push_back(std::string("led ") + led_colour()); });
        nsig::spawn([this] {
            if (!mqtt_online()) return;
            nsig::spawn([this] { out->push_back("pub temp " + fmt1(temp_c())); });
            nsig::spawn([this] { out->push_back("pub duty " + std::to_string(target_duty())); });
            nsig::spawn([this] {
                out->push_back(std::string("pub fault ") + (any_fault() ? "ON" : "OFF"));
            });
            (nsig::spawn([this] {
                 out->push_back("pub rpm " + std::to_string(I) + " " + std::to_string(rpm[I]()));
             }),
             ...);
        });
    }

    void sweep(const Sample& s) {
        nsig::batch b;
        temp_c = s.temp;
        temp_ok = s.temp_ok;
        for (int i = 0; i < channels; ++i) rpm[static_cast<std::size_t>(i)] = s.rpm[static_cast<std::size_t>(i)];
        mode = s.mode;
        manual_duty = s.manual_duty;
        mqtt_online = s.mqtt;
        tach_armed = s.armed;
    }
};


// ---------------------------------------------------------------------------
// Statically wired implementation — no heap graph and no dependency lists.
// Expressions re-run when the epoch moves; change detection happens where the
// value leaves the graph.
// ---------------------------------------------------------------------------

struct Cells {
    nsig::st::signal<float, deadband<250>> temp{25.0f};
    nsig::st::signal<bool> temp_ok{true};
    std::array<nsig::st::signal<int, rpm_equal>, channels> rpm{};
    nsig::st::signal<Mode> mode{Mode::automatic};
    nsig::st::signal<int> manual_duty{0};
    nsig::st::signal<bool> mqtt_online{false};
    nsig::st::signal<bool> tach_armed{false};
};

/// Hysteresis stays a pure function: the incremental form supplies the previous
/// duty, so there is no member to keep in sync.
inline int target_of(const Cells& c, const int* previous) {
    if (!c.temp_ok()) return 100;
    if (c.mode() == Mode::manual) return c.manual_duty();
    const int curve = curve_from(c.temp(), previous != nullptr ? *previous : 0);
    return c.mode() == Mode::silent ? std::min(curve, 40) : curve;
}

inline bool fault_of(const Cells& c, int duty) {
    if (!c.temp_ok()) return true;
    for (std::size_t i = 0; i < channels; ++i)
        if (c.tach_armed() && duty > 15 && c.rpm[i]() < 100) return true;
    return false;
}

/// The one memo here: five PWM outputs and two telemetry topics all want the
/// duty in the same cycle, and the curve is the only derivation that is not
/// trivially cheap.
inline auto make_duty(const Cells& c) {
    return nsig::st::computed<int>([&c](const int* previous) { return target_of(c, previous); });
}
using Duty = decltype(make_duty(std::declval<const Cells&>()));

/// Telemetry carries the connection state in the value it compares. A
/// disconnect changes the pair, so the sink runs and publishes nothing; a
/// reconnect changes it back, so everything is republished — without any
/// invalidate bookkeeping.
using Gated = std::pair<bool, int>;

inline auto mk_pwm(const Duty& duty, Trace* out, std::size_t i) {
    return nsig::st::watch([&duty] { return duty.get(); }, [out, i](int d) {
        out->push_back("pwm " + std::to_string(i) + " " + std::to_string(d));
    });
}

inline auto mk_led(const Cells& c, const Duty& duty, Trace* out) {
    // Sugar, and measurably free: swapping this for the equivalent lambda moves
    // neither the instruction count nor the timings.
    const auto faulty = nsig::lift([&c](int d) { return fault_of(c, d); });
    return nsig::st::watch(
        nsig::pick(faulty(duty), "red", nsig::pick(duty > 70, "amber", "green")),
        [out](const char* colour) { out->push_back(std::string("led ") + colour); });
}

inline auto mk_pub_temp(const Cells& c, Trace* out) {
    return nsig::st::watch(
        [&c] { return std::pair<bool, float>{c.mqtt_online(), c.temp()}; },
        [out](const std::pair<bool, float>& g) {
            if (g.first) out->push_back("pub temp " + fmt1(g.second));
        });
}

inline auto mk_pub_duty(const Cells& c, const Duty& duty, Trace* out) {
    return nsig::st::watch([&c, &duty] { return Gated{c.mqtt_online(), duty.get()}; },
                            [out](const Gated& g) {
                                if (g.first) out->push_back("pub duty " + std::to_string(g.second));
                            });
}

inline auto mk_pub_fault(const Cells& c, const Duty& duty, Trace* out) {
    return nsig::st::watch(
        [&c, &duty] { return Gated{c.mqtt_online(), fault_of(c, duty.get()) ? 1 : 0}; },
        [out](const Gated& g) {
            if (g.first) out->push_back(std::string("pub fault ") + (g.second ? "ON" : "OFF"));
        });
}

inline auto mk_pub_rpm(const Cells& c, Trace* out, std::size_t i) {
    return nsig::st::watch([&c, i] { return Gated{c.mqtt_online(), c.rpm[i].get()}; },
                            [out, i](const Gated& g) {
                                if (g.first)
                                    out->push_back("pub rpm " + std::to_string(i) + " " +
                                                   std::to_string(g.second));
                            });
}

struct Static {
    Cells c;
    Trace* out;
    Duty duty{make_duty(c)};

    // Outputs register themselves and run on every write; holding them keeps
    // them alive, nothing else. Aggregate-initialising from prvalues works even
    // though they are non-movable — C++17 elides the construction in place.
    std::array<decltype(mk_pwm(std::declval<const Duty&>(), nullptr, 0)), channels> pwm{
        mk_pwm(duty, out, 0), mk_pwm(duty, out, 1), mk_pwm(duty, out, 2),
        mk_pwm(duty, out, 3), mk_pwm(duty, out, 4)};
    decltype(mk_led(std::declval<const Cells&>(), std::declval<const Duty&>(), nullptr)) led{
        mk_led(c, duty, out)};
    decltype(mk_pub_temp(std::declval<const Cells&>(), nullptr)) pub_temp{mk_pub_temp(c, out)};
    decltype(mk_pub_duty(std::declval<const Cells&>(), std::declval<const Duty&>(),
                         nullptr)) pub_duty{mk_pub_duty(c, duty, out)};
    decltype(mk_pub_fault(std::declval<const Cells&>(), std::declval<const Duty&>(),
                          nullptr)) pub_fault{mk_pub_fault(c, duty, out)};
    std::array<decltype(mk_pub_rpm(std::declval<const Cells&>(), nullptr, 0)), channels> pub_rpm{
        mk_pub_rpm(c, out, 0), mk_pub_rpm(c, out, 1), mk_pub_rpm(c, out, 2),
        mk_pub_rpm(c, out, 3), mk_pub_rpm(c, out, 4)};

    explicit Static(Trace* t) : out(t) {}

    void sweep(const Sample& s) {
        nsig::st::batch sweep;  // one pass over the peripherals, as in the dynamic version
        c.temp = s.temp;
        c.temp_ok = s.temp_ok;
        for (std::size_t i = 0; i < channels; ++i) c.rpm[i] = s.rpm[i];
        c.mode = s.mode;
        c.manual_duty = s.manual_duty;
        c.mqtt_online = s.mqtt;
        c.tach_armed = s.armed;
    }
};

// ---------------------------------------------------------------------------
// Hand-written implementation — no library, but no sloppiness either
// ---------------------------------------------------------------------------

struct Imperative {
    Trace* out;

    // committed inputs (deadbands applied)
    float temp = 25.0f;
    bool temp_ok = true;
    std::array<int, channels> rpm{};
    Mode mode = Mode::automatic;
    int manual_duty = 0;
    bool mqtt = false;
    bool armed = false;

    // outputs last written, so nothing is written twice
    int curve = 0;
    std::array<int, channels> last_duty{};
    std::array<bool, channels> last_stalled{};
    const char* last_led = nullptr;
    bool mqtt_built = false;
    float pub_temp = NAN;
    int pub_duty = -1;
    int pub_fault = -1;
    std::array<int, channels> pub_rpm{};

    explicit Imperative(Trace* t) : out(t) {
        last_duty.fill(-1);
        pub_rpm.fill(-1);
        boot();
    }

    void boot() {
        for (int i = 0; i < channels; ++i) write_duty(i, 0);
        set_led("green");
    }

    void write_duty(int i, int d) {
        if (last_duty[static_cast<std::size_t>(i)] == d) return;
        last_duty[static_cast<std::size_t>(i)] = d;
        out->push_back("pwm " + std::to_string(i) + " " + std::to_string(d));
    }
    void set_led(const char* c) {
        if (last_led != nullptr && std::strcmp(last_led, c) == 0) return;
        last_led = c;
        out->push_back(std::string("led ") + c);
    }

    void sweep(const Sample& s) {
        // --- commit inputs through their deadbands, tracking whether anything
        //     actually moved. This early-out is the hand-written equivalent of
        //     what the graph does on its own; without it the comparison would
        //     be rigged, because a real controller mostly sees noise.
        bool dirty = false;
        if (std::fabs(s.temp - temp) >= temp_deadband) temp = s.temp, dirty = true;
        if (s.temp_ok != temp_ok) temp_ok = s.temp_ok, dirty = true;
        for (int i = 0; i < channels; ++i) {
            const auto j = static_cast<std::size_t>(i);
            if (std::abs(s.rpm[j] - rpm[j]) >= rpm_deadband) rpm[j] = s.rpm[j], dirty = true;
        }
        if (s.mode != mode) mode = s.mode, dirty = true;
        if (s.manual_duty != manual_duty) manual_duty = s.manual_duty, dirty = true;
        if (s.armed != armed) armed = s.armed, dirty = true;
        if (s.mqtt != mqtt) dirty = true;
        if (!dirty) return;

        // --- recompute
        int target;
        if (!temp_ok) {
            target = 100;
        } else if (mode == Mode::manual) {
            target = manual_duty;
        } else {
            curve = curve_from(temp, curve);
            target = (mode == Mode::silent) ? std::min(curve, 40) : curve;
        }

        bool fault = !temp_ok;
        for (int i = 0; i < channels; ++i) {
            const auto j = static_cast<std::size_t>(i);
            const int d = std::clamp(target, 0, 100);
            write_duty(i, d);
            last_stalled[j] = armed && d > 15 && rpm[j] < 100;
            fault = fault || last_stalled[j];
        }
        set_led(fault ? "red" : target > 70 ? "amber" : "green");

        // --- telemetry, rebuilt on (re)connect exactly like the scope is
        if (s.mqtt && !mqtt_built) {
            mqtt_built = true;
            pub_temp = NAN;
            pub_duty = -1;
            pub_fault = -1;
            pub_rpm.fill(-1);
        } else if (!s.mqtt) {
            mqtt_built = false;
        }
        mqtt = s.mqtt;
        if (!mqtt) return;

        if (!(pub_temp == temp)) {
            pub_temp = temp;
            out->push_back("pub temp " + fmt1(temp));
        }
        if (pub_duty != target) {
            pub_duty = target;
            out->push_back("pub duty " + std::to_string(target));
        }
        if (pub_fault != static_cast<int>(fault)) {
            pub_fault = fault;
            out->push_back(std::string("pub fault ") + (fault ? "ON" : "OFF"));
        }
        for (int i = 0; i < channels; ++i) {
            const auto j = static_cast<std::size_t>(i);
            if (pub_rpm[j] != rpm[j]) {
                pub_rpm[j] = rpm[j];
                out->push_back("pub rpm " + std::to_string(i) + " " + std::to_string(rpm[j]));
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Workload
// ---------------------------------------------------------------------------

std::vector<Sample> make_workload(int n) {
    std::vector<Sample> v;
    v.reserve(static_cast<std::size_t>(n));
    Sample s{25.0f, true, {1200, 1200, 1200, 1200, 1200}, Mode::automatic, 0, false, false};
    unsigned rng = 12345;
    for (int i = 0; i < n; ++i) {
        const auto rnd = [&] { return (rng = rng * 1103515245u + 12345u) >> 16; };
        // A real controller mostly sees noise: the temperature wanders by a
        // few hundredths of a degree and the tach jitters by a few RPM.
        s.temp += (static_cast<float>(rnd() % 200) - 100.0f) / 2000.0f;
        for (auto& r : s.rpm) r = 1200 + static_cast<int>(rnd() % 20) - 10;
        s.armed = i > 3;
        s.mqtt = i > 5;
        if (i % 97 == 0) s.temp += 3.0f;                       // a real excursion
        if (i % 401 == 0) s.mode = Mode::silent;               // occasional mode change
        if (i % 401 == 200) s.mode = Mode::automatic;
        if (i % 613 == 0) s.mqtt = false;                      // broker flap
        v.push_back(s);
    }
    return v;
}

/// Per-sweep operation sets. Effects fire in graph-traversal order, which is
/// not source order, so the two implementations are compared sweep by sweep as
/// sets rather than as one flat sequence.
template <class Ctl>
std::vector<Trace> run(const std::vector<Sample>& work) {
    std::vector<Trace> sweeps;
    Trace t;
    Ctl ctl{&t};
    sweeps.push_back(t);  // boot
    for (const auto& s : work) {
        t.clear();
        ctl.sweep(s);
        Trace one = t;
        std::ranges::sort(one);
        sweeps.push_back(std::move(one));
    }
    for (auto& v : sweeps) std::ranges::sort(v);
    return sweeps;
}

template <class Ctl>
double time_ns(const std::vector<Sample>& work, int repeats) {
    Trace sink;
    Ctl ctl{&sink};
    for (const auto& s : work) ctl.sweep(s);  // warm up
    sink.clear();

    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < repeats; ++r) {
        for (const auto& s : work) {
            ctl.sweep(s);
            sink.clear();
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() /
           (static_cast<double>(repeats) * static_cast<double>(work.size()));
}

}  // namespace

int main(int argc, char** argv) {
    const auto work = make_workload(2000);

    // Under callgrind, run one implementation only and skip everything else so
    // the instruction count belongs to the sweep loop alone.
    const char* only = argc > 1 ? argv[1] : nullptr;
    const bool verify_only = only != nullptr && std::strcmp(only, "--verify") == 0;
    if (only != nullptr && !verify_only) {
        Trace sink;
        if (std::strcmp(only, "--reactive") == 0) {
            Reactive ctl{&sink};
            for (const auto& s : work) {
                ctl.sweep(s);
                sink.clear();
            }
        } else if (std::strcmp(only, "--imperative") == 0) {
            Imperative ctl{&sink};
            for (const auto& s : work) {
                ctl.sweep(s);
                sink.clear();
            }
        } else if (std::strcmp(only, "--static") == 0) {
            Static ctl{&sink};
            for (const auto& s : work) {
                ctl.sweep(s);
                sink.clear();
            }
        }
        // "--baseline" falls through having built only the workload, so its
        // instruction count is the fixed cost to subtract from the other two.
        return 0;
    }

    // 1. Equivalence: the comparison is meaningless unless both agree exactly.
    const auto a = run<Reactive>(work);
    const auto b = run<Imperative>(work);
    const auto c = run<Static>(work);
    if (c != b) {
        std::printf("STATIC DIFFERS FROM HAND-WRITTEN\n");
        for (std::size_t i = 0; i < std::min(c.size(), b.size()); ++i)
            if (c[i] != b[i]) {
                std::printf("  first difference at sweep %zu:\n", i);
                for (const auto& x : c[i]) std::printf("    static     %s\n", x.c_str());
                for (const auto& x : b[i]) std::printf("    imperative %s\n", x.c_str());
                break;
            }
        return 1;
    }
    if (a != b) {
        std::printf("TRACES DIFFER\n");
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
            if (a[i] != b[i]) {
                std::printf("  first difference at sweep %zu:\n", i);
                for (const auto& x : a[i]) std::printf("    reactive   %s\n", x.c_str());
                for (const auto& x : b[i]) std::printf("    imperative %s\n", x.c_str());
                break;
            }
        return 1;
    }
    std::size_t ops = 0;
    for (const auto& v : a) ops += v.size();
    std::printf("traces identical: %zu board operations over %zu sweeps\n", ops, work.size());
    if (verify_only) return 0;

    // 3. Two decomposed workloads: the sweep where nothing crossed a
    //    threshold (what a controller sees almost always), and the sweep where
    //    the temperature moved and every output has to settle.
    std::vector<Sample> quiet, hot;
    {
        Sample s{40.0f, true, {1200, 1200, 1200, 1200, 1200}, Mode::automatic, 0, true, true};
        for (int i = 0; i < 512; ++i) {
            s.rpm[0] = 1200 + (i % 8);  // jitter inside the deadband
            quiet.push_back(s);
        }
        for (int i = 0; i < 512; ++i) {
            s.temp = 40.0f + static_cast<float>(i % 2) * 3.0f;  // crosses it every time
            hot.push_back(s);
        }
    }
    std::printf("\nquiet sweep — nothing crossed a deadband:\n");
    std::printf("  dynamic     %8.1f ns\n", time_ns<Reactive>(quiet, 400));
    std::printf("  static      %8.1f ns\n", time_ns<Static>(quiet, 400));
    std::printf("  hand-written%8.1f ns\n", time_ns<Imperative>(quiet, 400));
    std::printf("hot sweep — temperature moved, 5 channels + LED + 8 topics settle:\n");
    std::printf("  dynamic     %8.1f ns\n", time_ns<Reactive>(hot, 400));
    std::printf("  static      %8.1f ns\n", time_ns<Static>(hot, 400));
    std::printf("  hand-written%8.1f ns\n", time_ns<Imperative>(hot, 400));

    // 4. Wall time per sweep on the mixed workload.
    const double r = time_ns<Reactive>(work, 200);
    const double stat = time_ns<Static>(work, 200);
    const double i = time_ns<Imperative>(work, 200);
    std::printf("\nper sweep (7 inputs written, outputs settled):\n");
    std::printf("  dynamic      %8.1f ns  (%.2fx hand-written)\n", r, r / i);
    std::printf("  static       %8.1f ns  (%.2fx hand-written)\n", stat, stat / i);
    std::printf("  hand-written %8.1f ns\n", i);

    const auto st = nsig::get_stats();
    std::printf("\ngraph work: %llu node updates, %llu effect runs, %llu links ever created\n",
                static_cast<unsigned long long>(st.node_updates),
                static_cast<unsigned long long>(st.effect_runs),
                static_cast<unsigned long long>(st.links_created));
}
