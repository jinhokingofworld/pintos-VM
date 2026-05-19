#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

struct initd_info {
	char *file_name;
	struct child_info *child_info;
};

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
    for (int i = 0; i < FD_MAX; i++) {
        current->fd_table[i] = NULL;
    }
}

/* 첫 사용자 프로그램 initd를 생성하고 실행 준비를 한다.
 * 성공하면 initd의 tid를, 실패하면 TID_ERROR를 반환한다. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	char *name_copy;
	char *save_ptr;
	struct initd_info *info;
	struct child_info *child;

	/* 파일 이름 복사본 생성. */
	fn_copy = palloc_get_page (0);
	name_copy = palloc_get_page(0);

	info = malloc (sizeof *info);
	child = malloc (sizeof *child);

	if (fn_copy == NULL || name_copy == NULL || info == NULL || child == NULL) {
		if (fn_copy != NULL)
			palloc_free_page (fn_copy);
		if (name_copy != NULL)
			palloc_free_page (name_copy);
		if (info != NULL)
			free (info);
		if (child != NULL)
			free (child);
		return TID_ERROR;
	}
	strlcpy (fn_copy, file_name, PGSIZE);
	strlcpy (name_copy, file_name, PGSIZE);

	info->file_name=fn_copy;
	info->child_info=child;

	child->tid = TID_ERROR;
	child->exit_status = -1;
	child->exited = false;
	child->waited = false;
	child->load_success = false;
	sema_init (&child->load_sema, 0);
	child->parent = thread_current ();

	/* initd를 실행할 새 스레드 생성. */
	child->tid = thread_create (strtok_r(name_copy, " ", &save_ptr), PRI_DEFAULT, initd, info);
	if (child->tid == TID_ERROR){
		palloc_free_page (fn_copy);
		palloc_free_page (name_copy);
		free (info);
		free (child);
		return TID_ERROR;
	}
	
	list_push_back (&thread_current ()->children, &child->elem);
	palloc_free_page(name_copy);
	sema_down (&child->load_sema);
	free (info);
	if (!child->load_success) return TID_ERROR;
	
	return child->tid;
	
}

/* 첫 사용자 프로세스를 실행하는 스레드 함수. */
static void
initd (void *initd_info) {
#ifdef VM
	/* 현재 스레드의 보조 페이지 테이블 초기화. */
	supplemental_page_table_init (&thread_current ()->spt);
#endif
	struct initd_info *info = initd_info;
	char *file_name = info->file_name;
	thread_current ()->child_info = info->child_info;
	process_init ();
	
	/* 실행 파일을 현재 프로세스에 적재하고 시작. */
	if (process_exec (file_name) < 0) thread_exit ();
	NOT_REACHED ();
}

struct fork_info {
    struct intr_frame parent_if;
    struct child_info *child_info;
};

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	/* Clone current thread to new thread.*/

	struct child_info *child;
	struct fork_info *fork_info;

	child = malloc (sizeof *child);
	fork_info = malloc (sizeof *fork_info);

	if (child == NULL || fork_info == NULL){
	    return TID_ERROR;
	}

	// child_info 초기화
	child->tid = TID_ERROR;				// 자식 tid 저장용
	child->exit_status = -1;			// 자식 종료 상태 저장용
	child->exited = false;				// 자식 종료 여부
	child->waited = false;				// wait 중복 호출 방지용
	child->load_success = false;		// fork 복제 성공 여부
	sema_init (&child->load_sema, 0);	// fork 복제 완료 대기용
	child->parent = thread_current ();	// 부모 스레드 저장

	// fork_info 초기화 
	memcpy (&fork_info->parent_if, if_, sizeof *if_);	//intr_frame 복제용
	fork_info -> child_info = child;					//성공 실패 저장용

	// 자식 스레드 생성 
	child->tid = thread_create (name, PRI_DEFAULT, __do_fork, fork_info);

	if (child->tid == TID_ERROR) {
		
    	free(child);
		free (fork_info);

		return TID_ERROR;
	}

	// 자식 목록 등록 
	list_push_back (&thread_current ()->children, &child->elem);
	// 자식 복제 완료 대기 
	sema_down (&child->load_sema);

	// 자식 복제 실패 처리 
	if (!child->load_success) {
		
		list_remove(&child->elem);
    	free(child);
		free (fork_info);

		return TID_ERROR;
	}

	// 임시 fork 정보 해제 
	free (fork_info);

	return child->tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr (va)){
		return true;
	}    


	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL)
		return false;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page (PAL_USER);
	if (newpage == NULL)
		return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy (newpage, parent_page, PGSIZE);
	writable = is_writable (pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page (newpage);
		return false;
	}

	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct fork_info *fork_info = aux;						//process_fork에서 넘긴 fork_info
	struct child_info *child_info = fork_info->child_info;	//이번 자식의 상태 기록표
	struct thread *parent = child_info->parent;				//부모 thread
	struct intr_frame if_;
	struct thread* current = thread_current();

	current -> child_info = child_info;

	// 부모의 유저 실행 상태 복사
	// rip  = fork syscall이 끝난 뒤 돌아갈 유저 코드 위치
	// rsp  = 부모의 유저 스택 위치
	// rdi/rsi/rdx/... = syscall 당시 레지스터 값들
	// rax  = syscall 번호 또는 반환값으로 쓸 자리
	// cs/ss/eflags = 유저모드 복귀에 필요한 CPU 상태
	// fork 이후 자식도 부모와 같은 유저 코드 위치에서 실행되어야 하므로,
	// 부모의 intr_frame(rip, rsp, 일반 레지스터 등)을 자식용 if_에 복사한다.
	memcpy (&if_, &fork_info -> parent_if, sizeof (struct intr_frame));

	// 현재 실행 중인 자식 thread의 pml4 필드에
	// 새로 만든 pml4를 넣는다
	current -> pml4 = pml4_create();
	if (current -> pml4 == NULL){
		goto error;
	}

	// 이후 자식 pml4에 페이지를 매핑할 수 있도록
	// CPU의 주소 변환 기준을 자식 pml4로 바꾼다.
	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	// 부모 주소 공간 복제
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent)){
		goto error;
	}
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	process_init ();

	for (int fd = 0; fd < FD_MAX; fd++) {
		if (parent->fd_table[fd] != NULL) {
			thread_current()->fd_table[fd] = file_duplicate (parent->fd_table[fd]);
			if (thread_current()->fd_table[fd] == NULL){
				goto error;
			}
		}
	}

	// 자식의 fork 반환값은 0
	if_.R.rax = 0;

	// 부모에게 fork 복제 성공 알림
	child_info->load_success = true;
	sema_up (&child_info->load_sema);

	// 자식은 fork 다음 줄로 유저모드 복귀
	do_iret (&if_);

error:
	//에러가 나더라도 부모는 일어나야 됨
	child_info->load_success = false;
    sema_up (&child_info->load_sema);
	thread_exit ();
}

/* 현재 프로세스에 실행 파일을 적재하고 유저 모드로 전환한다.
 * 실패하면 -1을 반환한다. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* 유저 모드 진입에 사용할 인터럽트 프레임 준비. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 기존 실행 문맥 정리. */
	process_cleanup ();

	/* 실행 파일 로드. */
	success = load (file_name, &_if);

	if (thread_current ()->child_info != NULL) {
		thread_current ()->child_info->load_success = success;
		sema_up (&thread_current ()->child_info->load_sema);
	}

	/* 로드 실패 시 종료. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* 로드된 프로세스를 유저 모드에서 시작. */
	do_iret (&_if);
	NOT_REACHED ();
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
int
process_wait (tid_t child_tid) {
	struct thread *curr=thread_current();
	struct list_elem *e;
	struct child_info *child = NULL;
	int status;

	for (e = list_begin (&curr->children);e != list_end (&curr->children);e = list_next (e)){
		struct child_info *ctid = list_entry (e, struct child_info, elem);
		if (ctid->tid == child_tid) {
			child = ctid;
			break;
		}
	}
	
	if (child == NULL) return -1;
	if (child->waited) return -1;

	child->waited = true;

	while (!child->exited){
		sema_down (&curr->child_wait_sema);
	}

	status = child->exit_status;
	list_remove (&child->elem);
	free (child);
	return status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	for (int fd = 2; fd < FD_MAX; fd++) {
		if (curr->fd_table[fd] != NULL) {
			file_close (curr->fd_table[fd]);
			curr->fd_table[fd] = NULL;
		}
	}
	if(curr->child_info){
		curr->child_info->exited = true;
		if (curr->child_info->parent != NULL) sema_up (&curr->child_info->parent->child_wait_sema);
	}

	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
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
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	//CPU의 CR3 레지스터에 이 pml4를 넣어서 주소 변환 기준으로 삼게 함
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	//이후 커널 진입 시 사용할 커널 스택 정보를 현재 thread 기준으로 갱신
	//다음 thread가 유저모드에서 커널모드로 들어올 때 사용할 커널 스택 꼭대기를 CPU에 알려주는 코드
	//rsp0 값 바꿔줌
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

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

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* ELF 실행 파일을 현재 스레드의 주소 공간에 적재한다.
 * 진입점은 RIP에, 초기 스택은 RSP에 설정한다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* 새 주소 공간 생성 및 활성화. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	if(file_name == NULL) goto done;
	char *fn_copy_parse;

	/* 명령어 인자 파싱용 복사본 생성. */
	fn_copy_parse = palloc_get_page (0);
	if(fn_copy_parse == NULL) {
		goto done;
	}
	strlcpy(fn_copy_parse, file_name, PGSIZE);

	char *save_pt;
	char **argv[64] = {0};
	int argc = 0;

	char *ftken;

	/* 실행 파일 이름과 인자 분리. */
	for(ftken = strtok_r(fn_copy_parse, " ", &save_pt);
		ftken != NULL;
		ftken = strtok_r(NULL, " ", &save_pt)) {
			if(argc > 62) goto done;
			argv[argc] = ftken;
			argc++;
		}
	if(argv[0] == NULL) goto done;


	/* 실행 파일 열기. */
	file = filesys_open (argv[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}

	/* ELF 헤더 읽기 및 검증. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv[0]);
		goto done;
	}

	/* 프로그램 헤더를 순회하며 적재할 세그먼트 처리. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* 적재하지 않는 세그먼트는 무시. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* 파일 내용과 0으로 채울 영역 계산. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* 전체를 0으로 채울 세그먼트. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					/* 계산된 영역을 사용자 가상 주소에 적재. */
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* 초기 유저 스택 생성. */
	if (!setup_stack (if_))
		goto done;

	/* 프로그램 시작 주소 설정. */
	if_->rip = ehdr.e_entry;
	
	char *user_argv[64] = {0};

	int stk_argc = argc;
	while(stk_argc > 0) {
		size_t size = (strlen(argv[stk_argc-1])+1);
		if_->rsp -= size;

		memcpy((char *)if_->rsp, argv[stk_argc-1], size);
		user_argv[stk_argc-1] = (char *)if_->rsp;
		stk_argc--;
	}

	/* 스택 정렬. */
	while(if_->rsp % 8 != 0) {
		if_->rsp--;
	}
	// if (((unsigned long long) if_->rsp & 7) != 0) {
	// 	size_t padding = (size_t) if_-> rsp % 8;
	// 	if_->rsp -= padding;
	// 	memset(*(char **) if_->rsp, 0, padding);
	// }
	if_->rsp -= 8;
	*(char **)if_->rsp = NULL;

	/* argv 포인터들을 스택에 저장. */
	for(int i=argc-1; i>=0; i--) {
		if_->rsp -= sizeof(char*); // 8byte
		*(char **)(if_->rsp) = user_argv[i];
		// if_->rsp(유저스택주소)를 1byte char 형 포인터로 형변환
		// char 포인터가 가리키는 메모리에 프로그램 인자 주소값 삽입
	}
	/* 가짜 반환 주소 저장. */
	if_->rsp -= 8;
	*(char **)if_->rsp = 0;
	
	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp + 8;

	success = true;

done:
	/* 성공/실패와 관계없이 사용한 자원 정리. */
	file_close (file);
	if(fn_copy_parse != NULL) palloc_free_page(fn_copy_parse);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
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

/* VM이 정의되어 있지 않으면:
    위쪽 load_segment()만 컴파일됨

    VM이 정의되어 있으면:
    아래쪽 load_segment()만 컴파일됨
*/
#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

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
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
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
static bool
setup_stack (struct intr_frame *if_) {
	// 8비트 정수
	uint8_t *kpage;
	bool success = false;

	// 페이지 할당
	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		
		// 커널에서 가져온(kpage) 수정할 수 있는(true) 페이지를 첫번째 인자에 매핑
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			// 인터럽트 프레임(if)에서 rsp(스택 포인터)를 USER_STACK으로 변경
			if_->rsp = USER_STACK;
			//// 프로그램 시작 시 사용할 스택의 시작위치 저장
		else
			palloc_free_page (kpage);
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
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* ELF 세그먼트를 사용자 가상 주소 공간에 등록한다.
 * VM에서는 파일 내용을 즉시 메모리에 읽지 않고, 각 페이지를 uninit page로
 * 만들어 첫 페이지 폴트 때 lazy_load_segment()가 실제 내용을 채우게 한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

    /* lazy_load_segment()가 나중에 파일에서 읽을 위치와 크기를 알 수 있도록
     * 페이지별 로드 정보를 aux로 넘긴다. */
	struct lazy_load_info {
        struct file *file; // 어떤 파일?
        off_t ofs; // 파일 어디서부터?
        size_t page_read_bytes; // 몇 바이트 읽어?
        size_t page_zero_bytes; // 나머지 몇 바이트 0으로 채워?
    };

	while (read_bytes > 0 || zero_bytes > 0) {
		/* 현재 페이지에서 파일로부터 읽을 바이트와 0으로 채울 바이트를 계산한다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

        struct lazy_load_info *info = malloc(sizeof(struct lazy_load_info));	
        
        if (info == NULL) {
            goto err;
        }

        info->file = file;
        info->ofs = ofs;
        info->page_read_bytes = page_read_bytes;
        info->page_zero_bytes = page_zero_bytes;

        void *aux = info;

		/* 사용자 가상 페이지를 SPT에 등록한다. 실제 프레임 할당과 파일 읽기는
		 * 해당 주소에 처음 접근해 page fault가 발생했을 때 수행된다. */
		if (!vm_alloc_page_with_initializer (VM_ANON, upage, writable, lazy_load_segment, aux)) {
            free(info);
            return false;
        }
			
		/* 다음 페이지의 파일 오프셋, 남은 바이트 수, 사용자 주소로 이동한다. */
		ofs += page_read_bytes;
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
	}
	return true;

err:
    return false;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
