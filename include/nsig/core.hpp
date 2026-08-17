// native-signals — core reactive graph.
//
// A C++23 port of the push-pull propagation algorithm from
// https://github.com/stackblitz/alien-signals (MIT), with C++-native
// ownership (intrusive refcounting) and a pooled link arena.
//
// The core keeps alien-signals' hard constraints: no Array/Set/Map in the hot
// path, and no recursion in propagate/check_dirty (explicit stacks instead).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <utility>
#include <vector>
#include <version>

#ifndef NSIG_THREAD_LOCAL
#  define NSIG_THREAD_LOCAL thread_local
#endif

#ifndef NSIG_ASSERT
#  include <cassert>
#  define NSIG_ASSERT(x) assert(x)
#endif

// ESP-IDF (and most embedded toolchains) build with -fno-exceptions. The graph
// never needs exceptions itself; it only guards user callbacks that may throw.
#ifndef NSIG_HAS_EXCEPTIONS
#  if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#    define NSIG_HAS_EXCEPTIONS 1
#  else
#    define NSIG_HAS_EXCEPTIONS 0
#  endif
#endif

#if NSIG_HAS_EXCEPTIONS
#  define NSIG_TRY try
#  define NSIG_CATCH_ALL catch (...)
#else
#  define NSIG_TRY if (true)
#  define NSIG_CATCH_ALL if (false)
#endif

#if defined(__cpp_explicit_this_parameter) || defined(__cpp_deducing_this)
#  define NSIG_DEDUCING_THIS 1
#else
#  define NSIG_DEDUCING_THIS 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define NSIG_ALWAYS_INLINE [[gnu::always_inline]] inline
#  define NSIG_NOINLINE [[gnu::noinline]]
#  define NSIG_LIKELY(x)   (__builtin_expect(!!(x), 1))
#  define NSIG_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#  define NSIG_ALWAYS_INLINE inline
#  define NSIG_NOINLINE
#  define NSIG_LIKELY(x)   (x)
#  define NSIG_UNLIKELY(x) (x)
#endif

namespace nsig::detail {

// ---------------------------------------------------------------------------
// Flags & node kinds
// ---------------------------------------------------------------------------

using flag_t = std::uint8_t;

inline constexpr flag_t f_none           = 0;
inline constexpr flag_t f_mutable        = 1 << 0;  // participates in value propagation
inline constexpr flag_t f_watching       = 1 << 1;  // wants notify() (effects)
inline constexpr flag_t f_recursed_check = 1 << 2;  // currently tracking
inline constexpr flag_t f_recursed       = 1 << 3;
inline constexpr flag_t f_dirty          = 1 << 4;
inline constexpr flag_t f_pending        = 1 << 5;
inline constexpr flag_t f_has_child      = 1 << 6;  // owns child effects/scopes

enum class node_kind : std::uint8_t { signal, computed, effect, scope };

struct link;
struct node;

// Type-erased per-node operations. Signals and computeds use `update`;
// effects use `run`; every node uses `destroy`.
struct node_vtable {
    bool (*update)(node*);
    void (*run)(node*);
    void (*destroy)(node*) noexcept;
};

struct node {
    const node_vtable* vt;
    link* deps      = nullptr;
    link* deps_tail = nullptr;
    link* subs      = nullptr;
    link* subs_tail = nullptr;
    std::uint32_t refs = 1;
    flag_t flags = f_none;
    node_kind kind;

    constexpr node(const node_vtable* v, node_kind k, flag_t f) noexcept
        : vt(v), flags(f), kind(k) {}

    // Every reactive node — signal, computed, effect, scope — is allocated
    // from the per-thread block pool. Sized deallocation is always available
    // because nodes are only ever deleted through their concrete type.
    static void* operator new(std::size_t n);
    static void operator delete(void* p, std::size_t n) noexcept;
};

struct link {
    node* dep;
    node* sub;
    link* prev_sub;
    link* next_sub;
    link* prev_dep;
    link* next_dep;
    std::uint32_t version;
};

/// Effects carry a type-erased cleanup returned by the previous run.
struct effect_node : node {
    std::move_only_function<void()> cleanup{};
    using node::node;
};

// ---------------------------------------------------------------------------
// Link pool: chunked bump allocator + free list. `link` is trivial, so raw
// storage is recycled without construction or destruction.
// ---------------------------------------------------------------------------

class link_pool {
    static constexpr std::size_t chunk_links = 512;

    std::vector<link*> chunks_{};
    link* free_ = nullptr;
    link* bump_ = nullptr;
    std::size_t bump_left_ = 0;
    bool alive_ = true;

public:
    constexpr link_pool() noexcept = default;
    link_pool(const link_pool&) = delete;
    link_pool& operator=(const link_pool&) = delete;

    ~link_pool() {
        alive_ = false;
        for (link* c : chunks_) ::operator delete(static_cast<void*>(c));
        chunks_.clear();
    }

    NSIG_ALWAYS_INLINE link* acquire() {
        if (NSIG_UNLIKELY(!alive_)) return static_cast<link*>(::operator new(sizeof(link)));
        if (NSIG_LIKELY(free_ != nullptr)) {
            link* l = free_;
            free_ = l->next_dep;
            return l;
        }
        if (NSIG_LIKELY(bump_left_ != 0)) {
            --bump_left_;
            return bump_++;
        }
        return grow();
    }

    NSIG_ALWAYS_INLINE void release(link* l) noexcept {
        // Globals (the norm on a microcontroller) can outlive the pool at
        // shutdown; after the chunks are gone the storage is already reclaimed.
        if (NSIG_UNLIKELY(!alive_)) return;
        l->next_dep = free_;
        free_ = l;
    }

    [[nodiscard]] std::size_t reserved_links() const noexcept { return reserved_; }

    /// Carves one chunk large enough for `count` further links.
    void reserve(std::size_t count) {
        if (bump_left_ >= count) return;
        add_chunk(count);
    }

private:
    std::size_t reserved_ = 0;

    void add_chunk(std::size_t count) {
        auto* c = static_cast<link*>(::operator new(sizeof(link) * count));
        chunks_.push_back(c);
        bump_ = c;
        bump_left_ = count;
        reserved_ += count;
    }

    NSIG_NOINLINE link* grow() {
        add_chunk(chunk_links);
        --bump_left_;
        return bump_++;
    }
};

// ---------------------------------------------------------------------------
// Node pool: segregated free lists over 64 KiB chunks. Reactive nodes are
// small, short-lived and allocated in bursts, which is exactly the pattern a
// general-purpose malloc handles worst.
// ---------------------------------------------------------------------------

class block_pool {
    static constexpr std::size_t granularity = 16;
    static constexpr std::size_t max_class_size = 256;
    static constexpr std::size_t class_count = max_class_size / granularity;
    static constexpr std::size_t chunk_bytes = 64 * 1024;

    void* free_[class_count]{};
    std::vector<void*> chunks_{};
    std::byte* bump_ = nullptr;
    std::size_t bump_left_ = 0;
    bool alive_ = true;

    static constexpr std::size_t class_of(std::size_t n) noexcept {
        return (n + granularity - 1) / granularity - 1;
    }

public:
    constexpr block_pool() noexcept = default;
    block_pool(const block_pool&) = delete;
    block_pool& operator=(const block_pool&) = delete;

    ~block_pool() {
        alive_ = false;
        for (void* c : chunks_) ::operator delete(c);
        chunks_.clear();
    }

    NSIG_ALWAYS_INLINE void* allocate(std::size_t n) {
        if (NSIG_UNLIKELY(n > max_class_size || !alive_)) return ::operator new(n);
        const std::size_t c = class_of(n);
        if (void* p = free_[c]; NSIG_LIKELY(p != nullptr)) {
            free_[c] = *static_cast<void**>(p);
            return p;
        }
        return carve(c);
    }

    NSIG_ALWAYS_INLINE void deallocate(void* p, std::size_t n) noexcept {
        if (NSIG_UNLIKELY(n > max_class_size)) {
            ::operator delete(p);
            return;
        }
        if (NSIG_UNLIKELY(!alive_)) return;  // chunks already released
        const std::size_t c = class_of(n);
        *static_cast<void**>(p) = free_[c];
        free_[c] = p;
    }

private:
    /// Carves one chunk of at least `bytes`, so later allocations are pure
    /// pointer bumps.
public:
    void reserve(std::size_t bytes) {
        if (bump_left_ >= bytes) return;
        auto* chunk = static_cast<std::byte*>(::operator new(bytes));
        chunks_.push_back(chunk);
        bump_ = chunk;
        bump_left_ = bytes;
    }

private:
    NSIG_NOINLINE void* carve(std::size_t c) {
        const std::size_t size = (c + 1) * granularity;
        if (bump_left_ < size) {
            auto* chunk = static_cast<std::byte*>(::operator new(chunk_bytes));
            chunks_.push_back(chunk);
            bump_ = chunk;
            bump_left_ = chunk_bytes;
        }
        void* p = bump_;
        bump_ += size;
        bump_left_ -= size;
        return p;
    }
};

// ---------------------------------------------------------------------------
// Traversal stack: inline storage with heap spill. propagate/check_dirty stay
// iterative, so this replaces alien-signals' allocated `Stack<T>` cells.
// ---------------------------------------------------------------------------

template <class T, std::size_t N = 32>
class small_stack {
    T inline_[N];
    std::size_t size_ = 0;
    std::vector<T> spill_{};

public:
    NSIG_ALWAYS_INLINE void push(T v) {
        if (NSIG_LIKELY(size_ < N)) inline_[size_++] = v;
        else spill_.push_back(v);
    }
    NSIG_ALWAYS_INLINE T pop() noexcept {
        if (NSIG_UNLIKELY(!spill_.empty())) {
            T v = spill_.back();
            spill_.pop_back();
            return v;
        }
        return inline_[--size_];
    }
    [[nodiscard]] NSIG_ALWAYS_INLINE bool empty() const noexcept {
        return size_ == 0 && spill_.empty();
    }
};

// ---------------------------------------------------------------------------
// Per-thread runtime state
// ---------------------------------------------------------------------------

using scheduler_fn = void (*)();

struct runtime {
    node* active_sub = nullptr;
    std::uint32_t cycle = 0;
    std::uint32_t run_depth = 0;
    std::uint32_t batch_depth = 0;
    std::size_t notify_index = 0;
    std::size_t queued_len = 0;
    std::vector<node*> queued{};
    std::vector<std::move_only_function<void()>> post_flush{};
    link_pool pool{};
    block_pool nodes{};
    scheduler_fn scheduler = nullptr;
    bool flush_scheduled = false;
    // diagnostics
    std::uint64_t stat_links_created = 0;
    std::uint64_t stat_node_updates = 0;
    std::uint64_t stat_effect_runs = 0;

    constexpr runtime() noexcept = default;
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
};

// Constant-initialised so thread-local access needs no guard variable.
inline constinit NSIG_THREAD_LOCAL runtime g_rt;

NSIG_ALWAYS_INLINE runtime& rt() noexcept { return g_rt; }

NSIG_ALWAYS_INLINE void* node::operator new(std::size_t n) { return g_rt.nodes.allocate(n); }
NSIG_ALWAYS_INLINE void node::operator delete(void* p, std::size_t n) noexcept {
    g_rt.nodes.deallocate(p, n);
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

void release(node* n) noexcept;
void unwatched(node* dep) noexcept;
void notify(node* sub);
void shallow_propagate(link* l);
void dispose_effect(node* n);
void dispose_scope(node* n) noexcept;
void run_effect(node* n);

// ---------------------------------------------------------------------------
// Refcounting. Edges point sub -> dep and a link owns only its `dep`, so the
// strong-reference graph is a DAG: no cycles, no tracing collector needed.
// ---------------------------------------------------------------------------

NSIG_ALWAYS_INLINE void retain(node* n) noexcept { ++n->refs; }

// ---------------------------------------------------------------------------
// Graph primitives
// ---------------------------------------------------------------------------

/// Records that `sub` read `dep` during the tracking pass identified by
/// `version`. Reuses the existing link when dependency order is stable, which
/// is the overwhelmingly common case and allocates nothing.
inline void link_dep(node* dep, node* sub, std::uint32_t version) {
    link* prev_dep = sub->deps_tail;
    if (prev_dep != nullptr && prev_dep->dep == dep) return;

    link* next_dep = prev_dep != nullptr ? prev_dep->next_dep : sub->deps;
    if (next_dep != nullptr && next_dep->dep == dep) {
        next_dep->version = version;
        sub->deps_tail = next_dep;
        return;
    }

    link* prev_sub = dep->subs_tail;
    if (prev_sub != nullptr && prev_sub->version == version && prev_sub->sub == sub) return;

    auto& r = rt();
    link* nl = r.pool.acquire();
    ++r.stat_links_created;
    nl->version  = version;
    nl->dep      = dep;
    nl->sub      = sub;
    nl->prev_dep = prev_dep;
    nl->next_dep = next_dep;
    nl->prev_sub = prev_sub;
    nl->next_sub = nullptr;

    sub->deps_tail = nl;
    dep->subs_tail = nl;
    retain(dep);  // the edge owns its dependency

    if (next_dep != nullptr) next_dep->prev_dep = nl;
    if (prev_dep != nullptr) prev_dep->next_dep = nl;
    else sub->deps = nl;
    if (prev_sub != nullptr) prev_sub->next_sub = nl;
    else dep->subs = nl;
}

/// Removes one edge, returning the next dependency of `sub`.
inline link* unlink_dep(link* l, node* sub) noexcept {
    node* dep      = l->dep;
    link* prev_dep = l->prev_dep;
    link* next_dep = l->next_dep;
    link* next_sub = l->next_sub;
    link* prev_sub = l->prev_sub;

    if (next_dep != nullptr) next_dep->prev_dep = prev_dep;
    else sub->deps_tail = prev_dep;
    if (prev_dep != nullptr) prev_dep->next_dep = next_dep;
    else sub->deps = next_dep;

    if (next_sub != nullptr) next_sub->prev_sub = prev_sub;
    else dep->subs_tail = prev_sub;

    rt().pool.release(l);

    if (prev_sub != nullptr) {
        prev_sub->next_sub = next_sub;
    } else if ((dep->subs = next_sub) == nullptr) {
        unwatched(dep);
    }
    release(dep);
    return next_dep;
}

NSIG_ALWAYS_INLINE link* unlink_dep(link* l) noexcept { return unlink_dep(l, l->sub); }

inline bool is_valid_link(const link* check, const node* sub) noexcept {
    for (const link* l = sub->deps_tail; l != nullptr; l = l->prev_dep)
        if (l == check) return true;
    return false;
}

NSIG_ALWAYS_INLINE bool update_node(node* n) {
    ++rt().stat_node_updates;
    return n->vt->update(n);
}

/// Marks the transitive subscriber set of a changed dependency.
/// Iterative depth-first walk; `stack` holds deferred sibling branches.
inline void propagate(link* l, bool inner_write) {
    link* next = l->next_sub;
    small_stack<link*> stack;

    for (;;) {
        node* sub = l->sub;
        flag_t flags = sub->flags;

        if (!(flags & (f_recursed_check | f_recursed | f_dirty | f_pending))) {
            sub->flags = static_cast<flag_t>(
                flags | f_pending | (inner_write ? f_recursed : f_none));
        } else if (!(flags & (f_recursed_check | f_recursed))) {
            flags = f_none;
        } else if (!(flags & f_recursed_check)) {
            sub->flags = static_cast<flag_t>((flags & ~f_recursed) | f_pending);
        } else if (!(flags & (f_dirty | f_pending)) && is_valid_link(l, sub)) {
            sub->flags = static_cast<flag_t>(flags | f_recursed | f_pending);
            flags = static_cast<flag_t>(flags & f_mutable);
        } else {
            flags = f_none;
        }

        if (flags & f_watching) notify(sub);

        if (flags & f_mutable) {
            if (link* sub_subs = sub->subs; sub_subs != nullptr) {
                l = sub_subs;
                if (link* ns = l->next_sub; ns != nullptr) {
                    stack.push(next);
                    next = ns;
                }
                continue;
            }
        }

        if ((l = next) != nullptr) {
            next = l->next_sub;
            continue;
        }

        bool resumed = false;
        while (!stack.empty()) {
            l = stack.pop();
            if (l != nullptr) {
                next = l->next_sub;
                resumed = true;
                break;
            }
        }
        if (resumed) continue;
        return;
    }
}

/// Resolves whether a pending subscriber is genuinely dirty by pulling its
/// dependency chain. Iterative; `stack` records the descent path.
inline bool check_dirty(link* l, node* sub) {
    small_stack<link*> stack;
    int depth = 0;
    bool dirty = false;

    for (;;) {
        node* dep = l->dep;
        const flag_t dep_flags = dep->flags;

        if (sub->flags & f_dirty) {
            dirty = true;
        } else if ((dep_flags & (f_mutable | f_dirty)) == (f_mutable | f_dirty)) {
            link* subs = dep->subs;
            if (update_node(dep)) {
                if (subs->next_sub != nullptr) shallow_propagate(subs);
                dirty = true;
            }
        } else if ((dep_flags & (f_mutable | f_pending)) == (f_mutable | f_pending)) {
            stack.push(l);
            l = dep->deps;
            sub = dep;
            ++depth;
            continue;
        }

        if (!dirty) {
            if (link* nd = l->next_dep; nd != nullptr) {
                l = nd;
                continue;
            }
        }

        bool restarted = false;
        while (depth--) {
            l = stack.pop();
            if (dirty) {
                link* subs = sub->subs;
                if (update_node(sub)) {
                    if (subs->next_sub != nullptr) shallow_propagate(subs);
                    sub = l->sub;
                    continue;
                }
                dirty = false;
            } else {
                sub->flags = static_cast<flag_t>(sub->flags & ~f_pending);
            }
            sub = l->sub;
            if (link* nd = l->next_dep; nd != nullptr) {
                l = nd;
                restarted = true;
                break;
            }
        }
        if (restarted) continue;
        return dirty && sub->flags != f_none;
    }
}

inline void shallow_propagate(link* l) {
    do {
        node* sub = l->sub;
        const flag_t flags = sub->flags;
        if ((flags & (f_pending | f_dirty)) == f_pending) {
            sub->flags = static_cast<flag_t>(flags | f_dirty);
            if ((flags & (f_watching | f_recursed_check)) == f_watching) notify(sub);
        }
    } while ((l = l->next_sub) != nullptr);
}

/// Drops dependencies that were not re-read during the latest tracking pass.
inline void purge_deps(node* sub) noexcept {
    link* tail = sub->deps_tail;
    link* d = tail != nullptr ? tail->next_dep : sub->deps;
    while (d != nullptr) d = unlink_dep(d, sub);
}

inline void dispose_all_deps_reverse(node* sub) noexcept {
    link* l = sub->deps_tail;
    while (l != nullptr) {
        link* prev = l->prev_dep;
        unlink_dep(l, sub);
        l = prev;
    }
}

/// Detaches child effects/scopes before a parent re-runs, so a re-run rebuilds
/// its children instead of accumulating them.
inline void dispose_child_effects(node* parent) noexcept {
    link* l = parent->deps_tail;
    while (l != nullptr) {
        link* prev = l->prev_dep;
        const node_kind k = l->dep->kind;
        if (k == node_kind::effect || k == node_kind::scope) unlink_dep(l, parent);
        l = prev;
    }
}

// ---------------------------------------------------------------------------
// Effect queue & flush
// ---------------------------------------------------------------------------

/// Queues an effect together with its watching ancestors, outer-first, so a
/// parent always runs before the children it may destroy.
inline void notify(node* e) {
    auto& r = rt();
    std::size_t insert = r.queued_len;
    std::size_t first = insert;

    for (;;) {
        if (insert >= r.queued.size()) r.queued.resize(insert < 8 ? 8 : insert * 2, nullptr);
        r.queued[insert++] = e;
        e->flags = static_cast<flag_t>(e->flags & ~f_watching);
        link* s = e->subs;
        e = (s != nullptr) ? s->sub : nullptr;
        if (e == nullptr || !(e->flags & f_watching)) break;
    }

    r.queued_len = insert;
    while (first < --insert) {
        std::swap(r.queued[first], r.queued[insert]);
        ++first;
    }
}

inline void run_cleanup(effect_node* e) {
    auto cleanup = std::move(e->cleanup);
    e->cleanup = nullptr;
    auto& r = rt();
    node* prev = std::exchange(r.active_sub, nullptr);
    struct restore {
        runtime& r;
        node* prev;
        ~restore() { r.active_sub = prev; }
    } guard{r, prev};
    cleanup();
}

inline void run_effect(node* n) {
    auto* e = static_cast<effect_node*>(n);
    const flag_t flags = n->flags;

    if ((flags & f_dirty) || ((flags & f_pending) && check_dirty(n->deps, n))) {
        if (flags & f_has_child) dispose_child_effects(n);
        if (e->cleanup) {
            run_cleanup(e);
            if (n->flags == f_none) return;  // the cleanup disposed this effect
        }
        n->deps_tail = nullptr;
        n->flags = static_cast<flag_t>(f_watching | f_recursed_check);

        auto& r = rt();
        node* prev = std::exchange(r.active_sub, n);
        ++r.cycle;
        ++r.run_depth;
        ++r.stat_effect_runs;
        struct restore {
            runtime& r;
            node* prev;
            node* n;
            ~restore() {
                --r.run_depth;
                r.active_sub = prev;
                n->flags = static_cast<flag_t>(n->flags & ~f_recursed_check);
                purge_deps(n);
            }
        } guard{r, prev, n};
        n->vt->run(n);
    } else if (n->deps != nullptr) {
        n->flags = static_cast<flag_t>(f_watching | (flags & f_has_child));
    }
}

inline void drain_post_flush() {
    auto& r = rt();
    while (!r.post_flush.empty()) {
        auto batch = std::move(r.post_flush);
        r.post_flush.clear();
        for (auto& fn : batch) fn();
    }
}

inline void flush() {
    auto& r = rt();
    r.flush_scheduled = false;
    struct reset_guard {
        runtime& r;
        ~reset_guard() {
            // On the exceptional path, re-arm whatever is still queued so the
            // graph stays consistent instead of silently dropping updates.
            while (r.notify_index < r.queued_len) {
                node* e = r.queued[r.notify_index];
                r.queued[r.notify_index++] = nullptr;
                if (e != nullptr)
                    e->flags = static_cast<flag_t>(e->flags | f_watching | f_recursed);
            }
            r.notify_index = 0;
            r.queued_len = 0;
        }
    } guard{r};

    while (r.notify_index < r.queued_len) {
        node* e = r.queued[r.notify_index];
        r.queued[r.notify_index++] = nullptr;
        run_effect(e);
    }
}

inline void flush_now() {
    flush();
    drain_post_flush();
}

/// Pre-sizes the pools and the effect queue. On a microcontroller call this
/// once at boot with the graph's known upper bounds; afterwards the steady
/// state performs no heap allocation at all.
inline void reserve(std::size_t nodes_bytes, std::size_t links, std::size_t queued_effects) {
    auto& r = rt();
    r.pool.reserve(links);
    r.nodes.reserve(nodes_bytes);
    if (r.queued.size() < queued_effects) r.queued.resize(queued_effects, nullptr);
}

inline void request_flush() {
    auto& r = rt();
    if (r.batch_depth != 0) return;
    if (r.scheduler != nullptr) {
        if (!r.flush_scheduled) {
            r.flush_scheduled = true;
            r.scheduler();
        }
        return;
    }
    flush_now();
}

// ---------------------------------------------------------------------------
// Disposal & teardown
// ---------------------------------------------------------------------------

inline void dispose_scope(node* n) noexcept {
    n->flags = f_none;
    dispose_all_deps_reverse(n);
    if (link* s = n->subs; s != nullptr) unlink_dep(s);
}

inline void dispose_effect(node* n) {
    auto* e = static_cast<effect_node*>(n);
    dispose_scope(n);
    if (e->cleanup) run_cleanup(e);
}

inline void unwatched(node* dep) noexcept {
    switch (dep->kind) {
    case node_kind::computed:
        // Keep the cached value but drop the graph; recompute lazily if read
        // again. Matches alien-signals' unwatched() behaviour.
        if (dep->deps_tail != nullptr) {
            dep->flags = static_cast<flag_t>(f_mutable | f_dirty);
            dispose_all_deps_reverse(dep);
        }
        break;
    case node_kind::signal:
        break;
    case node_kind::effect:
        NSIG_TRY {
            dispose_effect(dep);
        }
        NSIG_CATCH_ALL {}
        break;
    case node_kind::scope:
        dispose_scope(dep);
        break;
    }
}

inline void release(node* n) noexcept {
    if (--n->refs == 0) {
        // Re-entrancy guard: teardown can transiently retain/release `n`.
        n->refs = 1;
        if (n->kind == node_kind::effect) {
            NSIG_TRY {
                dispose_effect(n);
            }
            NSIG_CATCH_ALL {}
        } else {
            n->flags = f_none;
            dispose_all_deps_reverse(n);
        }
        n->vt->destroy(n);
    }
}

}  // namespace nsig::detail
