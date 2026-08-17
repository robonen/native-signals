// Verifies the amalgamated header is self-contained and behaves identically.
#include <nsig.hpp>

#include <iostream>
#include <string>
#include <vector>

int main() {
    int failures = 0;
    auto want = [&](bool ok, const char* what) {
        if (!ok) {
            ++failures;
            std::cout << "  FAIL " << what << "\n";
        }
    };

    nsig::signal count{1};
    nsig::computed doubled{[&] { return count() * 2; }};
    std::vector<int> log;
    auto e = nsig::effect([&] { log.push_back(count()); });

    count = 2;
    {
        nsig::batch b;
        count = 3;
        count = 4;
    }
    want(doubled() == 8, "computed");
    want(log.size() == 3, "effect ran 3 times");
    want(log.back() == 4, "last value");

    // async layer came along for the ride
    nsig::signal<int> n{0};
    bool done = false;
    auto body = [&]() -> nsig::task<void> {
        co_await nsig::until([&] { return n() >= 2; });
        done = true;
    };
    nsig::launch(body());
    n = 2;
    want(done, "coroutine layer");

    std::cout << (failures == 0 ? "single-header build OK\n" : "single-header FAILED\n");
    return failures == 0 ? 0 : 1;
}
