/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "userprog/process.h"
#include <bitmap.h>
#include "threads/vaddr.h"
#include "threads/mmu.h"

static struct frame* frames;
static struct bitmap* frame_table;
static struct lock frame_lock;
static void init_frame_table();
void cleanup_frame_table();

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

    init_frame_table();
}

static void init_frame_table()
{
    size_t frame_count = user_pool_pages();
    frame_table = bitmap_create(frame_count);
    frames = malloc(sizeof *frames * frame_count);
    for (size_t i = 0; i < frame_count; i++) {
        frames[i].kva = NULL;
        frames[i].page = NULL;
    }
    lock_init(&frame_lock);
}

void cleanup_frame_table()
{
    free(frames);
    bitmap_destroy(frame_table);
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
static uint64_t spt_hash(const struct hash_elem* e, void* aux);
static bool spt_hash_less(const struct hash_elem* a, const struct hash_elem* b, void* aux);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void* upage, bool writable, vm_initializer* init, void* aux)
{

    ASSERT(VM_TYPE(type) != VM_UNINIT);
    ASSERT(is_user_vaddr(upage));

    struct supplemental_page_table* spt = &thread_current()->spt;

    /* Check wheter the upage is already occupied or not. */
    if (spt_find_page(spt, upage) == NULL) {

        struct page* new_page = (struct page*)malloc(sizeof(struct page));
        if (new_page == NULL) {
            PANIC("page alloc failed.");
            return false;
        }

        bool (*initializer)(struct page*, enum vm_type, void*);

        switch (VM_TYPE(type)) {
        case VM_ANON:
            initializer = anon_initializer;
            break;
        case VM_FILE:
            initializer = file_backed_initializer;
            break;
        default:
            free(new_page);
            return false;
        }

        uninit_new(new_page, upage, init, type, aux, initializer);
        new_page->writable = writable;

        return spt_insert_page(spt, new_page);
    }
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt, void* va)
{
    struct page* page = NULL;
    void* target = pg_round_down(va);
    struct hash_iterator i;

    // REF: lib/kernel/hash.c, 166:175
    hash_first(&i, &spt->pages);
    while (hash_next(&i)) {
        page = hash_entry(hash_cur(&i), struct page, elem);
        if (page->va == target)
            return page;
    }

    return NULL;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table* spt, struct page* page)
{
    ASSERT(page != NULL);
    ASSERT(page->va != NULL);
    hash_insert(&spt->pages, &page->elem);
    // TODO:: `hash_insert` 함수는 동일한 element가 이미 존재하면 기존 element를 리턴한다.
    // 이 경우에 대한 예외처리가 필요하다.

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
    struct frame* frame;

    lock_acquire(&frame_lock);

    size_t free_frame_index = bitmap_scan_and_flip(frame_table, 0, 1, false); /* 가용 프레임 체크 */
    if (free_frame_index >= 0 && free_frame_index < user_pool_pages()) {
        frame = &frames[free_frame_index];
    } else {
        // 없으면, evict 진행후 프레임 바인딩
        PANIC("no available frame");
    }

    void* upage = palloc_get_page(PAL_USER);
    if (upage == NULL) {
        free(frame);
        PANIC("palloc frame failed.");
    }

    frame->kva = upage;
    lock_release(&frame_lock);

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
    struct supplemental_page_table* spt = &thread_current()->spt;
    struct page* page = NULL;

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
static bool vm_do_claim_page(struct page* page)
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

    /* Insert page table entry to map page's VA to frame's PA. */
    if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, true))
        PANIC("pml4_set_page failed");

    return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table* spt)
{
    hash_init(&spt->pages, spt_hash, spt_hash_less, NULL);
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

/* 구조체의 멤버를 이용해서 같은 버킷에 저장하도록 하는 장치 */
static uint64_t spt_hash(const struct hash_elem* e, void* aux)
{
    const struct page* page = hash_entry(e, struct page, elem);

    return hash_bytes(&page->va, sizeof page->va);
}

/* 해시 값이 충돌했을 때, 버킷 내에서 같은 키인지 다른 키인지 가려주는 장치 */
static bool spt_hash_less(const struct hash_elem* a, const struct hash_elem* b, void* aux)
{
    const struct page* page_a = hash_entry(a, struct page, elem);
    const struct page* page_b = hash_entry(b, struct page, elem);

    return &page_a->va < &page_b->va;
}
