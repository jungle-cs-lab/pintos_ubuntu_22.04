#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <filesys/filesys.h>
#include <filesys/file.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/palloc.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include <string.h>

void syscall_entry(void);
void syscall_handler(struct intr_frame*);

#define CONSOLE_RING_SIZE 1024 /* 콘솔 링버퍼 크기 */
#define CONSOLE_CHUNK 256      /* 콘솔 flush 시 출력 청크 크기 */

static struct lock lock;
static struct lock console_buffer_lock;
static char console_ring[CONSOLE_RING_SIZE];
static size_t console_ring_len;

void exit(int status);
static int sys_fork(const char* thread_name, struct intr_frame* f);
static int exec(const char* cmd_line);
static int wait(int pid);
static int create(char* file_name, int initial_size);
static int write(int fd, const void* buffer, unsigned size);
static int open(const char* file_name);
static void close(int fd);
static void check_valid_ptr(int count, ...);
static int read(int fd, void* buffer, unsigned size);
static int filesize(int fd);
static void seek(int fd, unsigned position);
static void check_valid_fd(int fd);
static void flush_console_buffer(void);
static void enqueue_console_output(const char* buf, size_t size);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
    lock_init(&lock);
    lock_init(&console_buffer_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame* f UNUSED)
{
    int syscall_num = f->R.rax;
    uint64_t arg1 = f->R.rdi;
    uint64_t arg2 = f->R.rsi;
    uint64_t arg3 = f->R.rdx;

    switch (syscall_num) {

    case SYS_EXIT:
        exit((int)arg1);
        break;

    case SYS_FORK:
        f->R.rax = sys_fork((const char*)arg1, f);
        break;

    case SYS_EXEC:
        f->R.rax = exec((const char*)arg1);
        break;

    case SYS_WAIT:
        f->R.rax = wait((int)arg1);
        break;

    case SYS_CREATE:
        f->R.rax = create((char*)arg1, (int)arg2);
        break;

    case SYS_WRITE:
        f->R.rax = write((int)arg1, (const void*)arg2, (unsigned)arg3);
        break;

    case SYS_OPEN:
        f->R.rax = open((const char*)arg1);
        break;

    case SYS_CLOSE:
        close((int)arg1);
        break;

    case SYS_READ:
        f->R.rax = read((int)arg1, (void*)arg2, (unsigned)arg3);
        break;

    case SYS_FILESIZE:
        f->R.rax = filesize((int)arg1);
        break;

    case SYS_SEEK:
        seek((int)arg1, (unsigned)arg2);
        break;

    default:
        thread_exit();
    }
}

void exit(int status)
{
    struct thread* t = thread_current();

    t->exit_status = status;
    thread_exit();
}

static int sys_fork(const char* thread_name, struct intr_frame* f)
{
    check_valid_ptr(1, thread_name);

    /* thread name을 커널 영역으로 복사 */
    char* tn_copy = palloc_get_page(0);
    if (tn_copy == NULL)
        return TID_ERROR;
    strlcpy(tn_copy, thread_name, PGSIZE);

    /* parent intr frame을 커널 영역으로 복사 */
    struct intr_frame* parent_tf_copy = palloc_get_page(0);
    if (parent_tf_copy == NULL)
        return TID_ERROR;
    memcpy(parent_tf_copy, f, sizeof(struct intr_frame));

    tid_t tid = process_fork(tn_copy, parent_tf_copy);

    palloc_free_page(tn_copy);
    palloc_free_page(parent_tf_copy);

    /*
     * parent : return child pid
     * child  : return 0
     */
    return tid;
}

static int exec(const char* cmd_line)
{
    check_valid_ptr(1, cmd_line);

    /* cmd line을 커널 영역으로 복사 */
    char* cl_copy;
    cl_copy = palloc_get_page(0);
    if (cl_copy == NULL)
        return TID_ERROR;
    strlcpy(cl_copy, cmd_line, PGSIZE);

    process_exec(cl_copy);

    // 실패하고 현재 코드 흐름으로 돌아온 경우, failure
    exit(-1);
}

static int wait(int pid)
{
    /* 자식이 없으면 종료 */
    if (!list_size(&thread_current()->children)) {
        return -1;
    }

    return process_wait(pid);
}

static int create(char* file_name, int initial_size)
{
    check_valid_ptr(1, file_name);

    lock_acquire(&lock); // 동시 접근 방지
    int result = filesys_create(file_name, initial_size);
    lock_release(&lock);

    return result;
}

static int write(int fd, const void* buffer, unsigned size)
{
    check_valid_ptr(1, buffer);
    // need to add logic to check entire buffer

    if (fd == 1) {
        enqueue_console_output((char*)buffer, size);
        flush_console_buffer();
        return size;
    }

    check_valid_fd(fd);

    struct thread* curr = thread_current();
    struct file* f = curr->fdte[fd];

    lock_acquire(&lock);
    int bytes_written = file_write(f, buffer, size);
    lock_release(&lock);

    return bytes_written;
}

/* 콘솔 출력 링버퍼로 enqueue */
static void enqueue_console_output(const char* buf, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        lock_acquire(&console_buffer_lock);
        size_t space = CONSOLE_RING_SIZE - console_ring_len;
        if (space == 0) {
            lock_release(&console_buffer_lock);
            flush_console_buffer();
            continue;
        }

        size_t chunk = size - offset;
        if (chunk > space)
            chunk = space;

        memcpy(console_ring + console_ring_len, buf + offset, chunk);
        console_ring_len += chunk;
        offset += chunk;
        lock_release(&console_buffer_lock);
    }
}

/* 링버퍼에 모인 데이터를 실제 콘솔로 flush */
static void flush_console_buffer(void)
{

    char local[CONSOLE_CHUNK];
    size_t chunk;

    lock_acquire(&console_buffer_lock);
    if (console_ring_len == 0) {
        lock_release(&console_buffer_lock);
        return;
    }

    chunk = console_ring_len;
    if (chunk > CONSOLE_CHUNK)
        chunk = CONSOLE_CHUNK;
    memcpy(local, console_ring, chunk);
    memmove(console_ring, console_ring + chunk, console_ring_len - chunk);
    console_ring_len -= chunk;
    lock_release(&console_buffer_lock);

    putbuf(local, chunk);
}

static int open(const char* file_name)
{
    check_valid_ptr(1, file_name);

    struct file* f;
    struct thread* curr = thread_current();
    int fd = -1;

    lock_acquire(&lock);

    if (strcmp(curr->name, file_name) == 0 &&
        curr->execute_file != NULL) { /* 현재 프로세스와 open 파일이 동일한 경우 */
        f = file_duplicate(curr->execute_file);
        if (f == NULL) {
            lock_release(&lock);
            return -1;
        }
    } else { /* 새로 파일을 open 하는 경우 */
        f = filesys_open(file_name);

        if (f == NULL) { // file 오픈 실패
            lock_release(&lock);
            return -1;
        }
    }

    fd = new_fd(curr, f);
    if (fd == -1) {
        file_close(f);
        lock_release(&lock);
        exit(-1);
    } else
        curr->fdte[fd] = f; // file descriptor table entry 생성

    lock_release(&lock);

    return fd;
}

static void close(int fd)
{
    check_valid_fd(fd);

    struct thread* curr = thread_current();

    lock_acquire(&lock);
    file_close(curr->fdte[fd]); // open_cnt 보고 inode 제거
    lock_release(&lock);

    curr->fdte[fd] = NULL; // remove fdte
}

/**
 * Implement user memory access
 * Check allocated-ptr / kernel-memory-ptr / partially-valid-ptr
 *
 * Args: 검증하고자 하는 주소 값만 인자로 전달 (only call-by-ref arg)
 *
 * Code Segment 시작주소
 * See: lib/user/user.Ids:7-13
 * See: Makefile.userprog:9
 * See: userprog/process.c:445-468
 */
static void check_valid_ptr(int count, ...)
{
    va_list ptr_ap;
    va_start(ptr_ap, count);

    for (int i = 0; i < count; i++) {
        void* ptr = va_arg(ptr_ap, void*);

        // Check NULL
        if (ptr == NULL) {
            va_end(ptr_ap);
            exit(-1);
        }

        // Check user segment
        if ((uint64_t)ptr < CODE_SEGMENT || (uint64_t)ptr >= USER_STACK) {
            va_end(ptr_ap);
            exit(-1);
        }

        // Check memory allocated
        if (pml4_get_page(thread_current()->pml4, ptr) == NULL) {
            va_end(ptr_ap);
            exit(-1);
        }
    }

    va_end(ptr_ap);
}

static int read(int fd, void* buffer, unsigned size)
{
    check_valid_ptr(1, buffer);

    check_valid_fd(fd);

    struct thread* t = thread_current();
    struct file* f = t->fdte[fd];

    lock_acquire(&lock);
    int byte_read = file_read(f, buffer, size);
    lock_release(&lock);

    return byte_read;
}

static int filesize(int fd)
{
    check_valid_fd(fd);

    struct thread* t = thread_current();
    struct file* f = t->fdte[fd];
    lock_acquire(&lock);
    size_t size = file_length(f);
    lock_release(&lock);
    return size;
}

static void seek(int fd, unsigned position)
{
    check_valid_fd(fd);

    struct thread* t = thread_current();
    struct file* f = t->fdte[fd];

    lock_acquire(&lock);
    file_seek(f, position);
    lock_release(&lock);
}

static void check_valid_fd(int fd)
{
    if (fd < MIN_FD || fd > MAX_FD)
        exit(-1);

    if (thread_current()->fdte[fd] == NULL)
        exit(-1);
}
