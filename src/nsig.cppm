// native-signals as a C++20 module: `import nsig;`
//
// The implementation stays in headers so the same code serves both the
// `#include` and the `import` route; this file only re-exports the public
// names. Built by CMake when -DNSIG_BUILD_MODULE=ON.
module;

#include "nsig/nsig.hpp"

export module nsig;

export namespace nsig {

// Shared: equality policies, expressions, lifted maths, filter functors.

// Equality policies
using ::nsig::auto_equal;
using ::nsig::never_equal;

// Concepts
using ::nsig::getter_of;
using ::nsig::incremental_getter_of;

// Core API
using ::nsig::batch;
using ::nsig::batched;
using ::nsig::computed;
using ::nsig::effect;
using ::nsig::effect_scope;
using ::nsig::flush;
using ::nsig::on_cleanup;
using ::nsig::readonly;
using ::nsig::set_scheduler;
using ::nsig::signal;
using ::nsig::spawn;
using ::nsig::trigger;
using ::nsig::untracked;
using ::nsig::watch;

// Lazy expressions
using ::nsig::expr;
using ::nsig::lift;
using ::nsig::make_array;
using ::nsig::pick;
using ::nsig::readable;

// Adapters
using ::nsig::abs;
using ::nsig::clamp;
using ::nsig::debounce;
using ::nsig::ema;
using ::nsig::map_range;
using ::nsig::max;
using ::nsig::median3;
using ::nsig::min;
using ::nsig::peak;
using ::nsig::schmitt;
using ::nsig::slew;
using ::nsig::trough;

// Diagnostics
using ::nsig::get_stats;
using ::nsig::reset_stats;
using ::nsig::stats;

// Async layer
using ::nsig::async_event;
using ::nsig::changed;
using ::nsig::launch;
using ::nsig::make_resource;
using ::nsig::resource;
using ::nsig::task;
using ::nsig::until;

}  // namespace nsig

export namespace nsig::fixed {

using ::nsig::fixed::signal;
using ::nsig::fixed::epoch;
using ::nsig::fixed::computed;
using ::nsig::fixed::computed_node;
using ::nsig::fixed::watch;
using ::nsig::fixed::watch_node;
using ::nsig::fixed::fold;
using ::nsig::fixed::fold_node;
using ::nsig::fixed::version_t;
using ::nsig::fixed::expr;
using ::nsig::fixed::lift;
using ::nsig::fixed::make_array;
using ::nsig::fixed::pick;

using ::nsig::fixed::actuator;
using ::nsig::fixed::drive;
using ::nsig::fixed::on_falling;
using ::nsig::fixed::on_rising;
using ::nsig::fixed::sample;
using ::nsig::fixed::sensor;

}  // namespace nsig::fixed

export namespace nsig::dynamic {

using ::nsig::dynamic::batch;
using ::nsig::dynamic::computed;
using ::nsig::dynamic::effect;
using ::nsig::dynamic::effect_scope;
using ::nsig::dynamic::signal;
using ::nsig::dynamic::watch;

}  // namespace nsig::dynamic
