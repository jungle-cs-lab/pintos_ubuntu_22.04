#include "userprog/process.h"
#include "threads/malloc.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "include/devices/timer.h"
#include "userprog/syscall.h"

#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char* file_name, struct intr_frame* if_);
static void initd(void* aux);
static void __do_fork(void*);
static struct child_thread* get_child(tid_t child_tid);
static struct child_thread* child_create(void);
static void parse_thread_name(const char* cmdline, char name[16]);
int new_fd(struct thread* t, struct file* f);

/* General process initializer for initd and other process. */
static void process_init(void)
{
    struct thread* current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char* file_name)
{
    char* fn_copy = NULL;
    struct initd_aux* aux = NULL;
    struct child_thread* child = NULL;
    tid_t tid = TID_ERROR;
    char thread_name[16];
    struct thread* parent = thread_current();

    /* 자식 생성 상태 */
    child = child_create();
    if (child == NULL)
        goto error;

    aux = palloc_get_page(PAL_ZERO);
    if (aux == NULL)
        goto error;

    /* Make a copy of FILE_NAME.
     * Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page(0);
    if (fn_copy == NULL)
        goto error;
    strlcpy(fn_copy, file_name, PGSIZE);

    parse_thread_name(fn_copy, thread_name);

    aux->file_name = fn_copy;
    aux->child = child;
    aux->parent = parent;

    /* Create a new thread to execute FILE_NAME. */
    tid = thread_create(thread_name, PRI_DEFAULT, initd, aux);
    if (tid == TID_ERROR)
        goto error;

    child->tid = tid;
    list_push_back(&parent->children, &child->elem);
    return tid;

error:
    if (fn_copy != NULL)
        palloc_free_page(fn_copy);
    if (child != NULL)
        palloc_free_page(child);
    if (aux != NULL)
        palloc_free_page(aux);
    return TID_ERROR;
}

/* A thread function that launches first user process. */
static void initd(void* aux)
{
    struct initd_aux* initd_aux = (struct initd_aux*)aux;
    char* f_name = initd_aux->file_name;

#ifdef VM
    supplemental_page_table_init(&thread_current()->spt);
#endif

    thread_current()->parent = initd_aux->parent;
    thread_current()->self_metadata = initd_aux->child;

    palloc_free_page(initd_aux);

    process_init();

    if (process_exec(f_name) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char* name, struct intr_frame* if_ UNUSED)
{
    struct thread* current = thread_current();
    struct child_thread* child = NULL;
    child = child_create();

    struct fork_aux aux;
    aux.parent = current;
    aux.if_parent = if_;
    aux.ch = child;

    sema_init(&aux.loaded, 0);

    /* Clone current thread to new thread.*/
    tid_t child_tid = thread_create(name, PRI_DEFAULT, __do_fork, &aux);

    if (child_tid == TID_ERROR) {
        palloc_free_page(child);
        return TID_ERROR;
    }

    /* 부모에 자식 등록 */
    list_push_back(&current->children, &child->elem);

    sema_down(&aux.loaded);

    if (aux.success != 1) {
        list_remove(&child->elem);
        palloc_free_page(child);
        return TID_ERROR;
    }

    return child_tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool duplicate_pte(uint64_t* pte, void* va, void* aux)
{
    struct thread* current = thread_current();
    struct thread* parent = (struct thread*)aux;
    void* parent_page;
    void* newpage;
    bool writable;

    /* 1. If the parent_page is kernel page, then return immediately. */
    if (va < CODE_SEGMENT || va >= USER_STACK)
        return true; /* 부모의 유저 공간만 복사하므로, 복사 대상이 아니면 건너뛰고 진행. */

    /* 2. Resolve VA from the parent's page map level 4. */
    parent_page = pml4_get_page(parent->pml4, va);
    if (parent_page == NULL)
        return false;

    /* 3. Allocate new PAL_USER page for the child and set result to NEWPAGE. */
    newpage = palloc_get_page(PAL_USER);
    if (newpage == NULL)
        return false;

    /* 4. Duplicate parent's page to the new page and check whether parent's page is writable or not
     *    (set WRITABLE according to the result). */
    memcpy(newpage, parent_page, PGSIZE);
    writable = is_writable(pte);

    /* 5. Add new page to child's page table at address VA with WRITABLE permission. */
    if (!pml4_set_page(current->pml4, va, newpage, writable)) {
        /* 6. if fail to insert page, do error handling. */
        palloc_free_page(newpage);
        return false;
    }
    return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void __do_fork(void* aux)
{
    struct intr_frame if_;
    struct fork_aux* f_aux = (struct fork_aux*)aux;
    struct thread* parent = f_aux->parent;
    struct thread* current = thread_current();
    struct intr_frame* parent_if = f_aux->if_parent;
    bool succ = true;

    /* 1. Read the cpu context to local stack. */
    memcpy(&if_, parent_if, sizeof(struct intr_frame));

    /* Save parent/child linkage for wait(). */
    current->parent = parent;
    current->self_metadata = f_aux->ch;

    /* 2. Duplicate PT */
    current->pml4 = pml4_create();
    if (current->pml4 == NULL)
        goto error;

    process_activate(current);
#ifdef VM
    supplemental_page_table_init(&current->spt);
    if (!supplemental_page_table_copy(&current->spt, &parent->spt))
        goto error;
#else
    if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
        goto error;
#endif
    /* 파일 복사 */
    lock_acquire(&filesys_lock);
    if (parent->execute_file != NULL) {
        struct file* dup_execute_file = file_duplicate(parent->execute_file);
        if (dup_execute_file == NULL) {
            lock_release(&filesys_lock);
            goto error;
        }
        current->execute_file = dup_execute_file;
    }

    for (int i = MIN_FD; i <= MAX_FD; i++) { // fdte 전수 조사
        if (parent->fdte[i] != NULL) {
            struct file* f = file_duplicate(parent->fdte[i]);
            if (f == NULL) {
                lock_release(&filesys_lock);
                goto error;
            }

            current->fdte[i] = f;
        }
    }
    lock_release(&filesys_lock);

    /* 자식 상태 설정 */
    f_aux->ch->tid = current->tid;
    f_aux->ch->status = current->status;
    f_aux->ch->exit_status = current->exit_status;

    if_.R.rax = 0; /* 자식 프로세스 반환 값 */

    process_init();

    /* Finally, switch to the newly created process. */
    if (succ) {
        f_aux->success = true;
        sema_up(&f_aux->loaded);
        do_iret(&if_);
    }
error:
    f_aux->success = false;
    current->exit_status = EXIT_KERNEL;
    f_aux->ch->exit_status = EXIT_KERNEL;
    sema_up(&f_aux->loaded);
    thread_exit();
}

void arg_passing(void* f_line, struct intr_frame* _if)
{
    char* rsp = (char*)_if->rsp; // explicit cast necessary because rsp is uintptr_t

    char* save_ptr; // used for strtok_r
    char* token;
    char* argv_addr[64]; // 128 is max arg length ==> 64 max args considering space in between each arg composed of
                         // single characters
    int argc = 0;        // counting number of args

    // parse and push arguments into stack at the same time
    // simultaneously add argv_address within stack
    for (token = strtok_r(f_line, " ", &save_ptr); token != NULL; token = strtok_r(NULL, " ", &save_ptr)) {
        if (*token == '\0')
            continue;
        int len = strlen(token) + 1;
        rsp -= len;
        memcpy(rsp, token, len);
        argv_addr[argc++] = rsp;
    }

    // inserting padding if needed after finishing pushing in strings
    // no need to add 0 since when page_get_alloc, stack is zero-filled.
    while ((uintptr_t)rsp % 8 != 0)
        rsp--;

    // insert sentinel argv[argc] = NULL;
    rsp -= 8;

    // add in argv_address into rsp
    for (int i = argc - 1; i >= 0; i--) {
        rsp -= 8;                    // since addresses are 8 bytes
        *(char**)rsp = argv_addr[i]; // argv_addr[i] is pointer to pointer of string rsp should be a pointer to that.
    }

    // insert fake address
    rsp -= 8;

    // set register values
    _if->R.rdi = argc;               // argc
    _if->R.rsi = (uintptr_t)rsp + 8; // addr to argv[0]
    _if->rsp = (uintptr_t)rsp;       // update top of stack
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void* f_name)
{
    char* file_name = f_name;
    bool success;
    struct thread* curr = thread_current();

    /* We cannot use the intr_frame in the thread structure.
     * This is because when current thread rescheduled,
     * it stores the execution information to the member. */
    struct intr_frame _if;
    _if.ds = _if.es = _if.ss = SEL_UDSEG;
    _if.cs = SEL_UCSEG;
    _if.eflags = FLAG_IF | FLAG_MBS;

    /* We first kill the current context */
    lock_acquire(&filesys_lock);
    if (curr->execute_file != NULL) {
        file_close(curr->execute_file);
        curr->execute_file = NULL;
    }
    lock_release(&filesys_lock);
    process_cleanup();

    /* file name parsing logic necessary before passing on to load*/
    /* same logic from parsing thread_name in process_create_initd */
    char load_name[15];                   // load_name is max 14 bytes, +1 for null terminator
    size_t len = strcspn(file_name, " "); // returns length of initial segment up til rejected character
    if (len >= sizeof(load_name))         // logic to prevent buffer over flow
        len = sizeof(load_name) - 1;

    memcpy(load_name, file_name, len); // copy name
    load_name[len] = '\0';             // implement null termination
    /* And then load the binary */
    lock_acquire(&filesys_lock);
    success = load(load_name, &_if);
    lock_release(&filesys_lock);
    /* If load failed, quit. */
    if (!success)
        return -1;

    arg_passing(file_name, &_if);

    // move palloc_free_page after arg_passing()
    palloc_free_page(file_name);

    /* Start switched process. */
    do_iret(&_if);
    NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid UNUSED)
{
    struct child_thread* child = get_child(child_tid);

    /* TID가 잘못되었거나, 호출한 프로세스의 자식이 아닌 경우 */
    if (child == NULL)
        return -1;

    /* 혹은 주어진 TID에 대해 process_wait()이 이미 성공적으로 호출된 적이 있는 경우 */
    if (child->waited == 1)
        return -1;

    child->waited = 1;            /* wait 기록 */
    sema_down(&child->wait_sema); /* 자식 완료까지 부모 프로세스 대기 */

    enum thread_exit_status exit_status = child->exit_status;

    list_remove(&child->elem);
    palloc_free_page(child);

    return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
    struct thread* curr = thread_current();

// using preprocessor directives (전처리기 지시문) ==> 코드를 조건부로 컴파일.
#ifdef USERPROG             // only if user program
    if (curr->pml4 != NULL) // means thread running user code (kernel thread does not have user memory space pml4)
        printf("%s: exit(%d)\n", curr->name, curr->exit_status);
    if (curr->self_metadata != NULL) {
        curr->self_metadata->exit_status = curr->exit_status;
        sema_up(&curr->self_metadata->wait_sema);
    }

#endif
    lock_acquire(&filesys_lock);
    for (int i = MIN_FD; i <= MAX_FD; i++) {
        if (curr->fdte[i] != NULL) {
            file_close(curr->fdte[i]);
            curr->fdte[i] = NULL;
        }
    }
    if (curr->execute_file != NULL) {
        file_close(curr->execute_file);
    }
    lock_release(&filesys_lock);

    process_cleanup();
}

/* Free the current process's resources. */
static void process_cleanup(void)
{
    struct thread* curr = thread_current();

#ifdef VM
    supplemental_page_table_kill(&curr->spt);
#endif

    uint64_t* pml4;
    /* Destroy the current process's page directory and switch back
     * to the kernel-only page directory. */
    pml4 = curr->pml4;
    if (pml4 != NULL) {
        /* Correct ordering here is crucial.  We must set
         * cur->pagedir to NULL before switching page directories,
         * so that a timer interrupt can't switch back to the
         * process page directory.  We must activate the base page
         * directory before destroying the process's page
         * directory, or our active page directory will be one
         * that's been freed (and cleared). */
        curr->pml4 = NULL;
        pml4_activate(NULL);
        pml4_destroy(pml4);
    }
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread* next)
{
    /* Activate thread's page tables. */
    pml4_activate(next->pml4);

    /* Set thread's kernel stack for use in processing interrupts. */
    tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct ELF64_PHDR {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame* if_);
static bool validate_segment(const struct Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool load(const char* file_name, struct intr_frame* if_)
{
    struct thread* t = thread_current();
    struct ELF ehdr;
    struct file* file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    /* Allocate and activate page directory. */
    t->pml4 = pml4_create();
    if (t->pml4 == NULL)
        goto done;
    process_activate(thread_current());

    /* Open executable file. */
    file = filesys_open(file_name);
    if (file == NULL) {
        printf("load: %s: open failed\n", file_name);
        goto done;
    }

    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) ||
        ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
        || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024) {
        printf("load: %s: error loading executable\n", file_name);
        goto done;
    }

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file))
            goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type) {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
            /* Ignore this segment. */
            break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
            goto done;
        case PT_LOAD:
            if (validate_segment(&phdr, file)) {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint64_t file_page = phdr.p_offset & ~PGMASK;
                uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint64_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0) {
                    /* Normal segment.
                     * Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
                } else {
                    /* Entirely zero.
                     * Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                }
                if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
                    goto done;
            } else
                goto done;
            break;
        }
    }

    /* Set up stack. */
    if (!setup_stack(if_))
        goto done;

    /* Start address. */
    if_->rip = ehdr.e_entry;

    file_deny_write(file);
    t->execute_file = file;

    success = true;

done:
    /* We arrive here whether the load is successful or not. */
    return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Phdr* phdr, struct file* file)
{
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
        return false;

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (uint64_t)file_length(file))
        return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false;

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
        return false;

    /* The virtual memory region must both start and end within the
       user address space range. */
    if (!is_user_vaddr((void*)phdr->p_vaddr))
        return false;
    if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
        return false;

    /* The region cannot "wrap around" across the kernel virtual
       address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
        return false;

    /* Disallow mapping page 0.
       Not only is it a bad idea to map page 0, but if we allowed
       it then user code that passed a null pointer to system calls
       could quite likely panic the kernel by way of null pointer
       assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE)
        return false;

    /* It's okay. */
    return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void* upage, void* kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    file_seek(file, ofs);
    while (read_bytes > 0 || zero_bytes > 0) {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* Get a page of memory. */
        uint8_t* kpage = palloc_get_page(PAL_USER);
        if (kpage == NULL)
            return false;

        /* Load this page. */
        if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
            palloc_free_page(kpage);
            return false;
        }
        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page(upage, kpage, writable)) {
            printf("fail\n");
            palloc_free_page(kpage);
            return false;
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool setup_stack(struct intr_frame* if_)
{
    uint8_t* kpage;
    bool success = false;

    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL) {
        success = install_page(((uint8_t*)USER_STACK) - PGSIZE, kpage, true);
        if (success)
            if_->rsp = USER_STACK;
        else
            palloc_free_page(kpage);
    }
    return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable)
{
    struct thread* t = thread_current();

    /* Verify that there's not already a page at that virtual
     * address, then map our page there. */
    return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool lazy_load_segment(struct page* page, void* aux)
{
    /* TODO: Load the segment from the file */
    /* TODO: This called when the first page fault occurs on address VA. */
    /* TODO: VA is available when calling this function. */

    struct thread* curr = thread_current();
    struct executable_load_aux* load_aux = (struct executable_load_aux*)aux;
    void* kpage = page->frame->kva;

    // struct file* file = load_aux->file;
    struct file* file = curr->execute_file;
    off_t ofs = load_aux->ofs;
    size_t page_read_bytes = load_aux->page_read_bytes;
    size_t page_zero_bytes = load_aux->page_zero_bytes;

    file_seek(file, ofs);

    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
        return false; // page fault handler의 실패 핸들링은 한곳에서 집중적으로 하자

    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    while (read_bytes > 0 || zero_bytes > 0) {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        // struct executable_load_aux aux = {
        //     .ofs = ofs,
        //     .page_read_bytes = page_read_bytes,
        //     .page_zero_bytes = page_zero_bytes,
        // };
        struct executable_load_aux* aux = malloc(PGSIZE);
        aux->ofs = ofs;
        aux->page_read_bytes = page_read_bytes;
        aux->page_zero_bytes = page_zero_bytes;

        if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable, lazy_load_segment, aux))
            return false;

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
        ofs += page_read_bytes;
    }
    return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool setup_stack(struct intr_frame* if_)
{
    printf("setup_stack\n");
    bool success = false;
    void* stack_bottom = (void*)(((uint8_t*)USER_STACK) - PGSIZE);

    /* TODO: Map the stack on stack_bottom and claim the page immediately.
     * TODO: If success, set the rsp accordingly.
     * TODO: You should mark the page is stack. */
    /* TODO: Your code goes here */

    if (vm_alloc_page(VM_ANON, stack_bottom, true))
        success = true;

    vm_claim_page(stack_bottom);

    if_->rsp = USER_STACK;

    return success;
}
#endif /* VM */

static struct child_thread* get_child(tid_t child_tid)
{
    struct thread* curr = thread_current();

    struct list_elem* e;
    for (e = list_begin(&curr->children); e != list_end(&curr->children); e = list_next(e)) {
        struct child_thread* tmp_child = list_entry(e, struct child_thread, elem);
        if (child_tid == tmp_child->tid) {
            return tmp_child;
        }
    }
    return NULL;
}

/* Initialize child thread metadata. */
static struct child_thread* child_create(void)
{
    struct child_thread* child = palloc_get_page(PAL_ZERO);
    if (child == NULL)
        return NULL;

    child->exit_status = EXIT_NORMAL;
    child->status = THREAD_READY;
    child->waited = 0;
    sema_init(&child->wait_sema, 0);
    return child;
}

/* Extract a thread name from the command line (max 15 chars + null). */
static void parse_thread_name(const char* cmdline, char name[16])
{
    size_t len = strcspn(cmdline, " ");
    if (len >= 15)
        len = 15;

    memcpy(name, cmdline, len);
    name[len] = '\0';
}

int new_fd(struct thread* t, struct file* f)
{
    // 3부터 순회 -> 빈 순번 할당
    for (int i = MIN_FD; i <= MAX_FD; i++) {
        if (t->fdte[i] == NULL) {
            return i;
        }
    }
    return -1;
}
