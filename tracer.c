#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "decoder.h"
#include "helpers.h"

// TODO, following is also copypasta.

// Change the magic anytime the content or the semantics of this struct change.
#define TRACER_STRUCT_MAGIC 0xbeefcafe

#include <semaphore.h>

#define COALESCED_TRACE_BUFFER_LEN 32
typedef struct {
    uint32_t magic;
    sem_t one_thread_at_a_time;
    sem_t tracer_ready;
    sem_t tracers_turn;
    sem_t tracer_done;
    // When a thread calls wait_for_tracer, it'll dump its own trace buffer
    // into this consolidated buffer.
    struct {
        pid_t tid;
        uint32_t value;
    } buffer[COALESCED_TRACE_BUFFER_LEN];
    size_t remaining;
} __attribute__((packed)) coalesced_trace_struct;

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: tracer <pid> <decoderfile>\n", stderr);
        return 1;
    }
    int tracee_pid = atoi(argv[1]);
    char *decoder_fn = argv[2];

    decoder_t *decoder = decoder_load(decoder_fn, 0);

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
    int expected = sizeof(coalesced_trace_struct);
    if (sz != expected) {
        fprintf(stderr, "shm is wrong size: got %i, expected %i\n", sz, expected);
        return 1;
    }

    // mmap it in.
    coalesced_trace_struct *shm;
    if ((shm = mmap(NULL, sizeof(coalesced_trace_struct), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0))
        == MAP_FAILED) {
        fprintf(stderr, "mmap() failed with %s\n", strerror(errno));
        return 1;
    }

    // Does it have the right magic?
    if (shm->magic != TRACER_STRUCT_MAGIC) {
        fprintf(stderr, "bad magic! got 0x%x, expected 0x%x\n", shm->magic, TRACER_STRUCT_MAGIC);
        return 1;
    }

    id_t line_just_traced = 0;
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
        for (int i = 0; i < COALESCED_TRACE_BUFFER_LEN; i++) {
            // Find the corresponding line(s) and print them.
            chunk_data_t *chunk_data = decoder_lookup_chunk(decoder, shm->buffer[i].value);
            ASSERT(chunk_data != NULL);
            for (int i = 0; i < chunk_data->line_id_count; i++) {
                id_t line_id = chunk_data->line_ids[i];
                if (line_id != line_just_traced) {
                    line_data_t *line_data = decoder_lookup_line(decoder, line_id);
                    ASSERT(line_data != NULL);
                    printf("%s L%i: %s\n", line_data->path, line_data->line_no, line_data->content);
                    line_just_traced = line_id;
                }
            }
        }

        // Let the tracee know we're done.
        ASSERT(sem_post(&(shm->tracer_ready)) == 0);
        ASSERT(sem_post(&(shm->tracer_done)) == 0);
    }
tracee_dead:

    // Even though the tracee's dead, and they can't flip the semaphore on for
    // us, we can still poke through the leftovers in the buffer.
    // TODO

    return 0;
}
