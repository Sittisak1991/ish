#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/process.h"
#include "emu/memory.h"

__thread struct process *current;

static struct pid pids[MAX_PID + 1] = {};
lock_t pids_lock = LOCK_INITIALIZER;

static bool pid_empty(struct pid *pid) {
    return pid->proc == NULL && list_empty(&pid->session) && list_empty(&pid->group);
}

struct pid *pid_get(dword_t id) {
    struct pid *pid = &pids[id];
    if (pid_empty(pid))
        return NULL;
    return pid;
}

struct process *pid_get_proc_zombie(dword_t id) {
    struct pid *pid = pid_get(id);
    if (pid == NULL)
        return NULL;
    struct process *proc = pid->proc;
    return proc;
}

struct process *pid_get_proc(dword_t id) {
    struct process *proc = pid_get_proc_zombie(id);
    if (proc != NULL && proc->zombie)
        return NULL;
    return proc;
}

struct process *process_create(struct process *parent) {
    lock(pids_lock);
    static int cur_pid = 1;
    while (!pid_empty(&pids[cur_pid])) {
        cur_pid++;
        if (cur_pid > MAX_PID) cur_pid = 0;
    }
    struct pid *pid = &pids[cur_pid];
    pid->id = cur_pid;
    list_init(&pid->session);
    list_init(&pid->group);

    struct process *proc = malloc(sizeof(struct process));
    if (proc == NULL)
        return NULL;
    *proc = (struct process) {};
    if (parent != NULL)
        *proc = *parent;
    proc->pid = pid->id;
    pid->proc = proc;
    unlock(pids_lock);

    list_init(&proc->children);
    list_init(&proc->siblings);
    if (parent != NULL) {
        proc->parent = parent;
        proc->ppid = parent->pid;
        list_add(&parent->children, &proc->siblings);
    }

    proc->has_timer = false;
    proc->children_rusage = (struct rusage_) {};

    lock_init(proc->signal_lock);
    lock_init(proc->exit_lock);
    pthread_cond_init(&proc->child_exit, NULL);
    pthread_cond_init(&proc->vfork_done, NULL);
    return proc;
}

void process_destroy(struct process *proc) {
    lock(pids_lock);
    list_remove(&proc->siblings);
    list_remove(&proc->group);
    list_remove(&proc->session);
    pid_get(proc->pid)->proc = NULL;
    unlock(pids_lock);
    free(proc);
}

void (*process_run_hook)() = NULL;

static void *process_run(void *proc) {
    current = proc;
    if (process_run_hook)
        process_run_hook();
    else
        cpu_run(&current->cpu);
    abort(); // above function call should never return
}

void process_start(struct process *proc) {
    if (pthread_create(&proc->thread, NULL, process_run, proc) < 0)
        abort();
    pthread_detach(proc->thread);
}
