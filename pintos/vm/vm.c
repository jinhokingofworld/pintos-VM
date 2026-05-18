/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

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

/* TYPE에 맞는 사용자 가상 페이지를 만들고 현재 스레드의 SPT에 등록한다.
 * 실제 물리 프레임 할당은 지금 하지 않고, 첫 페이지 폴트 때 INIT/AUX로 초기화되도록
 * uninit page 상태로 준비한다. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	struct supplemental_page_table *spt = &thread_current ()->spt;

    if (pg_round_down(upage) != upage) {
        goto err;
    }

	/* 같은 가상 주소의 페이지가 이미 SPT에 있으면 중복 할당이므로 실패한다. */
	if (spt_find_page (spt, upage) != NULL) {
        return false;
    }
	
    /* SPT에 저장할 page 구조체를 동적으로 할당한다. */
    struct page *page = malloc(sizeof(struct page));

    if (page == NULL) {
        goto err;
    }

    /* 페이지 종류에 따라 첫 폴트 시 호출할 실제 initializer를 연결한다. */
    switch(VM_TYPE(type)) {
        case VM_ANON: 
            uninit_new(page, upage, init, type, aux, anon_initializer);
            break;
        case VM_FILE:
            uninit_new(page, upage, init, type, aux, file_backed_initializer);
            break;
        default:
            free(page);
            goto err;
    }
    /* uninit_new()가 page 전체를 초기화한 뒤, 추가 메타데이터를 채운다. */
    page->writable = writable;

    /* 준비된 page를 SPT에 등록하면 이후 페이지 폴트에서 찾아 쓸 수 있다. */
    if (spt_insert_page(spt, page)) {
        return true;
    } else {
        free(page);
        return false;
    }
err:
	return false;
}

/* SPT에서 VA가 속한 가상 페이지를 찾아 반환한다.
 * VA는 페이지 시작 주소로 내림해 해시 키와 맞춘다.
 * 해당 페이지가 없으면 NULL을 반환한다. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
    struct page page;
    struct hash_elem *result_elem;

    /* 임시 page는 hash_find에 넘길 검색 키 역할만 한다. */
    page.va = pg_round_down(va);

    result_elem = hash_find(&spt->pages, &page.hash_elem);
    if (result_elem == NULL) {
        return NULL;
    }
    
    return hash_entry(result_elem, struct page, hash_elem);
}

/* PAGE를 SPT 해시 테이블에 등록한다.
 * page->va가 이미 등록되어 있으면 중복 삽입이므로 false를 반환한다. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
    
    struct hash_elem *insert_elem;

    /* hash_insert는 같은 키가 없으면 NULL, 있으면 기존 원소를 반환한다. */
    insert_elem = hash_insert(&spt->pages, &page->hash_elem);

    if (insert_elem == NULL) {
        return true;
    } else {
        return false;
    }
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
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
    hash_init (&spt->pages, hash_page, hash_less, NULL);
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
