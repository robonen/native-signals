// Port of alien-signals/benchs/propagate.mjs plus a few extra shapes.
// Same graph, same op ("read src, write src + 1"), same measurement protocol
// as bench/propagate.mjs so the numbers line up.
#include "nsig/signals.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <malloc.h>
#include <memory>
#include <string>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

template <class T>
inline void keep(T&& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

/// Bytes currently handed out by malloc — precise, unlike RSS deltas.
double heap_kb() {
    const auto mi = mallinfo2();
    return static_cast<double>(mi.uordblks) / 1024.0;
}

struct result {
    std::string name;
    double ns_per_op;
};

std::vector<result> results;

template <class Setup>
void bench(const std::string& name, int iters, Setup&& setup) {
    auto op = setup();
    for (int i = 0; i < iters / 10 + 1; ++i) op();  // warmup
    const auto t0 = clock_type::now();
    for (int i = 0; i < iters; ++i) op();
    const auto t1 = clock_type::now();
    const double ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(iters);
    results.push_back({name, ns});
    std::printf("%-34s %10.1f ns/op\n", name.c_str(), ns);
}

/// The alien-signals propagate benchmark: `w` independent chains of `h`
/// computeds hanging off one signal, one effect per chain.
void propagate(int w, int h, int iters) {
    bench("propagate " + std::to_string(w) + "x" + std::to_string(h), iters, [w, h] {
        auto src = std::make_shared<nsig::signal<int>>(1);
        auto keepalive = std::make_shared<std::vector<nsig::computed<int>>>();
        auto effects = std::make_shared<std::vector<nsig::effect>>();
        keepalive->reserve(static_cast<std::size_t>(w) * h);

        for (int i = 0; i < w; ++i) {
            nsig::computed<int> last{[src] { return src->get(); }};
            for (int j = 0; j < h; ++j) {
                nsig::computed<int> prev = last;
                last = nsig::computed<int>{[prev] { return prev.get() + 1; }};
                keepalive->push_back(last);
            }
            effects->emplace_back([last] { keep(last.get()); });
        }
        return [src, keepalive, effects] { src->set(src->peek() + 1); };
    });
}

/// One signal, `n` effects — the fan-out shape.
void broadcast(int n, int iters) {
    bench("broadcast 1->" + std::to_string(n), iters, [n] {
        auto src = std::make_shared<nsig::signal<int>>(0);
        auto effects = std::make_shared<std::vector<nsig::effect>>();
        for (int i = 0; i < n; ++i) effects->emplace_back([src] { keep(src->get()); });
        return [src, effects] { src->set(src->peek() + 1); };
    });
}

/// A single deep chain where the tail value never changes, so `check_dirty`
/// must walk the chain and stop the propagation.
void deep_unchanged(int depth, int iters) {
    bench("deep-unchanged d=" + std::to_string(depth), iters, [depth] {
        auto src = std::make_shared<nsig::signal<int>>(0);
        auto keepalive = std::make_shared<std::vector<nsig::computed<int>>>();
        nsig::computed<int> last{[src] { return src->get() & 0; }};
        keepalive->push_back(last);
        for (int i = 0; i < depth; ++i) {
            nsig::computed<int> prev = last;
            last = nsig::computed<int>{[prev] { return prev.get(); }};
            keepalive->push_back(last);
        }
        auto e = std::make_shared<nsig::effect>([last] { keep(last.get()); });
        return [src, keepalive, e] { src->set(src->peek() + 1); };
    });
}

/// Pure read throughput of a memoised computed that is never invalidated.
void cached_read(int iters) {
    bench("cached computed read", iters, [] {
        auto a = std::make_shared<nsig::signal<int>>(1);
        auto c = std::make_shared<nsig::computed<int>>([a] { return a->get() * 2; });
        keep(c->get());
        return [c] { keep(c->get()); };
    });
}

/// Graph construction + teardown: 1 signal, 1 computed, 1 effect per op.
void create_destroy(int iters) {
    bench("create+destroy triple", iters, [] {
        return [] {
            nsig::signal<int> a{1};
            nsig::computed<int> c{[a] { return a.get() * 2; }};
            nsig::effect e{[c] { keep(c.get()); }};
        };
    });
}

/// Dependencies that change shape on every run.
void unstable_deps(int iters) {
    bench("unstable deps (16 sources)", iters, [] {
        auto sel = std::make_shared<nsig::signal<int>>(0);
        auto sources = std::make_shared<std::vector<nsig::signal<int>>>();
        for (int i = 0; i < 16; ++i) sources->emplace_back(i);
        auto e = std::make_shared<nsig::effect>(
            [sel, sources] { keep((*sources)[static_cast<std::size_t>(sel->get()) % 16].get()); });
        auto i = std::make_shared<int>(0);
        return [sel, sources, e, i] { sel->set(++*i); };
    });
}

void memory_report() {
    constexpr int n = 10000;
    std::printf("\n-- memory (malloc in use, %d nodes each) --\n", n);

    const double base = heap_kb();
    auto signals = std::make_unique<std::vector<nsig::signal<int>>>();
    signals->reserve(n);
    for (int i = 0; i < n; ++i) signals->emplace_back(0);
    const double after_sig = heap_kb();
    std::printf("signal:   %8.2f KB (%.1f B/node)\n", after_sig - base,
                (after_sig - base) * 1024.0 / n);

    auto computeds = std::make_unique<std::vector<nsig::computed<int>>>();
    computeds->reserve(n);
    for (int i = 0; i < n; ++i) {
        auto s = (*signals)[static_cast<std::size_t>(i)];
        computeds->emplace_back([s] { return s.get() + 1; });
    }
    const double after_c = heap_kb();
    std::printf("computed: %8.2f KB (%.1f B/node)\n", after_c - after_sig,
                (after_c - after_sig) * 1024.0 / n);

    auto effects = std::make_unique<std::vector<nsig::effect>>();
    effects->reserve(n);
    for (int i = 0; i < n; ++i) {
        auto c = (*computeds)[static_cast<std::size_t>(i)];
        effects->emplace_back([c] { keep(c.get()); });
    }
    const double after_e = heap_kb();
    std::printf("effect:   %8.2f KB (%.1f B/node)\n", after_e - after_c,
                (after_e - after_c) * 1024.0 / n);

    std::printf("sizeof(node)=%zu sizeof(link)=%zu\n", sizeof(nsig::detail::node),
                sizeof(nsig::detail::link));
}

}  // namespace

int main(int argc, char** argv) {
    const bool mem_only = argc > 1 && std::string(argv[1]) == "--memory";
    if (!mem_only) {
        std::printf("-- native-signals (C++23, -O2) --\n");
        propagate(1, 1, 2000000);
        propagate(10, 10, 200000);
        propagate(100, 100, 3000);
        propagate(1000, 10, 3000);
        broadcast(1000, 20000);
        deep_unchanged(100, 200000);
        deep_unchanged(1000, 20000);
        cached_read(20000000);
        create_destroy(2000000);
        unstable_deps(2000000);
    }
    memory_report();
    return 0;
}
