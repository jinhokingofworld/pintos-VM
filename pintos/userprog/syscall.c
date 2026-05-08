#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "userprog/process.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

#include "devices/input.h"
#include "threads/init.h"
#include "filesys/file.h"  
#include "filesys/filesys.h" 

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static void syscall_exit (int status);
static tid_t syscall_exec (const char *file);
static int syscall_wait (tid_t pid);

static bool syscall_create (const char *file, unsigned initial_size);
static bool syscall_remove (const char *file);
static int syscall_open (const char *file);
static int syscall_filesize (int fd);
static int syscall_read (int fd, void *buffer, unsigned size);
static int syscall_write (int fd, const void *buffer, unsigned size);
static void syscall_seek (int fd, unsigned position);
static unsigned syscall_tell (int fd);
static void syscall_close (int fd);

static tid_t syscall_fork (const char *thread_name, struct intr_frame *f);
static void syscall_halt (void);
static void syscall_invalid (void);
static tid_t syscall_spawn (const char *cmdline);

static void check_address (const void *uaddr);
static void check_string (const char *str);
static void check_buffer (const void *buffer, unsigned size);

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

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	switch (f->R.rax) {
		case SYS_HALT:
			syscall_halt ();
			break;

		case SYS_EXIT:
			syscall_exit ((int) f->R.rdi);
			break;

		case SYS_FORK:
			f->R.rax = syscall_fork ((const char *) f->R.rdi, f);
			break;

		case SYS_EXEC:
			f->R.rax = syscall_exec ((const char *) f->R.rdi);
			break;

		case SYS_WAIT:
			f->R.rax = syscall_wait ((tid_t) f->R.rdi);
			break;

		case SYS_CREATE:
			f->R.rax = syscall_create ((const char *) f->R.rdi,
									   (unsigned) f->R.rsi);
			break;

		case SYS_REMOVE:
			f->R.rax = syscall_remove ((const char *) f->R.rdi);
			break;

		case SYS_OPEN:
			f->R.rax = syscall_open ((const char *) f->R.rdi);
			break;

		case SYS_FILESIZE:
			f->R.rax = syscall_filesize ((int) f->R.rdi);
			break;

		case SYS_READ:
			f->R.rax = syscall_read ((int) f->R.rdi,
									 (void *) f->R.rsi,
									 (unsigned) f->R.rdx);
			break;

		case SYS_WRITE:
			f->R.rax = syscall_write ((int) f->R.rdi,
									  (const void *) f->R.rsi,
									  (unsigned) f->R.rdx);
			break;

		case SYS_SEEK:
			syscall_seek ((int) f->R.rdi, (unsigned) f->R.rsi);
			break;

		case SYS_TELL:
			f->R.rax = syscall_tell ((int) f->R.rdi);
			break;

		case SYS_CLOSE:
			syscall_close ((int) f->R.rdi);
			break;

		case SYS_SPAWN:
			f->R.rax = syscall_spawn ((const char *) f->R.rdi);
			break;
		
		default:
			syscall_invalid ();
			break;
	}
}

static struct file *
fd_to_file(int fd) {
    if (fd < 2 || fd >= FD_MAX) { /* 각 syscall에서 따로 처리하도록 */
        return NULL;
    }
    return thread_current()->fd_table[fd];
}

static int
fd_alloc(struct file *f) { /* f = 등록할 파일 포인터 (filesys_open이 반환한 것) */
    struct thread *current = thread_current();
    for (int i = 2; i < FD_MAX; i++) { /* fd 2부터 탐색 (0, 1은 특수 fd) */
        if (current->fd_table[i] == NULL) { /* 빈 슬롯 발견 */
            current->fd_table[i] = f; /* 파일 포인터 등록 */
            return i; /* 할당된 fd 번호 반환 */
        }
    }
    return -1; /* 빈 슬롯 없음 → 실패 */
}

static void
fd_free(int fd) {
    struct file *f = fd_to_file(fd); /* fd → 파일 포인터 변환 */
    if (f == NULL) { /* 잘못된 fd → 그냥 종료 */
        return;
    }
    file_close(f); /* 커널 자원 해제 */
    thread_current()->fd_table[fd] = NULL; /* 슬롯 비우기 */
}

static void
syscall_exit (int status) {
	struct thread *curr = thread_current ();
	if (curr->child_info != NULL) curr->child_info->exit_status = status;
	printf ("%s: exit(%d)\n", thread_current ()->name, status);
	thread_exit ();
}

static tid_t
syscall_exec (const char *file) {
	check_string(file);
	char *fn_copy = palloc_get_page (0); 
	if (fn_copy == NULL)
		return -1;

	strlcpy (fn_copy, file, PGSIZE);

	if (process_exec (fn_copy) < 0)
		syscall_exit (-1);

	NOT_REACHED ();
}

static int
syscall_wait (tid_t pid) {
	return process_wait (pid);
}

static bool
syscall_create (const char *file, unsigned initial_size) {
    check_string(file);
    if (file == NULL) { /* NULL 포인터는 잘못된 사용자 접근으로 처리한다. */
        syscall_exit(-1);
    }
    if (!is_user_vaddr(file)) { /* 커널 영역 주소 접근은 허용하지 않는다. */
        syscall_exit(-1);
    }
    return filesys_create (file, initial_size);
}

static bool
syscall_remove (const char *file) {
    check_string(file);
    if (file == NULL) { /* NULL 포인터는 잘못된 사용자 접근으로 처리한다. */
        syscall_exit(-1);
    }
    if (!is_user_vaddr(file)) { /* 커널 영역 주소 접근은 허용하지 않는다. */
        syscall_exit(-1);
    }
    return filesys_remove(file); /* 파일 시스템에서 해당 파일을 삭제하고 결과를 반환한다. */
}

static int
syscall_open (const char *file) {
    check_string(file);
    if (file == NULL) { /* NULL 포인터는 잘못된 사용자 접근으로 처리한다. */
        syscall_exit(-1);
    }
    if (!is_user_vaddr(file)) { /* 커널 영역 주소 접근은 허용하지 않는다. */
        syscall_exit(-1);
    }
    struct file *f = filesys_open(file); /* 파일 시스템에서 파일을 연다. */
    if (f == NULL) {
        return -1;
    }
    int fd = fd_alloc(f); /* 열린 파일에 fd 번호를 할당한다. */
    if (fd == -1) {
        file_close(f); /* fd 할당 실패 시 열린 파일을 닫아 자원을 해제한다. */
        return -1;
    }
    return fd;
}

static int
syscall_filesize (int fd) {
    struct file *f = fd_to_file(fd);
    if (f == NULL) {
        return -1;
    }
    return file_length(f);
}

static int
syscall_read (int fd , void *buffer , unsigned size ) {
	check_buffer(buffer, size);
	if(fd == 0) {
		int read_size = 0;

		for(int i = 0; i < size; i++) {
			((uint8_t *)buffer)[i] = input_getc();
			read_size++;
		}
		return read_size;
	}
	else if (fd == 1) {
		return -1;
	}
	else if (fd >= 2) {
		struct file *get_fl = fd_to_file(fd);
		
		if(get_fl != NULL) {
			int read_size = file_read(get_fl, buffer, size);

			return read_size;
		}
	}
	return -1;
}

static int
syscall_write (int fd, const void *buffer, unsigned size) {
	check_buffer(buffer, size);
	if(fd == 0) {
		return -1;
	}
	else if (fd == 1) {
		putbuf (buffer, size);
		return size;
	}
	else if (fd >= 2) {
		struct file *get_fl = fd_to_file(fd);

		if(get_fl != NULL) {
			return file_write(get_fl, buffer, size);
		}
	}
	return -1;
}

static void
syscall_seek (int fd , unsigned position ) {
	if(fd >= 2) {
		struct file *get_fl = fd_to_file(fd);
		if(get_fl != NULL) {
			file_seek(get_fl, position);
		}
	}
}

static unsigned
syscall_tell (int fd ) {
	if(fd >= 2) {
		struct file *get_fl = fd_to_file(fd);
		if(get_fl != NULL) {
			off_t cur_pos = file_tell(get_fl);
			return cur_pos;
		}
	}
	return -1;
}

static void
syscall_close (int fd) {
    fd_free(fd);
}

static tid_t
syscall_fork (const char *thread_name, struct intr_frame *f) {
	return process_fork (thread_name, f);
}

static void
syscall_halt (void) {
	power_off ();
}

static void
syscall_invalid (void) {
	syscall_exit (-1);
}

static tid_t
syscall_spawn (const char *cmdline) {
	return process_create_initd (cmdline);
}

static void
check_address (const void *uaddr) {
	if (uaddr == NULL || !is_user_vaddr (uaddr) || pml4_get_page (thread_current ()->pml4, uaddr) == NULL) {
		syscall_exit (-1);
	}
}

static void
check_string (const char *str) {
	check_address (str);

	while (*str != '\0') {
		str++;
		check_address (str);
	}
}

static void
check_buffer (const void *buffer, unsigned size) {
	const char *buf = buffer;

	for (unsigned i = 0; i < size; i++) {
		check_address (buf + i);
	}
}
