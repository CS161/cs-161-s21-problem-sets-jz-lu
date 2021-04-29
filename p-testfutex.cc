#include "u-lib.hh"
#define NLOOP 100 // raise probability of catching race conditions

// A simple handshake-based test for futexes.
void process_main() {
    int* shared_data = shmat(shm_id, NULL, 0);
    *shared_data = 0;

    int forkstatus = fork();
    if (forkstatus < 0) {
        perror("fork");
        exit(1);
    }

    if (forkstatus == 0) {
        // Child process

        printf("child waiting for A\n");
        wait_on_futex_value(shared_data, 0xA);

        printf("child writing B\n");
        // Write 0xB to the shared data and wake up parent.
        *shared_data = 0xB;
        wake_futex_blocking(shared_data);
    } else {
        // Parent process.

        printf("parent writing A\n");
        // Write 0xA to the shared data and wake up child.
        *shared_data = 0xA;
        wake_futex_blocking(shared_data);

        printf("parent waiting for B\n");
        wait_on_futex_value(shared_data, 0xB);

        // Wait for the child to terminate.
        wait(NULL);
        shmdt(shared_data);
    }

    sys_exit(0);
}