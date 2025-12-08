/* vm.c: Generic interface for virtual memory objects. */

#include "list.h"
#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <stdio.h>
#include "threads/mmu.h"

struct list frame_list;

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
    list_init(&frame_list);
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
static bool vm_do_claim_page(struct page* page);
static struct frame* vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void* upage, bool writable, vm_initializer* init, void* aux)
{

    // MEMO: 시작은 UNINIT으로 한다며...?
    ASSERT(VM_TYPE(type) != VM_UNINIT)

    struct supplemental_page_table* spt = &thread_current()->spt;

    /* Check wheter the upage is already occupied or not. */

    if (spt_find_page(spt, upage) == NULL) {
        /* TODO: Create the page, fetch the initialier according to the VM type,
         * TODO: and then create "uninit" page struct by calling uninit_new. You
         * TODO: should modify the field after calling the uninit_new. */

        // 1. fetch an appropriate initializer according to the passed vm_type
        bool (*page_initializer)(struct page*, enum vm_type, void*) = NULL;
        switch (type) {
        case VM_ANON:
            page_initializer = anon_initializer;
            break;
        case VM_FILE:
            page_initializer = file_backed_initializer;
            break;
        default:
            printf("soemthing wrong!");
            break;
        }

        struct page* page = malloc(PGSIZE);
        // 2. call uninit_new with it.
        uninit_new(page, upage, init, type, aux, page_initializer);

        // MEMO:: what fields should I modify at the moment that is "after calling the uninit_new" ???
        page->writable = writable;
        if (aux) {
            struct executable_load_aux* aux_ = (struct executable_load_aux*)aux;
            page->elf.ofs = aux_->ofs;
            page->elf.page_read_bytes = aux_->page_read_bytes;
            page->elf.page_zero_bytes = aux_->page_zero_bytes;
        }

        /* TODO: Insert the page into the spt. */
        spt_insert_page(spt, page);

        // 성공적으로 page를 alloc한다면
        return true;
    }

err:
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt, void* va)
{
    struct page* page = NULL;
    struct list_elem* page_elem;

    printf("finding page in spt, accessing: %p\n", va);
    for (page_elem = list_begin(&spt->pages); page_elem != list_end(&spt->pages); page_elem = list_next(page_elem)) {
        page = list_entry(page_elem, struct page, elem);

        if (pg_round_down(va) == page->va)
            return page;
    }

    printf("page not found from spt, list iter end, returning NULL\n");
    return NULL;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table* spt, struct page* page)
{
    int succ = false;

    printf("inserting: %p\n", page->va);
    list_push_back(&spt->pages, &page->elem);
    size_t sz = list_size(&spt->pages);
    printf("after inserting: curr list size: %zu\n", sz);

    // validataion
    // (if validated)
    succ = true;

    return succ;
}

bool spt_remove_page(struct supplemental_page_table* spt, struct page* page)
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
    struct frame* frame = malloc(sizeof(struct frame));

    list_push_back(&frame_list, &frame->elem);

    frame->page = NULL;
    frame->kva = palloc_get_page(PAL_USER);
    if (frame->kva == NULL) // eviction needed
        if ((frame->kva = vm_evict_frame()->kva) == NULL)
            PANIC("Eviction needed, but fail.");

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
bool vm_try_handle_fault(struct intr_frame* f UNUSED, void* addr UNUSED, bool user UNUSED, bool write UNUSED,
                         bool not_present UNUSED)
{
    struct supplemental_page_table* spt UNUSED = &thread_current()->spt;
    struct page* page = NULL;
    /* TODO: Validate the fault */
    /* TODO: Your code goes here */

    // spt에서 accessing va에 맞는 page를 찾아서 전달해준다.
    printf("\tPAGE FAULT! (accessing: %p)\n", addr);
    page = spt_find_page(spt, addr);

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
bool vm_claim_page(void* va UNUSED)
{
    struct page* page = NULL;
    /* TODO: Fill this function */

    // gitbook: You will first need to get a page and then calls vm_do_claim_page with the page.
    struct supplemental_page_table* spt UNUSED = &thread_current()->spt;
    printf("\tspt_find_page: from vm_claim_page\n");
    page = spt_find_page(spt, va);

    return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page(struct page* page)
{

    struct frame* frame = vm_get_frame();

    /* Set links */
    frame->page = page;
    page->frame = frame;

    /* TODO: Insert page table entry to map page's VA to frame's PA. */
    pml4_set_page(thread_current()->pml4, pg_round_down(page->va), frame->kva, page->writable);

    return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table* spt UNUSED)
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
