// A small end-to-end example: derived state, scoped effects, batching and an
// async resource — the shapes you'd actually reach for in an application.
#include <nsig/nsig.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

struct Item {
    std::string text;
    bool done = false;
    bool operator==(const Item&) const = default;
};

enum class Filter { all, active, done };

class TodoStore {
public:
    // Reactive state, exposed read-only.
    nsig::readonly<std::vector<Item>> items() const { return items_; }
    nsig::readonly<int> remaining() const { return remaining_; }
    nsig::readonly<std::vector<Item>> visible() const { return visible_; }

    void add(std::string text) {
        items_.modify([&](auto& v) { v.push_back({std::move(text), false}); });
    }
    void toggle(std::size_t i) {
        items_.modify([&](auto& v) {
            if (i < v.size()) v[i].done = !v[i].done;
        });
    }
    void set_filter(Filter f) { filter_.set(f); }

    /// Two writes, one effect run.
    void add_many(std::vector<std::string> texts) {
        nsig::batch guard;
        for (auto& t : texts) add(std::move(t));
    }

private:
    nsig::signal<std::vector<Item>> items_{};
    nsig::signal<Filter> filter_{Filter::all};

    nsig::computed<int> remaining_{[this] {
        return static_cast<int>(std::ranges::count_if(items_(), [](const Item& i) { return !i.done; }));
    }};

    nsig::computed<std::vector<Item>> visible_{[this] {
        const auto f = filter_();
        std::vector<Item> out;
        for (const auto& i : items_())
            if (f == Filter::all || (f == Filter::done) == i.done) out.push_back(i);
        return out;
    }};
};

int main() {
    TodoStore store;

    // A scope groups the "UI" effects so they can all be torn down at once.
    nsig::effect_scope ui{[&] {
        nsig::spawn([&] {
            std::cout << "[header] " << store.remaining().get() << " remaining\n";
        });
        nsig::spawn([&] {
            std::cout << "[list]  ";
            for (const auto& i : store.visible().get())
                std::cout << (i.done ? "[x] " : "[ ] ") << i.text << "  ";
            std::cout << "\n";
        });
    }};

    store.add_many({"write core", "port tests", "benchmark"});
    store.toggle(1);
    store.set_filter(Filter::active);

    std::cout << "\n-- async resource --\n";
    nsig::signal<int> page{1};
    std::vector<nsig::async_event<std::string>> inbox;
    inbox.reserve(4);

    auto feed = nsig::make_resource(
        [&] { return page(); },
        [&](int p) -> nsig::task<std::string> {
            inbox.emplace_back();
            auto ev = inbox.back();
            std::string body = co_await ev;  // stands in for a network call
            co_return "page " + std::to_string(p) + ": " + body;
        });

    auto view = nsig::effect([&] {
        if (feed.loading()) std::cout << "[feed] loading...\n";
        else if (feed.value()) std::cout << "[feed] " << *feed.value() << "\n";
    });

    inbox[0].set("hello");
    page = 2;
    inbox[1].set("world");

    ui.stop();
    store.add("this triggers nothing");
    std::cout << "\n(scope stopped; no further output above this line)\n";
}
