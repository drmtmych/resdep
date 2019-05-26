#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <signal.h>
#include <string.h>

#include <math.h>

#include "loadgen_sysload.h"
#include "loadgen_thread.h"
#include "loadgen_log.h"

pthread_t *loadgen_threads = NULL;
int *is_running = NULL;
int loadgen_threads_num = -1;
pthread_attr_t pthread_attr;

/* This hack is extremely dirty.
 * Since signals 1..8 are blockable, each CPU thread
 * will use its own signal corresponding to its index
 * (and thus CPU number).
 */
#define SIG_TO_IND(sig)     (sig - 1)
#define IND_TO_SIG(ind)     (ind + 1)

void loadgen_thread_init(void)
{
    int ret;

    /* Initialize array of thread data. */
    loadgen_threads_num = cpus_onln;
    loadgen_threads = malloc(sizeof(*loadgen_threads) * loadgen_threads_num);
    is_running = malloc(sizeof(*is_running) * cpus_onln);
    if (!loadgen_threads || !is_running)
        log_exit("malloc");
    memset((void *) loadgen_threads, 0,
           sizeof(*loadgen_threads) * loadgen_threads_num);
    memset((void *) is_running, 0, sizeof(*is_running) * cpus_onln);

    /* We're going to launch threads in detached state. */
    if ((ret = pthread_attr_init(&pthread_attr)) != 0) {
        errno = ret;
        log_exit("pthread_attr_init");
    }
    ret = pthread_attr_setdetachstate(&pthread_attr, PTHREAD_CREATE_DETACHED);
    if (ret != 0) {
        errno = ret;
        log_exit("pthread_attr_setdetachstate");
    }
}

void thread_sigterm_handler(int signo)
{
    /* Die silently. */
    pthread_exit(NULL);
}

void loadgen_run_threads(void)
{
    int i;
    for (i = 0; i < cpus_onln; ++i) {
        if (cur_sysload->cpu_load_user[i].cpu_num < 0)
            continue;

        if (pthread_create(&(loadgen_threads[i]), &pthread_attr,
                           loadgen_cpu_thread_fn,
                           (void *) &(cur_sysload->cpu_load_user[i])) != 0)
            log_err("pthread_create");
    }
}

void loadgen_kill_threads(void)
{
    int i;
    for (i = 0; i < loadgen_threads_num; ++i) {
        if (pthread_kill(loadgen_threads[i], LOADGEN_THREAD_TERM_SIGNAL) < 0) {
            log_err("pthread_kill");
        }
    }
    memset((void *) loadgen_threads, 0,
           sizeof(*loadgen_threads) * loadgen_threads_num);
}

static void loadgen_cpu_thread_switch_state(int signo)
{
    is_running[SIG_TO_IND(signo)] = 0;
}

void detect_mem_borders(int thread_index, unsigned char **mem_begin,
                        unsigned char **mem_end)
{
    int mem_pages, per_thread, offset;
    int page_begin, page_end;

    mem_pages = cur_sysload->mem_pages_num;
    while ((mem_pages % cpus_onln) != 0)
        mem_pages++;
    per_thread = mem_pages / cpus_onln;
    offset = cur_sysload->mem_pages_num - mem_pages;
    page_begin = offset + thread_index * per_thread;
    if (page_begin < 0)
        page_begin = 0;
    page_end = offset + (thread_index + 1) * per_thread;

    *mem_begin = loadgen_mem + page_begin * page_size;
    *mem_end = loadgen_mem + page_end * page_size;
}

void *loadgen_cpu_thread_fn(void *arg)
{
    int ret;
    int thread_index;
    int load_msec;
    struct cpu_load *cpu_load;
    cpu_set_t cpu_set;
    sigset_t sigset;
    struct sigaction sa;
    struct sigevent sev;
    timer_t timerid;
    struct itimerspec work_its;
    struct timespec sleep_ts;
    unsigned char *mem_begin, *mem_end, *p;

    cpu_load = (struct cpu_load *) arg;
    thread_index = cpu_load->cpu_num;
    load_msec = cpu_load->load_msec;

    detect_mem_borders(thread_index, &mem_begin, &mem_end);

    /* Set CPU affinity. */
    CPU_ZERO(&cpu_set);
    CPU_SET(thread_index, &cpu_set);
    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
    if (ret != 0) {
        errno = ret;
        thread_log_exit("pthread_setaffinity_np");
    }

    /* Set thread's sigmask. */
    sigfillset(&sigset);
    sigdelset(&sigset, LOADGEN_THREAD_TERM_SIGNAL);
    sigdelset(&sigset, IND_TO_SIG(thread_index));
    if ((ret = pthread_sigmask(SIG_SETMASK, &sigset, NULL)) != 0) {
        errno = ret;
        thread_log_exit("pthread_sigmask");
    }

    /* Set signal disposition. */
    memset(&sa, 0, sizeof(struct sigaction));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = loadgen_cpu_thread_switch_state;
    if (sigaction(IND_TO_SIG(thread_index), &sa, NULL) < 0)
        thread_log_exit("sigaction");

    /* Create the timer. */
    memset(&sev, 0, sizeof(struct sigevent));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = IND_TO_SIG(thread_index);
    sev._sigev_un._tid = gettid();
    if (timer_create(LOADGEN_THREAD_CLOCKID, &sev, &timerid) < 0)
        thread_log_exit("timer_create");

    memset(&work_its, 0, sizeof(struct itimerspec));
    memset(&sleep_ts, 0, sizeof(struct timespec));
    work_its.it_value.tv_nsec = MSEC_TO_NSEC(load_msec);
    sleep_ts.tv_nsec = NSEC_PER_SEC - work_its.it_value.tv_nsec;

    is_running[thread_index] = 1;
    p = mem_begin;
    while (1) {
        timer_settime(timerid, 0, &work_its, NULL);
        while (is_running[thread_index]) {
            *p = sqrt((long int) p);
            p = p < mem_end ? p + page_size : mem_begin;
        }
        clock_nanosleep(LOADGEN_THREAD_CLOCKID, 0, &sleep_ts, NULL);
        is_running[thread_index] = 1;
    }

    /* Unreachable. */
    pthread_exit(NULL);
}