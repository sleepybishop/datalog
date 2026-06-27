#include "linda.h"
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

static void *producer_thread(void *arg)
{
    s_linda_space *space = (s_linda_space *)arg;
    printf("[Producer] Sleeping for 1 second...\n");
    sleep(1);
    printf("[Producer] Placing tuple: ('task', '1', 'compile')\n");
    linda_out(space, "task", "1", "compile");
    return NULL;
}

static void *consumer_thread(void *arg)
{
    s_linda_space *space = (s_linda_space *)arg;
    char id[256] = {0};
    char action[256] = {0};
    printf("[Consumer] Blocking on in('task', '?Id', '?Action')...\n");
    int rc = linda_in(space, "task", "?Id", "?Action", NULL, id, action);
    assert(rc == 0);
    printf("[Consumer] Woke up! Retrieved tuple: ('task', '%s', '%s')\n", id, action);
    return NULL;
}

static void *worker_square(void *arg)
{
    s_linda_space *space = (s_linda_space *)arg;
    printf("[Worker] Doing complex calculation: 9 * 9\n");
    sleep(1);
    linda_out(space, "result", "9", "81");
    printf("[Worker] Out-ed result: ('result', '9', '81')\n");
    return NULL;
}

int main()
{
    printf("Initializing Linda Tuplespace examples...\n");
    s_linda_space *space = new_linda_space(10000);
    assert(space != NULL);

    pthread_t prod, cons;

    // 1. Thread coordination: Producer/Consumer
    pthread_create(&cons, NULL, consumer_thread, space);
    pthread_create(&prod, NULL, producer_thread, space);

    pthread_join(cons, NULL);
    pthread_join(prod, NULL);

    // 2. Parallel computation: eval()
    printf("\n[Main] Spawning parallel worker via eval()...\n");
    linda_eval(space, worker_square, space);

    char val[256] = {0};
    printf("[Main] Blocking on rd('result', '9', '?Val')...\n");
    int rc = linda_rd(space, "result", "9", "?Val", NULL, NULL, val);
    assert(rc == 0);
    printf("[Main] Found result! 9 squared = %s\n", val);

    // 3. Non-blocking checking: rdp/inp
    printf("\n[Main] Checking non-blocking rdp...\n");
    int found = linda_rdp(space, "result", "9", "81", NULL, NULL, NULL);
    printf("[Main] rdp('result', '9', '81') found = %d (expected 1)\n", found);
    assert(found == 1);

    printf("[Main] Consuming result via non-blocking inp...\n");
    found = linda_inp(space, "result", "9", "81", NULL, NULL, NULL);
    printf("[Main] inp('result', '9', '81') found/consumed = %d (expected 1)\n", found);
    assert(found == 1);

    found = linda_rdp(space, "result", "9", "81", NULL, NULL, NULL);
    printf("[Main] Second rdp('result', '9', '81') found = %d (expected 0)\n", found);
    assert(found == 0);

    delete_linda_space(space);
    printf("\nAll Linda tests passed successfully!\n");
    return 0;
}
