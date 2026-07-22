#include "libc.h"

#define MAX_WORKERS 6

struct worker_state {
    uint32_t ip;
    uint16_t port;
    volatile int result;
};

static struct worker_state workers[MAX_WORKERS];
static volatile int ready_count;
static volatile int start_round;

static int response_has_token(int sd) {
    static const char token[] = "NETSTRESS_OK";
    uint8_t buf[512];
    size_t matched = 0;

    for (;;) {
        int n = recv(sd, buf, sizeof(buf), 0);
        if (n < 0)
            return -1;
        if (n == 0)
            return matched == sizeof(token) - 1 ? 0 : -2;
        for (int i = 0; i < n; i++) {
            if (buf[i] == (uint8_t)token[matched]) {
                matched++;
                if (matched == sizeof(token) - 1)
                    return 0;
            } else {
                matched = buf[i] == (uint8_t)token[0] ? 1u : 0u;
            }
        }
    }
}

static void run_worker(int index) {
    static const char request[] =
        "GET /stress HTTP/1.0\r\n"
        "Host: 10.0.2.2\r\n"
        "Connection: close\r\n\r\n";
    struct worker_state *worker = &workers[index];
    worker->result = -10;
    __sync_add_and_fetch(&ready_count, 1);
    while (!start_round)
        yield();

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        worker->result = -1;
        return;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(worker->port);
    addr.sin_addr = worker->ip;
    if (connect(sd, &addr, sizeof(addr)) < 0) {
        worker->result = -2;
        closesocket(sd);
        return;
    }
    if (send(sd, request, sizeof(request) - 1, 0) < 0) {
        worker->result = -3;
        closesocket(sd);
        return;
    }

    int ret = response_has_token(sd);
    worker->result = ret == 0 ? 0 : (ret == -1 ? -4 : -5);
    closesocket(sd);
}

static void worker_0(void) { run_worker(0); }
static void worker_1(void) { run_worker(1); }
static void worker_2(void) { run_worker(2); }
static void worker_3(void) { run_worker(3); }
static void worker_4(void) { run_worker(4); }
static void worker_5(void) { run_worker(5); }

static thread_fn worker_functions[MAX_WORKERS] = {
    worker_0, worker_1, worker_2, worker_3, worker_4, worker_5,
};

int main(int argc, char **argv) {
    if (argc != 4) {
        puts("netstress: usage: netstress <port> <workers> <rounds>");
        return 2;
    }
    int port = atoi(argv[1]);
    int worker_count = atoi(argv[2]);
    int rounds = atoi(argv[3]);
    if (port <= 0 || port > 65535 ||
        worker_count < 2 || worker_count > MAX_WORKERS ||
        rounds <= 0 || rounds > 100) {
        puts("netstress: bad arguments");
        return 2;
    }

    for (int round = 0; round < rounds; round++) {
        int tids[MAX_WORKERS];
        ready_count = 0;
        start_round = 0;
        for (int i = 0; i < worker_count; i++) {
            workers[i].ip = 10u | (2u << 16) | (2u << 24);
            workers[i].port = (uint16_t)port;
            workers[i].result = -20;
            tids[i] = spawn(worker_functions[i]);
            if (tids[i] < 0) {
                printf("netstress: spawn failed round %d worker %d\n", round, i);
                return 1;
            }
        }
        while (ready_count != worker_count)
            yield();
        __sync_synchronize();
        start_round = 1;

        int failed = 0;
        for (int i = 0; i < worker_count; i++) {
            join(tids[i]);
            if (workers[i].result != 0) {
                printf("netstress: failed round %d worker %d stage %d\n",
                       round, i, workers[i].result);
                failed = 1;
            }
        }
        if (failed)
            return 1;
        printf("netstress: round %d ok\n", round + 1);
    }

    printf("netstress: ok %dx%d\n", rounds, worker_count);
    return 0;
}
