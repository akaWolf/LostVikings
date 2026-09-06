// Translation unit of the generated executors of world-script chunk 0x1C5
// (tools/data/gen_transpile.py): the chunk executor — piece functions + router —
// and the anim executor, compiled apart from v2_vm.cpp so that the compiler's
// memory stays per chunk (see v2_vm_gen.h). The _entry functions are what
// v2_vm.cpp's dispatchers call; without V2_GENCODE they report "not handled".
#include "../v2_vm_gen.h"
#ifdef V2_GENCODE
#include "chunk_01c5.gen.inc"
#include "anim_01c5.gen.inc"
bool v2_gen_exec_1c5_entry(V2VM& vm, int& max_ops) { return v2_gen_exec_1c5(vm, max_ops); }
bool v2_gen_anim_1c5_entry(V2VM& vm, uint16_t& anim_bx, int& max) { return v2_gen_anim_1c5(vm, anim_bx, max); }
#else
bool v2_gen_exec_1c5_entry(V2VM&, int&) { return false; }
bool v2_gen_anim_1c5_entry(V2VM&, uint16_t&, int&) { return false; }
#endif
