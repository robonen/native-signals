// Consumes the library through `import nsig;` instead of #include.
import nsig;

#include <iostream>

int main() {
    nsig::signal count{2};
    nsig::computed squared{[&] { return count() * count(); }};
    auto e = nsig::effect([&] { std::cout << "count=" << count() << " squared=" << squared() << "\n"; });
    count = 5;
    return squared() == 25 ? 0 : 1;
}
