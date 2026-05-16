#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

struct anon_page {
};

struct segment_load_info {
            struct file *file;  // 불러올 파일
            off_t offset;       // 읽기 시작할 부분
            size_t read_bytes;  // 읽을 크기
            size_t zero_bytes;  // 0으로 채울 크기
        };

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
