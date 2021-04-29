#include "k-list.hh"
#include "k-waitstruct.hh"

struct futexstate {
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
    futexstate* find(uint32_t *word);
    void wake_all(futexstate* ftstate);
    void wake_all(uint32_t *word);
};
