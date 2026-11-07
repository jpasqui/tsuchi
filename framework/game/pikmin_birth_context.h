// Native birth context belongs to one admitted thread and synchronous call.
#pragma once
#include <stdint.h>
namespace tsuchi {
class PikminBirthContexts {
public:
    struct Value {
        uint64_t thread = 0, owner = 0, part = 0;
        uint8_t color = 0;
        bool present = false, blocked = false;
    };
    static constexpr unsigned capacity = 32;
private:
    struct Node { Value value; Node* next = nullptr; bool active = false, quarantined = false; };
    Node* head_ = nullptr;
    unsigned count_ = 0;
    bool locked_ = false;
    struct Lock {
        PikminBirthContexts& self;
        explicit Lock(PikminBirthContexts& at) : self(at) {
            while (__atomic_test_and_set(&self.locked_, __ATOMIC_ACQUIRE)) {}
        }
        ~Lock() { __atomic_clear(&self.locked_, __ATOMIC_RELEASE); }
    };
public:
    constexpr PikminBirthContexts() = default;
    PikminBirthContexts(const PikminBirthContexts&) = delete;
    Value snapshot(uint64_t thread) {
        if (!thread) return {};
        Lock lock(*this);
        for (Node* at = head_; at; at = at->next) if (at->value.thread == thread) {
            if (at->quarantined) return {thread, 0, 0, 0, true, true};
            return at->value;
        }
        return {};
    }
    class Scope {
        PikminBirthContexts& self_;
        Node node_{};
    public:
        Scope(PikminBirthContexts& self, Value wanted) : self_(self) {
            if (!wanted.thread) return;
            Lock lock(self_);
            if (self_.count_ == capacity) {
                // Overflow never exposes an enclosing frame as the failed child.
                // Its thread stays quarantined until those owned calls return.
                for (Node* at = self_.head_; at; at = at->next)
                    if (at->value.thread == wanted.thread) at->quarantined = true;
                return;
            }
            for (Node* at = self_.head_; at; at = at->next)
                if (at->value.thread == wanted.thread && at->quarantined) return;
            wanted.present = true;
            node_.value = wanted; node_.next = self_.head_; node_.active = true;
            self_.head_ = &node_; ++self_.count_;
        }
        Scope(const Scope&) = delete;
        bool allowed(uint64_t thread) {
            if (!thread || thread != node_.value.thread) return false;
            Lock lock(self_);
            return node_.active && !node_.quarantined;
        }
        ~Scope() {
            if (!node_.active) return;
            // The synchronous call owns this node even if the witness was reset.
            Lock lock(self_);
            Node** link = &self_.head_;
            while (*link && *link != &node_) link = &(*link)->next;
            if (*link) { *link = node_.next; --self_.count_; }
            node_.active = false;
        }
    };
};
}
