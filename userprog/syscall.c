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

#define MAX_CHUNK 256 /* 콘솔 출력 청크 사이즈 */

static struct lock lock;
static void exit(int status);
static int write(int fd, const void* buffer, unsigned size);

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
        if (strlen(arg1) > 14) // 14자 제한
            return 0;

        lock_acquire(&lock); // 동시 접근 방지
        int result = filesys_create(arg1, arg2);
        lock_release(&lock);

        return result;
        break;

    case SYS_WRITE:
        f->R.rax = write(arg1, arg2, arg3);
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

int write(int fd, const void* buffer, unsigned size)
{
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