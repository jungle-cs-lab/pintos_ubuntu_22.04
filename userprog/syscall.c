#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

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

    switch (rax) {
    case SYS_WRITE:
        // FIXME: write file은 skip된 상태
        if (arg1 == 1) {
            // FIXME: 만약 arg2(buffer)가 너무 커지면, buffer를 나눠서 putbuf를 호출해야 함
            putbuf(arg2, arg3);
            // TODO: 실제로 출력한 크기를 어떻게 호출하지?
            return sizeof arg2;
        }
        break;

    default:
        break;
    }

    thread_exit();
}
