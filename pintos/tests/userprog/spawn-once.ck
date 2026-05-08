# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(spawn-once) begin
(spawn-once) I'm your father
(child-simple) run
child-simple: exit(81)
(spawn-once) wait(spawn()) = 81
(spawn-once) end
spawn-once: exit(0)
EOF
pass;
