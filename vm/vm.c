/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include <stdio.h>

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
    vm_anon_init();
    vm_file_init();
#ifdef EFILESYS /* For project 4 */
    pagecache_init();
#endif
    register_inspect_intr();
    /* DO NOT MODIFY UPPER LINES. */
    /* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page* page)
{
    int ty = VM_TYPE(page->operations->type);
    switch (ty) {
    case VM_UNINIT:
        return VM_TYPE(page->uninit.type);
    default:
        return ty;
    }
}

/* Helpers */
static struct frame* vm_get_victim(void);
bool vm_do_claim_page(struct page* page);
static struct frame* vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void* upage, bool writable, vm_initializer* init, void* aux)
{

    ASSERT(VM_TYPE(type) != VM_UNINIT)
    ASSERT(is_user_vaddr(upage));

    struct supplemental_page_table* spt = &thread_current()->spt;

    /* Check wheter the upage is already occupied or not. */
    if (spt_find_page(spt, upage) == NULL) {

        struct page* page = (struct page*)malloc(sizeof(struct page));
        if (page == NULL) {
            PANIC("page alloc failed.");
            return false;
        }
        page->va = pg_round_down(upage);
        page->frame = NULL;

        bool (*initializer)(struct page*, enum vm_type, void*);

        switch (VM_TYPE(type)) {
        case VM_ANON:
            initializer = anon_initializer;
            break;
        case VM_FILE:
            initializer = file_backed_initializer;
            break;
        default:
            free(page);
            return false;
        }

        uninit_new(page, upage, init, type, aux, initializer);
        page->writable = writable;
        spt_insert_page(spt, page);

        return true;
    }
    goto err;
err:
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt, void* va)
{
    struct page* page = NULL;
    struct list_elem* elem;

    void* target = pg_round_down(va);

    for (elem = list_begin(&spt->pages); elem != list_end(&spt->pages); elem = list_next(elem)) {
        page = list_entry(elem, struct page, elem);
        if (pg_round_down(page->va) == target)
            return page; // 리스트 순회 해서 va와 매칭시 리턴.
    }

    return NULL;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table* spt, struct page* page)
{
    if (!spt_find_page(spt, page->va)) {
        list_push_back(&spt->pages, &page->elem);
    }

    return true;
}

void spt_remove_page(struct supplemental_page_table* spt, struct page* page)
{
    vm_dealloc_page(page);
    return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame* vm_get_victim(void)
{
    struct frame* victim = NULL;
    /* TODO: The policy for eviction is up to you. */

    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame* vm_evict_frame(void)
{
    struct frame* victim UNUSED = vm_get_victim();
    /* TODO: swap out the victim and return the evicted frame. */

    return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame* vm_get_frame(void)
{
    /* TODO: Fill this function. */

    struct frame* frame = (struct frame*)malloc(sizeof(struct frame));
    if (frame == NULL) {
        PANIC("malloc frame failed.");
    }

    void* page = palloc_get_page(PAL_USER);
    if (page == NULL) {
        free(frame);
        PANIC("palloc frame failed.");
    }

    frame->kva = page;
    frame->page = NULL;

    ASSERT(frame != NULL);
    ASSERT(frame->page == NULL);
    return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void* addr UNUSED)
{
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page* page UNUSED)
{
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame* f, void* addr, bool user, bool write, bool not_present)
{
    struct supplemental_page_table* spt UNUSED = &thread_current()->spt;
    struct page* page = NULL;
    /* TODO: Validate the fault */
    /* TODO: Your code goes here */
    if (addr == NULL) {
        PANIC("addr == NULL");
    }
    if (!is_user_vaddr(addr)) {
        PANIC("not user addr");
    }

    page = spt_find_page(&thread_current()->spt, addr);
    if (page == NULL) {
        PANIC("page null error");
        return false;
    }

    return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page* page)
{
    destroy(page);
    free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void* va)
{
    /* TODO: Fill this function */
    struct thread* t = thread_current();
    if (!(is_user_vaddr(va))) {
        return false;
    }

    struct page* page = spt_find_page(&t->spt, va);
    if (page == NULL) {
        return false;
    }

    if (page->frame != NULL) {
        return true;
    }

    return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
bool vm_do_claim_page(struct page* page)
{
    struct frame* frame = vm_get_frame();
    if (frame == NULL) {
        return false;
    }
    /* Set links */
    frame->page = page;
    page->frame = frame;
    if (!is_user_vaddr(page->va)) {
        PANIC("vm_do_claim_page: page->va is kernel address!");
    }

    /* TODO: Insert page table entry to map page's VA to frame's PA. */
    if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, true))
        PANIC("pml4_set_page failed");

    return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table* spt)
{
    list_init(&spt->pages);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table* dst UNUSED,
                                  struct supplemental_page_table* src UNUSED)
{
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table* spt UNUSED)
{
    /* TODO: Destroy all the supplemental_page_table hold by thread and
     * TODO: writeback all the modified contents to the storage. */
}
