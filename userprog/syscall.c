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

#define MAX_CHUNK 256 /* 콘솔 출력 청크 사이즈 */

static struct lock lock;

void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame* f UNUSED)
{
    register uint64_t rax = f->R.rax;
    register uint64_t arg1 = f->R.rdi;
    register uint64_t arg2 = f->R.rsi;
    register uint64_t arg3 = f->R.rdx;
    register uint64_t arg4 = f->R.r10;
    register uint64_t arg5 = f->R.r8;
    register uint64_t arg6 = f->R.r9;

    struct thread* curr = thread_current();

    lock_init(&lock);

    switch (rax) {
    case SYS_HALT:
        power_off();
        break;

    case SYS_EXIT:
        curr->tf.R.rax = arg1;
        break;

    case SYS_WRITE:
        if (arg1 == 1) {
            lock_acquire(&lock); // race condition 방지

            size_t size = arg3;
            char* buf = (char*)arg2;
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
        } else {
            // TODO: file write 구현
        }
        break;

    default:
        break;
    }

    thread_exit();
}
