#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "assert.h"

// TODO, following is also copypasta.

// Change the magic anytime the content or the semantics of this struct change.
#define TRACER_STRUCT_MAGIC 0xbeefcafe

#include <semaphore.h>
#include <stdint.h>

typedef struct tracer {
    uint32_t magic;
    sem_t tracer_ready;
    sem_t tracers_turn;
    sem_t tracer_done;
    uint32_t buffer[32];
    uint32_t buffer_sentinel;
} __attribute__((packed)) tracer_struct;

int main(int argc, char **argv) {
    // Take the pid to trace on the command line.
    if (argc != 2) {
        fputs("usage: tracer <pid>\n", stderr);
        return 1;
    }
    int tracee_pid = atoi(argv[1]);

    // TODO: shm_name construction is copypasta.
    // Open the associated shm.
    char shm_name[128] = { '\0' };
    ASSERT(snprintf(shm_name, sizeof(shm_name), "/as-tracer-%s", argv[1]) < sizeof(shm_name))
    int fd;
    if ((fd = shm_open(shm_name, O_RDWR, 0666)) == -1) {
        fprintf(stderr, "shm_open(\"%s\") failed with %s\n", shm_name, strerror(errno));
        return 1;
    }

    // Is it the right size?
    int sz = lseek(fd, 0, SEEK_END);
    if (sz != sizeof(tracer_struct)) {
        fprintf(stderr, "shm is wrong size: got %i, expected %i\n", sz, sizeof(tracer_struct));
        return 1;
    }

    // mmap it in.
    tracer_struct *shm;
    if ((shm = mmap(NULL, sizeof(tracer_struct), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0))
        == MAP_FAILED) {
        fprintf(stderr, "mmap() failed with %s\n", strerror(errno));
        return 1;
    }

    // Does it have the right magic?
    if (shm->magic != TRACER_STRUCT_MAGIC) {
        fprintf(stderr, "bad magic! got 0x%x, expected 0x%x\n", shm->magic, TRACER_STRUCT_MAGIC);
        return 1;
    }

    ASSERT(sem_post(&(shm->tracer_ready)) == 0);
    while (1) {
        // Wait for our turn.
        struct timespec timeout;
        ASSERT(clock_gettime(CLOCK_REALTIME, &timeout) == 0);
        timeout.tv_sec += 1;
        int ret;
        while ((ret = sem_timedwait(&(shm->tracers_turn), &timeout)) == -1) {
            ASSERT((errno == EINTR) || (errno = ETIMEDOUT));

            // Check if the tracee is alive. If it isn't, we're done.
            if ((ret = kill(tracee_pid, 0)) != 0) {
                fprintf(stderr, "kill() failed with %s\n", strerror(errno));
                goto tracee_dead;
            }
        }

        // Read out the trace data! The trace buffer must be full, otherwise we
        // wouldn't have been woken up.
        printf("trace:");
        for (int i = 0; i < sizeof(shm->buffer) / sizeof(shm->buffer[0]); i++) {
            printf(" [%i]%i", i, shm->buffer[i]);
            ASSERT(shm->buffer[i] != 0);
        }
        puts("");

        // Let the tracee know we're done.
        ASSERT(sem_post(&(shm->tracer_ready)) == 0);
        ASSERT(sem_post(&(shm->tracer_done)) == 0);
    }
tracee_dead:

    return 0;
}