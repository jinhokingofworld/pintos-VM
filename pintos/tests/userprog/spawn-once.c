#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

void
test_main (void) {
  pid_t pid;

  msg ("I'm your father");

  pid = spawn ("child-simple");
  msg ("wait(spawn()) = %d", wait (pid));
}
