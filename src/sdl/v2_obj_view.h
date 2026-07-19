// v2_obj_view.h — object-slot views over the flat shadow DS (phase B, #38).
//
// Two backends, one vocabulary:
//   ObjMem — raw uint8_t* DS (phase functions, unit runners). Reads/writes
//            go straight to the byte array, exactly like the `s + COL` forms
//            it replaces.
//   ObjRef — V2VM-backed (opcode handlers). Reads/writes go through
//            vm.ds_read/ds_write so every existing diagnostic trap
//            (V2-DS302, PWWRITE, 0x117C window, ...) keeps firing.
//
// Views carry {base, slot}; slot is the object column index (obj = slot
// value used by the orig: even 0..0x26 for main slots, sub-sprite slots
// above). Memory layout is untouched — this is naming/roles only.

#ifndef V2_OBJ_VIEW_H
#define V2_OBJ_VIEW_H

#include <cstdint>
#include "v2_ds_layout.h"

struct ObjMem {
    uint8_t* s;
    uint16_t slot;

    uint16_t u16(uint16_t col) const { return *(uint16_t*)(s + (uint16_t)(slot + col)); }
    int16_t  i16(uint16_t col) const { return (int16_t)u16(col); }
    void     w16(uint16_t col, uint16_t v) const { *(uint16_t*)(s + (uint16_t)(slot + col)) = v; }

    uint16_t flags()      const { return u16(OBJ_FLAGS); }
    uint16_t code_seg()   const { return u16(OBJ_CODE_SEG); }
    uint16_t pc()         const { return u16(OBJ_PC); }
    int16_t  world_x()    const { return i16(OBJ_WORLD_X); }
    int16_t  world_y()    const { return i16(OBJ_WORLD_Y); }
    uint16_t anim_idx()   const { return u16(OBJ_ANIM_IDX); }
    int16_t  bbox_x0()    const { return i16(OBJ_BBOX_X0); }
    int16_t  bbox_x1()    const { return i16(OBJ_BBOX_X1); }
    int16_t  bbox_y0()    const { return i16(OBJ_BBOX_Y0); }
    int16_t  bbox_y1()    const { return i16(OBJ_BBOX_Y1); }
    uint16_t type_id()    const { return u16(OBJ_TYPE_ID); }
    uint16_t partner()    const { return u16(OBJ_PARTNER); }
    uint16_t sub_slot()   const { return u16(OBJ_SUB_SLOT); }
    uint16_t sub_end()    const { return u16(OBJ_SUB_END); }
    uint16_t sub_count()  const { return u16(OBJ_SUB_COUNT); }
    void set_partner(uint16_t v)  const { w16(OBJ_PARTNER, v); }
    void set_alt_pc(uint16_t v)   const { w16(OBJ_ALT_PC, v); }
};

// Defined in v2_vm.cpp where struct V2VM is visible; declared here so both
// TUs share the vocabulary. Kept as a template-free thin adapter: the
// implementation forwards to vm.ds_read / vm.ds_write (traps preserved).
struct V2VM;
struct ObjRef {
    V2VM& vm;
    uint16_t slot;

    uint16_t u16(uint16_t col) const;                // vm.ds_read(slot + col)
    int16_t  i16(uint16_t col) const { return (int16_t)u16(col); }
    void     w16(uint16_t col, uint16_t v) const;    // vm.ds_write(slot + col, v)

    uint16_t flags()      const { return u16(OBJ_FLAGS); }
    uint16_t code_seg()   const { return u16(OBJ_CODE_SEG); }
    int16_t  world_x()    const { return i16(OBJ_WORLD_X); }
    int16_t  world_y()    const { return i16(OBJ_WORLD_Y); }
    uint16_t anim_idx()   const { return u16(OBJ_ANIM_IDX); }
    int16_t  bbox_x0()    const { return i16(OBJ_BBOX_X0); }
    int16_t  bbox_x1()    const { return i16(OBJ_BBOX_X1); }
    int16_t  bbox_y0()    const { return i16(OBJ_BBOX_Y0); }
    int16_t  bbox_y1()    const { return i16(OBJ_BBOX_Y1); }
    uint16_t type_id()    const { return u16(OBJ_TYPE_ID); }
    uint16_t partner()    const { return u16(OBJ_PARTNER); }
    void set_partner(uint16_t v) const { w16(OBJ_PARTNER, v); }
    void set_alt_pc(uint16_t v)  const { w16(OBJ_ALT_PC, v); }
};

#endif // V2_OBJ_VIEW_H
