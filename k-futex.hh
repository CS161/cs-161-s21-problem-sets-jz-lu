#include "k-wait.hh" // imports "kernel.hh" so this file cannot be imported in "kernel.hh"
#include "k-lock.hh"

struct futexstate {
    std::atomic<int> ref_ = 1; // state freed when ref_ == 0
    list_links link_; // for list in `struct futexwaiters`
    const uint32_t *word_;
    wait_queue wq_;

    inline futexstate(uint32_t* word) : word_(word) {};
    void wake_some(int nwake);
    void wake_all();
};

struct futexwaiters {
    spinlock lock_; // protects accesses to states_
    list<futexstate, &futexstate::link_> states_;
    futexstate* add(uint32_t* word);
    void remove(futexstate* state);
    void wake_some(futexstate* ftstate, int nwake);
    void wake_some(uint32_t *word, int nwake);
    void check_timeout();
    static inline futexwaiters& get();

 private:
    static futexwaiters ftwaiters;
    futexstate* find(uint32_t *word);
    futexwaiters();
    NO_COPY_OR_ASSIGN(futexwaiters);
};


inline futexwaiters& futexwaiters::get() {
    return ftwaiters;
}

