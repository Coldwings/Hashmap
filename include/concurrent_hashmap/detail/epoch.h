#pragma once

#include <atomic>
#include <cassert>
#include <mutex>

namespace concurrent_hashmap {
namespace detail {

// ---------------------------------------------------------------------------
// EpochManager -- epoch-based memory reclamation (3-generation scheme)
//
// Objects retired in epoch N are safe to free once global_epoch reaches N+2,
// because by then every thread has moved past epoch N.
//
// Thread registration is transparent: a thread_local ThreadEntry is lazily
// created on first use and automatically deregistered when the OS thread
// exits (via thread_local destructor).
// ---------------------------------------------------------------------------
class EpochManager {
public:
    // ------------------------------------------------------------------
    // Retired -- base class for any object managed by epoch reclamation.
    // Clients derive from this and override the virtual destructor to
    // release their payload.
    // ------------------------------------------------------------------
    struct Retired {
        virtual ~Retired() = default;
        Retired* next = nullptr;
    };

private:
    // Forward declaration so ThreadEntry can be used in ThreadHandle.
    struct ThreadEntry;

    // ------------------------------------------------------------------
    // RetireList -- thread-safe singly-linked list of Retired objects.
    // Implemented as a lock-free Treiber stack.
    // ------------------------------------------------------------------
    struct RetireList {
        std::atomic<Retired*> head{nullptr};

        void push(Retired* node) {
            Retired* old_head = head.load(std::memory_order_relaxed);
            do {
                node->next = old_head;
            } while (!head.compare_exchange_weak(
                old_head, node,
                std::memory_order_release,
                std::memory_order_relaxed));
        }

        // Atomically detach the entire list and delete every node.
        void drain() {
            Retired* list = head.exchange(nullptr, std::memory_order_acquire);
            while (list) {
                Retired* nxt = list->next;
                delete list;
                list = nxt;
            }
        }
    };

    // ------------------------------------------------------------------
    // ThreadEntry -- per-OS-thread metadata.
    // Linked into an intrusive lock-free list owned by the EpochManager.
    // ------------------------------------------------------------------
    struct ThreadEntry {
        std::atomic<uint64_t>     local_epoch{0};
        std::atomic<bool>         active{false};
        int                       nesting{0};      // thread-local only
        unsigned                  ops_since_advance{0}; // thread-local only
        std::atomic<bool>         alive{true};
        std::atomic<ThreadEntry*> next{nullptr};
        EpochManager*             owner{nullptr};
    };

    // ------------------------------------------------------------------
    // ThreadHandle -- thread_local destructor marks entry as dead.
    // A single handle per thread tracks the most-recently-used entry.
    // ------------------------------------------------------------------
    struct ThreadHandle {
        ThreadEntry* entry{nullptr};

        ~ThreadHandle() {
            if (entry) {
                entry->active.store(false, std::memory_order_release);
                entry->alive.store(false, std::memory_order_release);
                entry = nullptr;
            }
        }
    };

    // ------------------------------------------------------------------
    // Manager state
    // ------------------------------------------------------------------
    static const unsigned     kAdvanceInterval = 64;
    std::atomic<uint64_t>     global_epoch_{0};
    std::atomic<ThreadEntry*> thread_list_{nullptr};
    RetireList                retire_lists_[3];
    std::mutex                advance_mutex_;

public:
    EpochManager() = default;

    ~EpochManager() {
        // Drain all three retire lists.
        for (int i = 0; i < 3; ++i) {
            retire_lists_[i].drain();
        }
        // Free every ThreadEntry in the intrusive list.
        ThreadEntry* e = thread_list_.load(std::memory_order_relaxed);
        while (e) {
            ThreadEntry* nxt = e->next.load(std::memory_order_relaxed);
            delete e;
            e = nxt;
        }
    }

    // Non-copyable, non-movable.
    EpochManager(const EpochManager&) = delete;
    EpochManager& operator=(const EpochManager&) = delete;

    // ------------------------------------------------------------------
    // retire -- place a Retired object on the current epoch's list.
    // ------------------------------------------------------------------
    void retire(Retired* obj) {
        uint64_t epoch = global_epoch_.load(std::memory_order_relaxed);
        retire_lists_[epoch % 3].push(obj);
        try_advance();
    }

    // ------------------------------------------------------------------
    // pin / unpin -- called by EpochGuard.
    // ------------------------------------------------------------------
    void pin(ThreadEntry* entry) {
        ++entry->nesting;
        if (entry->nesting == 1) {
            entry->active.store(true, std::memory_order_relaxed);
            entry->local_epoch.store(
                global_epoch_.load(std::memory_order_acquire),
                std::memory_order_release);
        }
    }

    void unpin(ThreadEntry* entry) {
        assert(entry->nesting > 0);
        --entry->nesting;
        if (entry->nesting == 0) {
            entry->active.store(false, std::memory_order_release);
            // Amortise try_advance: only attempt every kAdvanceInterval
            // unpins to reduce mutex contention and thread-list scans.
            if (++entry->ops_since_advance >= kAdvanceInterval) {
                entry->ops_since_advance = 0;
                try_advance();
            }
        }
    }

    // ------------------------------------------------------------------
    // get_thread_entry -- obtain (or create) the calling thread's entry.
    //
    // A function-local thread_local stores ONE ThreadHandle per thread
    // (shared across all EpochManager instances).  When a thread first
    // touches a given manager, a new ThreadEntry is allocated and CAS-
    // pushed onto that manager's thread_list.  If the thread later
    // accesses a different manager, the old entry is orphaned (marked
    // alive=false via the handle destructor logic below).
    //
    // For the primary use-case -- a single ConcurrentHashMap per
    // application -- this is a non-issue.
    // ------------------------------------------------------------------
    ThreadEntry* get_thread_entry() {
        thread_local ThreadHandle handle;

        ThreadEntry* entry = handle.entry;
        if (entry && entry->owner == this) {
            return entry;
        }

        // If the handle currently points to a different manager's entry,
        // mark that entry as dead before switching.
        if (entry) {
            entry->active.store(false, std::memory_order_release);
            entry->alive.store(false, std::memory_order_release);
        }

        // Allocate a new entry for this (manager, thread) pair.
        entry = new ThreadEntry;
        entry->owner = this;

        // Lock-free CAS push onto the intrusive thread list.
        ThreadEntry* head = thread_list_.load(std::memory_order_relaxed);
        do {
            entry->next.store(head, std::memory_order_relaxed);
        } while (!thread_list_.compare_exchange_weak(
            head, entry,
            std::memory_order_release,
            std::memory_order_relaxed));

        handle.entry = entry;
        return entry;
    }

private:
    // ------------------------------------------------------------------
    // try_advance -- attempt to advance the global epoch.
    //
    // Scans all ThreadEntry nodes.  If every active entry has
    // local_epoch >= global_epoch, it is safe to advance.  After
    // advancing, drain the retire list two generations behind.
    // ------------------------------------------------------------------
    void try_advance() {
        // Serialise so two threads don't race on draining the same list.
        std::unique_lock<std::mutex> lock(advance_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }

        uint64_t epoch = global_epoch_.load(std::memory_order_acquire);

        // Check all active threads.
        ThreadEntry* e = thread_list_.load(std::memory_order_acquire);
        while (e) {
            if (e->alive.load(std::memory_order_acquire) &&
                e->active.load(std::memory_order_acquire)) {
                if (e->local_epoch.load(std::memory_order_acquire) < epoch) {
                    return;  // This thread is still in an older epoch.
                }
            }
            e = e->next.load(std::memory_order_acquire);
        }

        // All active threads are caught up.  Advance.
        uint64_t new_epoch = epoch + 1;
        global_epoch_.store(new_epoch, std::memory_order_release);

        // Drain the retire list two generations behind new_epoch.
        //
        //   retire_lists_[new_epoch % 3]       -- current (new retirements)
        //   retire_lists_[(new_epoch - 1) % 3] -- one gen old (may be read)
        //   retire_lists_[(new_epoch - 2) % 3] -- two gens old (safe)
        //
        // new_epoch >= 2 guarantees (new_epoch - 2) doesn't underflow.
        if (new_epoch >= 2) {
            retire_lists_[(new_epoch - 2) % 3].drain();
        }
    }

    // Allow EpochGuard to call pin/unpin and get_thread_entry.
    friend class EpochGuard;
};

// -----------------------------------------------------------------------
// EpochGuard -- RAII critical-section guard for epoch-based reclamation.
//
// While an EpochGuard is alive on a thread, objects retired in the
// current epoch will NOT be freed.  Guards may nest.
// -----------------------------------------------------------------------
class EpochGuard {
public:
    explicit EpochGuard(EpochManager& mgr)
        : mgr_(mgr)
        , entry_(mgr.get_thread_entry())
    {
        mgr_.pin(entry_);
    }

    ~EpochGuard() {
        mgr_.unpin(entry_);
    }

    // Non-copyable, non-movable.
    EpochGuard(const EpochGuard&) = delete;
    EpochGuard& operator=(const EpochGuard&) = delete;

private:
    EpochManager&              mgr_;
    EpochManager::ThreadEntry* entry_;
};

}  // namespace detail
}  // namespace concurrent_hashmap
