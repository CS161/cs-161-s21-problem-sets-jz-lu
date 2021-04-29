#include "k-futex.hh"
#include "kernel.hh"

void futexstate::wake_all() {
    wq_.wake_all();
}


int futexwaiters::add(uint32_t* word) {
    if (find(word)) {
        return -1;
    }
    futexstate* state = knew<futexstate>(word);
    if (!state) {
        return E_NOMEM;
    }
    spinlock_guard guard(lock_);
    states_.push_back(state);
    return 0;
}


futexstate* futexwaiters::find(uint32_t *word) {
    spinlock_guard guard(lock_);
    for (auto it = states_.front(); it; it = states_.next(it)) {
        if (word == it->word_) {
            return it;
        }
    }
    return nullptr;
}


void futexwaiters::wake_all(futexstate* ftstate) {
    ftstate->wake_all();
}


void futexwaiters::wake_all(uint32_t *word) {
    if (futexstate* state = find(word)) {
        state->wake_all();
    }
}


