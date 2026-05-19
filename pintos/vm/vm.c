/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "vm/uninit.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
struct list frame_table;
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
	list_init(&frame_table);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type);
	default:
		return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
static uint64_t page_hash (const struct hash_elem *e, void *aux UNUSED);
static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);
static void page_destroy (struct hash_elem *e, void *aux UNUSED);
static bool is_certified_stackgrowth (void); // 정의 안되어있어서 우선 stub 처리 - 추후 구현 필요

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux)
{

	ASSERT(VM_TYPE(type) != VM_UNINIT)

	// upage, init, aux가 null일때는 검사하지 않는건가? 어떻게 믿고 사용하는거지?

	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page;
	bool (*initializer) (struct page *, enum vm_type, void *) = NULL;

	switch (VM_TYPE (type)) {
	case VM_ANON:
		initializer = anon_initializer;
		break;
	case VM_FILE:
		initializer = file_backed_initializer;
		break;
	}

	/* Check wheter the upage is already occupied or not. */
	// 현재는 spt를 채우는 과정. spt에 해당 페이지가 없어야지 새로 생성가능
	if (spt_find_page(spt, upage) == NULL)
	{
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		page = malloc(sizeof(*page));
		if (page == NULL) {
			goto err;
		}

		/* uninit_new()는 페이지를 uninit 상태로 등록하고,
		 * 첫 page fault 때 initializer와 init(aux)를 호출해 실제 페이지로 전환한다. */
		uninit_new(page, upage, init, type, aux, initializer);

		/* 아래 값들은 struct page 공통 메타데이터다.
		 * uninit_new()가 *page 전체를 초기화하므로 호출 후에 설정한다.
		 * uninit_new()의 인자는 바꾸지 않아 uninit_new() 함수의 책임을 유지한다. */
		page->writable = writable;
		page->owner = thread_current ();
		page->accessed = false;

		/* TODO: Insert the page into the spt. */
		if (!spt_insert_page(spt, page)) {
			goto err;
		}
	}
	else
		return false;
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
		return NULL;
	}

	struct page key;
	key.va = pg_round_down(va); // 호출되는곳에 대한 정보가 없어, 여기서 va가 페이지의 시작 주소임을 보장해주어야한다고 판단.

	// key.va 값을 갖는 페이지를 불러오기
	struct hash_elem *hash_e = hash_find(&spt->page_hash, &key.hash_elem);
	if (hash_e != NULL) {
		page = hash_entry(hash_e, struct page, hash_elem);
	}

	return page;

}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt,
					 struct page *page)
{
	/* 어디서 호출될지 모르므로 최대한 많이 작성 */
	ASSERT (spt != NULL); // spt는 NULL이면 안돼
	ASSERT (page != NULL); // page는 정상적으로 생성되어야 해
	ASSERT (is_user_vaddr(page->va)); // va는 유저 영역을 가리켜야 해
	ASSERT (pg_ofs(page->va) == 0); // 페이지 단위로 정렬되어 있어야 해 (페이지 시작 주소)
	int succ = false;
	/* TODO: Fill this function. */
	// spt에 넣는 함수 = hash에 넣는 함수
	if (hash_insert(&spt->page_hash, &page->hash_elem) == NULL) {
		succ = true;
	}
	
	return succ;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	vm_dealloc_page(page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim(void)
{
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame(void)
{
	struct frame *victim UNUSED = vm_get_victim();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame(void)
{ /* TODO: Fill this function. */
	struct frame *frame = malloc(sizeof(frame));

	// palloc으로 프레임을 가져오는 것을 시도
	frame->kva = palloc_get_page(PAL_USER);
	frame->page = NULL;

	// palloc이 실패했다 == 메모리가 꽉 찼다 == swap out 필요
	if (frame->kva == NULL)
	{
		// 메모리 swap out 이후에 구현
		PANIC("todo");
	}
	ASSERT(frame != NULL);
	ASSERT(frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth(void *addr UNUSED)
{
}

static bool
is_certified_stackgrowth(void)
{
	/* 정의 안되어있어서 우선 stub 처리 - 추후 구현 필요 */
	return true;
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp(struct page *page UNUSED)
{
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f, void *addr,
						 bool user, bool write, bool not_present)
{
	struct supplemental_page_table *spt = &thread_current()->spt;
	// struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	// PT에 매핑이 있었던 경우 -> 권한 문제 때문에 실패
	if (not_present == false)
		return false;

	// PT에 매핑이 없었던 경우 -> SPT에서 확인해보기
	else
	{
		// spt에서 찾아보기
		struct page *found_page = spt_find_page(spt, addr);

		// SPT에서 페이지 정보를 찾을 수 있는 경우
		if (found_page != NULL)
		{
			// page 구조체의 권한을 확인 -> 에러
			if (found_page->writable != write)
				return false;
			// spt의 page 내용대로 Frame 요청
			return vm_do_claim_page(found_page);
		}

		// SPT에서 페이지 정보를 찾을 수 없는 경우
		else // found_page == NULL
		{
			// stack growth 조건을 확인
			if (!is_certified_stackgrowth())
				return false;
			// user인지 확인
			if (user != true)
				return false;
			// stack growth 실행
			vm_stack_growth(addr);
		}
	}
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va)
{
	/* TODO: Fill this function */
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct page *page = spt_find_page(spt, va);

	if(page == NULL)
	{
		return false;
	}

	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page(struct page *page)
{
	struct frame *frame = vm_get_frame();
	struct thread *curr = thread_current();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	pml4_set_page(curr->pml4, page->va, frame->kva, page->writable);

	// 매크로 함수였어
	return swap_in(page, frame->kva);
}

/* page의 가상 주소(va)를 기준으로 해시값을 계산한다. */
uint64_t page_hash (const struct hash_elem *e, void *aux) {
    struct page *page = hash_entry(e, struct page, hash_elem);

    return hash_bytes(&page->va, sizeof(page->va));
}

/* 두 page를 가상 주소(va) 기준으로 비교한다. */
bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux) {
    struct page *page_a = hash_entry(a, struct page, hash_elem);
    struct page *page_b = hash_entry(b, struct page, hash_elem);

    return page_a->va < page_b->va;
}

/* 새 프로세스의 SPT를 초기화한다. */
bool
supplemental_page_table_init (struct supplemental_page_table *spt) {
    if (!hash_init (&spt->page_hash, page_hash, page_less, NULL)) {
        return false;
    }
    return true;
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
								  struct supplemental_page_table *src UNUSED)
{
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_destroy (&spt->page_hash, page_destroy);
}

static void
page_destroy (struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry (e, struct page, hash_elem);

	vm_dealloc_page (page);
}
