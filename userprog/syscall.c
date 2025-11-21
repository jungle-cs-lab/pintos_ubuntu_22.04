#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/synch.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame*);

#define MAX_CHUNK 256                      /* 콘솔 출력 청크 사이즈 */
#define CODE_SEGMENT ((uint64_t)0x0400000) /* 코드 세그먼트 시작 주소 */

static struct lock lock;
static void exit(int status);
static int create(char* file_name, int initial_size);
static int write(int fd, const void* buffer, unsigned size);
static void check_valid_ptr(int count, ...);
static int open(const char* file);
static void close(int fd);
static int read(int fd, void* buffer, unsigned size);
static int32_t filesize(int fd);
static struct file_descriptor* find_fd(int fd);

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
}

/* The main system call interface */
void syscall_handler(struct intr_frame* f UNUSED)
{
    int syscall_num = f->R.rax;
    uint64_t rax = f->R.rax;
    uint64_t arg1 = f->R.rdi;
    uint64_t arg2 = f->R.rsi;
    uint64_t arg3 = f->R.rdx;
    uint64_t arg4 = f->R.r10;
    uint64_t arg5 = f->R.r8;
    uint64_t arg6 = f->R.r9;

    switch (syscall_num) {

    case SYS_EXIT:
        exit(arg1);
        break;

    case SYS_CREATE:
        f->R.rax = create(arg1, arg2);
        break;

    case SYS_WRITE:
        f->R.rax = write(arg1, arg2, arg3);
        break;
    case SYS_OPEN:
        f->R.rax = open(arg1);
        break;
    case SYS_CLOSE:
        close(arg1);
        break;
    case SYS_FILESIZE:
        f->R.rax = filesize(arg1);
        break;
    case SYS_READ:
        f->R.rax = read(arg1, arg2, arg3);
        break;
    default:
        thread_exit();
    }
}

static void exit(int status)
{
    struct thread* t = thread_current();
    t->exit_status = status;
    thread_exit();
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

    lock_acquire(&lock); // race condition 방지
    char* buf = (char*)buffer;

    if (size <= MAX_CHUNK) {
        putbuf(buf, size);
    } else { // 256 이상은 분할 출력
        size_t offset = 0;
        while (offset < size) {
            size_t chunk_size = size - offset < MAX_CHUNK ? size - offset : MAX_CHUNK;
            putbuf(buf + offset, chunk_size);
            offset += chunk_size;
        }
    }
    lock_release(&lock);

    return size;
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
        uint64_t ptr = va_arg(ptr_ap, uint64_t);

        // Check NULL
        if (ptr == NULL) {
            va_end(ptr_ap);
            exit(-1);
        }

        // Check user segment
        if (ptr < CODE_SEGMENT || ptr >= USER_STACK) {
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


static int open(const char* file)
{
    check_valid_ptr(1,file);
    lock_acquire(&lock); // lock before opening file

    struct file* open_file = filesys_open(file);

    if (open_file == NULL) // failure of opening file
    {
        lock_release(&lock);
        return -1;
    }

    struct thread* t = thread_current();
    struct file_descriptor* fd = malloc(sizeof(struct file_descriptor));

    fd->fd_file = file;
    fd->fd_val = t->min_fd;
    t->min_fd++;

    list_push_back(&t->files_opened, &fd->fd_elem);
    lock_release(&lock); // release lock after fd is pushed in

    return fd->fd_val; // return fd_val
}

static void close(int fd)
{
    struct file_descriptor* real_fd = find_fd(fd);
    struct thread* t = thread_current();

    lock_acquire(&lock); // acquire lock before handling file

    if (real_fd == NULL) {
        lock_release(&lock);
        return;
    }

    if (real_fd->fd_val <
        t->min_fd) //  update min_fd of thread so that it can give lowest available fd for next file opened
        t->min_fd = real_fd->fd_val;

    file_close(real_fd->fd_file);
    list_remove(&real_fd->fd_elem); // don't forget to remove from list
    free(real_fd);                  // also to free the file descriptor memory allocated by malloc when opened
    lock_release(&lock);            // also to release lock
    return;
}

static int read(int fd, void* buffer, unsigned size)
{

    check_valid_ptr(1,buffer);

    lock_acquire(&lock); // acquire lock before handling file // but since read can be accessed by multiple processes,
                         // maybe different lock?

    if (fd == 0) { // read from keyboard(STDOUT)
        lock_release(&lock);
        return input_getc();
    }

    struct file_descriptor* real_fd = find_fd(fd);

    if (real_fd == NULL) {
        lock_release(&lock);
        return;
    }

    struct file* file = real_fd->fd_file;

    int32_t bytes_read =  file_read(file, buffer, size);

    lock_release(&lock); // also to release lock
    return bytes_read;
}

static int32_t filesize(int fd)
{
    struct file_descriptor* real_fd = find_fd(fd);

    lock_acquire(&lock); // acquire lock before handling file

    if (real_fd == NULL) {
        lock_release(&lock);
        return;
    }

    return file_length(real_fd->fd_file);

    lock_release(&lock);
}

// helper function: returns file_descriptor if fd is given.
static struct file_descriptor* find_fd(int fd)
{
    struct thread* t = thread_current();

    if (list_empty(&t->files_opened)) { // if process has no files opened, failed to find file descriptor
        return NULL;
    }

    struct list_elem* e = NULL;
    // traverse through files_opened of current thread to find matching fd_val
    for (e = list_begin(&t->files_opened); e != list_end(&t->files_opened); e = list_next(e)) {
        struct file_descriptor* fd = list_entry(e, struct file_descriptor, fd_elem);

        if (fd->fd_val == fd)
            return fd; // return pointer of file_descriptor found
    }
    return NULL; // failed to find file descriptor
}