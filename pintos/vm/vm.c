/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	// upage, init, aux가 null일때는 검사하지 않는건가? 어떻게 믿고 사용하는거지?

	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		if (page = malloc(sizeof(*page)) == NULL) {
			goto err;
		}

		uninit_new(page, upage, init, type, aux, NULL); // type으로 어떻게 자동으로 초기화함수를 지정할지 모르겠음.

		/* TODO: Insert the page into the spt. */
		if (!spt_insert_page(spt, page)) {
			goto err;
		}
	}
	return true;
err:
	return false;
}

/* SPT에서 VA에 해당하는 page를 찾아 반환한다.
 * 찾지 못하면 NULL을 반환한다. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page *page = NULL;

	ASSERT (spt != NULL); // spt는 적어도 존재해야 함

	// va가 NULL이거나 커널 영역을 가리키는 경우
	if (va == NULL || is_kernel_vaddr(va)) {
		return page;
	}

	struct page key;
	key.va = pg_round_down(va); // 호출되는곳에 대한 정보가 없어, 여기서 va가 페이지의 시작 주소임을 보장해주어야한다고 판단.

	// key.va 값을 갖는 페이지를 불러오기
	struct hash_elem *hash_e = hash_find(&spt->hash_table, &key.elem);
	if (hash_e != NULL) {
		page = hash_entry(hash_e, struct page, elem);
	}

	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	
	/* 어디서 호출될지 모르므로 최대한 많이 작성 */
	ASSERT (spt != NULL); // spt는 NULL이면 안돼
	ASSERT (page != NULL); // page는 정상적으로 생성되어야 해
	ASSERT (is_user_vaddr(page->va)); // va는 유저 영역을 가리켜야 해
	ASSERT (pg_ofs(page->va) == 0); // 페이지 단위로 정렬되어 있어야 해 (페이지 시작 주소)
			
	int succ = false;

	/* 중복되는 페이지가 없었다면, 성공*/
	if (hash_insert(&spt->hash_table, &page->elem) == NULL) {
		succ = true;
	}
	
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */

	return swap_in (page, frame->kva);
}

/* page의 가상 주소(va)를 기준으로 해시값을 계산한다. */
uint64_t hash_page (const struct hash_elem *e, void *aux) {
    struct page *page = hash_entry(e, struct page, hash_elem);

    return hash_bytes(&page->va, sizeof(page->va));
}

/* 두 page를 가상 주소(va) 기준으로 비교한다. */
bool hash_less (const struct hash_elem *a, const struct hash_elem *b, void *aux) {
    struct page *page_a = hash_entry(a, struct page, hash_elem);
    struct page *page_b = hash_entry(b, struct page, hash_elem);

    return page_a->va < page_b->va;
}

/* 새 프로세스의 SPT를 초기화한다. */
bool
supplemental_page_table_init (struct supplemental_page_table *spt) {
    if (!hash_init (&spt->hash_table, hash_page, hash_less, NULL)) {
        return false;
    }
    return true;
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}
