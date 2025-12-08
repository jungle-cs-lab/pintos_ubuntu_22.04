/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <hash.h>
#include <bitmap.h>
#include "threads/vaddr.h"
#include "threads/mmu.h"

static struct frame* frames;
struct bitmap* frame_table;

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
    /* global 테이블 초기화 */
    // frame table
    size_t frame_count = user_pool_pages();
    frame_table = bitmap_create(frame_count);
    frames = malloc(sizeof *frames * frame_count);
    for (size_t i = 0; i < frame_count; i++) {
        frames[i].kva = user_pool_base + i * PGSIZE;
        frames[i].page = NULL;
    }
    // swap table
    // file mapping table
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

    ASSERT(VM_TYPE(type) != VM_UNINIT)

    struct supplemental_page_table* spt = &thread_current()->spt;

    /* Check wheter the upage is already occupied or not. */
    if (spt_find_page(spt, upage) == NULL) {
        /* TODO: Create the page, fetch the initialier according to the VM type,
         * TODO: and then create "uninit" page struct by calling uninit_new. You
         * TODO: should modify the field after calling the uninit_new. */

        /* TODO: Insert the page into the spt. */
    }
err:
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt UNUSED, void* va UNUSED)
{
    struct page* page = NULL;
    /* TODO: Fill this function. */

    return page;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table* spt UNUSED, struct page* page UNUSED)
{
    int succ = false;
    /* TODO: Fill this function. */

    return succ;
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
    struct frame* frame = NULL;
    /* TODO: Fill this function. */

    // 가용 가능한 프레임이 있는지 체크
    size_t free_frame_index = bitmap_scan_and_flip(frame_table, 0, 1, false);
    // 있으면, 프레임 바인딩
    if (free_frame_index >= 0 && free_frame_index < user_pool_pages()) {
        frame = &frames[free_frame_index];
    }
    // 없으면, evict 진행후 프레임 바인딩
    else {
    }

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

    return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table* spt)
{
    /* init */
    hash_init(&spt->hash_table, spt_hash, spt_hash_less, NULL);

    /* init spt entry */
    // spt->origin = ZERO_PAGE;
    // spt->dirty = false;
    // spt->present = false;
    // spt->frame = NULL;
    // spt->initializer = NULL;
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table* dst, struct supplemental_page_table* src)
{
    // TODO: lock

    // src 해시맵을 순회
    struct hash_iterator i;
    hash_first(&i, src);
    while (hash_next(&i)) {
        struct supplemental_page_table_entry* src_spte =
            hash_entry(hash_cur(&i), struct supplemental_page_table_entry, elem);

        /* spte 복제 */
        struct supplemental_page_table_entry* copy_spte;
        copy_spte = malloc(sizeof *copy_spte);
        copy_spte->origin = src_spte->origin;
        copy_spte->dirty = src_spte->dirty;
        copy_spte->present = src_spte->present;
        copy_spte->frame = src_spte->frame;
        copy_spte->initializer = src_spte->initializer;

        hash_insert(dst, &copy_spte->elem);
    }
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table* spt)
{
    // TODO: lock

    // free spte
    struct hash_iterator i;
    hash_first(&i, &spt->hash_table);
    while (hash_next(&i)) {
        struct supplemental_page_table_entry* spte =
            hash_entry(hash_cur(&i), struct supplemental_page_table_entry, elem);
        free(spte);
    }

    // destroy spt
    hash_destroy(&spt->hash_table, NULL);

    // spte의 프레임은 해제 안해도 되겠지?
}

/* 구조체의 멤버를 이용해서 같은 버킷에 저장하도록 하는 장치 */
static uint64_t spt_hash(const struct hash_elem* e, void* aux)
{
    const struct supplemental_page_table_entry* pt = hash_entry(e, struct supplemental_page_table_entry, elem);
    return hash_bytes(&pt->frame,
                      sizeof pt->frame); // FIXME: frame을 아직 할당 받지 않을 수 있기 때문에, 다른 멤버로 변경 필요
}

/* 해시 값이 충돌했을 때, 버킷 내에서 같은 키인지 다른 키인지 가려주는 장치 */
static bool spt_hash_less(const struct hash_elem* a, const struct hash_elem* b, void* aux)
{
    const struct supplemental_page_table_entry* pt_a = hash_entry(a, struct supplemental_page_table_entry, elem);
    const struct supplemental_page_table_entry* pt_b = hash_entry(b, struct supplemental_page_table_entry, elem);
    return &pt_a->frame < &pt_b->frame;
}