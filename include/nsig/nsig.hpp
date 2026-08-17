// native-signals — umbrella header.
//
//   #include <nsig/nsig.hpp>          // everything, including the async layer
//   #include <nsig/signals.hpp>       // core API only, no <coroutine>
//   #include <nsig/core.hpp>          // the raw graph, to build your own surface
//   #include <nsig/static_graph.hpp>  // compile-time wiring, for control loops
//   #include <nsig/expr.hpp>          // operator sugar over either graph
//   #include <nsig/adapt.hpp>         // driver adapters, filters, lifted math
#pragma once

#include "adapt.hpp"
#include "async.hpp"
#include "core.hpp"
#include "expr.hpp"
#include "signals.hpp"
#include "static_graph.hpp"

#define NSIG_VERSION_MAJOR 0
#define NSIG_VERSION_MINOR 1
#define NSIG_VERSION_PATCH 0
#define NSIG_VERSION_STRING "0.1.0"
