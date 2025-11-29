#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/synch.h"
#include "threads/thread.h"

#define CODE_SEGMENT ((uint64_t)0x0400000) /* 코드 세그먼트 시작 주소 */

struct fork_aux {
    struct thread* parent;
    struct intr_frame* if_parent;

    struct semaphore loaded; /* 자식 프로세스 로드 확인 */
    bool success;            /* 자식 프로세스 로드 성공 여부 */
    struct child_thread* ch; /* 자식 프로세스 */
};

struct initd_aux {
    char* file_name;
    struct child_thread* child;
    struct thread* parent;
};

tid_t process_create_initd(const char* file_name);
tid_t process_fork(const char* name, struct intr_frame* if_);
int process_exec(void* f_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(struct thread* next);
int new_fd(struct thread* t, struct file* f);

#endif /* userprog/process.h */
