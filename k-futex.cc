#include "k-futex.hh"
#include "kernel.hh"

void futexstate::wake_all() {
    wq_.wake_all();
}


int futexwaiters::add(uint32_t* word) {
    if (!word) {
        return -1;
    }
    spinlock_guard guard(lock_);
    if (futexstate* state = find(word)) {
        ++state->ref_;
    } else {
        futexstate* newstate = knew<futexstate>(word); // ref_ starts at 1
        if (!newstate) {
            return E_NOMEM;
        }
        states_.push_back(newstate);
    }
    return 0;
}


// Assumes locked.
futexstate* futexwaiters::find(uint32_t *word) {
    if (!word) {
        return nullptr;
    }
    for (auto it = states_.front(); it; it = states_.next(it)) {
        if (word == it->word_) {
            return it;
        }
    }
    return nullptr;
}


void futexwaiters::wake_all(futexstate* ftstate) {
    if (ftstate) {
        ftstate->wake_all();
    }
}


void futexwaiters::wake_all(uint32_t* word) {
    if (!word) {
        return;
    }
    if (futexstate* state = find(word)) {
        state->wake_all();
    }
}


void futexwaiters::remove(futexstate* state) {
    if (!state) {
        return;
    }
    spinlock_guard guard(lock_);
    if (!(--state->ref_)) {
        if (state->link_.is_linked()) {
            states_.erase(state);
        }
        delete state;
    }
}
