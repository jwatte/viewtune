#include "stdafx.h"
#include "workqueue.h"
#include <pthread.h>
#include <list>

static pthread_mutex_t wqMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wqCond = PTHREAD_COND_INITIALIZER;
static pthread_t *wqThreads;
static int wqThreadCount;
static int wqWorking;
static int wqComplete;
static bool wqRunning;
static std::list<Work *> wqWork;

static void *wq_worker(void *) {
    pthread_mutex_lock(&wqMutex);
    while (wqRunning) {
        if (wqWork.empty()) {
            pthread_cond_wait(&wqCond, &wqMutex);
            if (!wqRunning) {
                break;
            }
        }
        if (!wqWork.empty()) {
            Work *w = wqWork.front();
            wqWork.pop_front();
            wqWorking += 1;
            fprintf(stderr, "got work: %s\n", w->name());
            pthread_mutex_unlock(&wqMutex);
            try {
                w->work();
                w->complete();
            } catch (std::exception const &x) {
                fprintf(stderr, "Work exception in %s: %s\n", w->name(), x.what());
                w->error();
            } catch (...) {
                fprintf(stderr, "Work exception in %s, unknown kind\n", w->name());
                w->error();
            }
            pthread_mutex_lock(&wqMutex);
            wqWorking -= 1;
        }
    }
    wqComplete += 1;
    pthread_cond_broadcast(&wqCond);
    pthread_mutex_unlock(&wqMutex);
    return 0;
}

bool start_work_queue(int nthreads) {
    if (wqThreads) {
        return false;
    }
    wqThreads = new pthread_t[nthreads];
    wqThreadCount = nthreads;
    wqRunning = true;
    wqWorking = 0;
    wqComplete = 0;
    for (int i = 0; i != nthreads; ++i) {
        if (pthread_create(&wqThreads[i], NULL, wq_worker, nullptr)) {
            fprintf(stderr, "work queue create failed\n");
            exit(1);
        }
    }
    return true;
}

bool add_work(Work *work) {
    pthread_mutex_lock(&wqMutex);
    wqWork.push_back(work);
    pthread_cond_signal(&wqCond);
    fprintf(stderr, "work added: %s\n", work->name());
    pthread_mutex_unlock(&wqMutex);
    return true;
}

void wait_for_all_work_to_complete() {
    pthread_mutex_lock(&wqMutex);
    while (wqWorking > 0 || !wqWork.empty()) {
        pthread_cond_broadcast(&wqCond);
    }
    pthread_mutex_unlock(&wqMutex);
}

void stop_work_queue() {
    wqRunning = false;
    for (int i = 0; i != wqThreadCount; ++i) {
        void *j = nullptr;
        pthread_cond_broadcast(&wqCond);
        pthread_join(wqThreads[i], &j);
    }
    delete[] wqThreads;
    wqThreads = nullptr;
}

int get_num_working() {
    return wqWorking;
}

