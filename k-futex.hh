#include "k-wait.hh"
#include "k-lock.hh"

struct futexstate {
    std::atomic<int> ref_ = 1; // state freed when ref_ == 0
    list_links link_; // linker in list of futexstates
    const uint32_t *word_;
    wait_queue wq_;

    inline futexstate(uint32_t* word) : word_(word) {};
    void wake_all();
};

struct futexwaiters {
    spinlock lock_; // protects accesses to states_
    list<futexstate, &futexstate::link_> states_;
    int add(uint32_t* word);
    void remove(futexstate* state);
    void wake_all(futexstate* ftstate);
    void wake_all(uint32_t *word);
    static inline futexwaiters& get();

 private:
    static futexwaiters ftwaiters;
    futexstate* find(uint32_t *word);
    NO_COPY_OR_ASSIGN(futexwaiters);
};


inline futexwaiters& futexwaiters::get() {
    return ftwaiters;
}
