#include "asm.h"
extern "C" void headless_golden_dump(void);   // direction V: end-state snapshot at clean exits (all builds; v2_gamestate.cpp)

#include <exception>
#include <string>
#include <csignal>

extern "C" int v2_fntest_isolated_active;   // v2_fn_test.cpp; see fntest_trap_or_exit

#include <sys/time.h>

#include <cassert>
#include <ctime>

extern void render_init(void *);
extern void render_init_v2(void *);
extern void sound_init();
#ifdef HEADLESS
extern void headless_init(int argc, char* argv[]);
extern int  headless_check_exit(void);
#endif

bool from_callf=false;

// v2 DosMemAlloc replay: record each allocation result + MCB header bytes
extern void v2_record_alloc(uint16_t seg, const uint8_t* mcb_ptr);

// (orig-write trap defined inside namespace m2c below — placeholder removed)

extern db& byte_128a8;   // _data.cpp global ref (#47 IRQ-defer model)
// (#60 branch) direct setter for the cs-side INT21-busy flag: the 128D1
// critical-error ISR tests it BEFORE any INT fires, so the IRQ-defer hook
// alone cannot reach the 28EB skip arm.
extern "C" void v2_fntest_set_128a8(int v);
extern "C" void v2_fntest_set_128a8(int v) { ::byte_128a8 = (db)v; }

// (#58) clean-quit shutdown chain for the INT21/4C model (see the 0x4c case)
extern void v2_game_thread_stop();
extern bool need_quit;
#ifdef FT_COV_BUILD
extern "C" void __gcov_dump(void);
#endif

namespace m2c {

// orig-write trap: m2c::setdata logs every write to v2_orig_trap_addr (with caller IP).
uintptr_t v2_orig_trap_addr = 0;
uint64_t  v2_orig_trap_count = 0;
} // close namespace m2c temporarily for global helper
extern "C++" uintptr_t* v2_orig_trap_addr_ptr_helper() {
    return &m2c::v2_orig_trap_addr;
}
namespace m2c {

#ifdef M2CDEBUG
  size_t debug = M2CDEBUG;
#else
  size_t debug = 0;
#endif

  size_t counter = 0;

    db _indent=0;
    const char *_str="";
    bool fix_segs(){return true;}
    void interpret_unknown_callf(dw cs, dd eip, db source){assert(0);}


ShadowStack shadow_stack;

//db vgaPalette[256*3];
#include "vgapal.h"
dd selectorsPointer;
dd selectors[NB_SELECTORS];

dd heapPointer;
struct find_t;
struct find_t * diskTransferAddr = 0;
//#include "memmgr.c"


bool isLittle;
bool jumpToBackGround;
//char *path;
bool executionFinished;
db exitCode;

FILE * logDebug=NULL;

#define MAX_FMT_SIZE 1024
void log_error(const char *fmt, ...) {
	char formatted_string[MAX_FMT_SIZE];
	va_list argptr;
	va_start(argptr,fmt);
	vsprintf (formatted_string,fmt, argptr);
	va_end(argptr);
#ifdef __LIBRETRO__
	log_cb(RETRO_LOG_ERROR,"%s",formatted_string);
#else
	if (logDebug!=NULL) { fprintf(logDebug,"%s",formatted_string); fflush(logDebug);}
	{ printf("%s",formatted_string); }
#endif
}
void log_debug(const char *fmt, ...) {
#ifdef M2CDEBUG
	char formatted_string[MAX_FMT_SIZE];
	va_list argptr;
	va_start(argptr,fmt);
	vsprintf (formatted_string,fmt, argptr);
	va_end(argptr);
#ifdef __LIBRETRO__
	log_cb(RETRO_LOG_DEBUG,"%s",formatted_string);
#else
	if (logDebug!=NULL) { fprintf(logDebug,"%s",formatted_string); fflush(logDebug); } else { printf("%s",formatted_string); }
#endif
#endif
}

void log_info(const char *fmt, ...) {
	char formatted_string[MAX_FMT_SIZE];
	va_list argptr;
	va_start(argptr,fmt);
	vsprintf (formatted_string,fmt, argptr);
	va_end(argptr);
#ifdef __LIBRETRO__
	log_cb(RETRO_LOG_INFO,"%s",formatted_string);
#else
	if (logDebug!=NULL) { fprintf(logDebug,"%s",formatted_string); fflush(logDebug); } else { printf("%s",formatted_string); }
#endif
}

void log_debug2(const char *fmt, ...) {
#if M2CDEBUG>=2
	char formatted_string[MAX_FMT_SIZE];
	va_list argptr;
	va_start(argptr,fmt);
	vsprintf (formatted_string,fmt, argptr);
	va_end(argptr);
	log_debug(formatted_string);
#endif
}


void stackDump(struct _STATE* _state) {
X86_REGREF

	log_debug("is_little_endian()=%d\n",isLittle);
	log_debug("sizeof(dd)=%zu\n",sizeof(dd));
	log_debug("sizeof(dd *)=%zu\n",sizeof(dd *));
	log_debug("sizeof(dw)=%zu\n",sizeof(dw));
	log_debug("sizeof(db)=%zu\n",sizeof(db));
//	log_debug("sizeof(jmp_buf)=%zu\n",sizeof(jmp_buf));
//	log_debug("sizeof(mem)=%zu\n",sizeof(m));
	log_debug("eax: %x\n",eax);
//	hexDump(&eax,sizeof(dd));
	log_debug("ebx: %x\n",ebx);
	log_debug("ecx: %x\n",ecx);
	log_debug("edx: %x\n",edx);
	log_debug("ebp: %x\n",ebp);
	log_debug("cs: %d -> %p\n",cs,(void *) realAddress(0,cs));
	log_debug("ds: %d -> %p\n",ds,(void *) realAddress(0,ds));
	log_debug("esi: %x\n",esi);
	log_debug("ds:esi %p\n",(void *) realAddress(esi,ds));
	log_debug("es: %d -> %p\n",es,(void *) realAddress(0,es));
	hexDump(&es,sizeof(dd));
	log_debug("edi: %x\n",edi);
	log_debug("es:edi %p\n",(void *) realAddress(edi,es));
	hexDump((void *) realAddress(edi,es),50);
	log_debug("fs: %d -> %p\n",fs,(void *) realAddress(0,fs));
	log_debug("gs: %d -> %p\n",gs,(void *) realAddress(0,gs));
//	log_debug("adress heap: %p\n",(void *) &m.heap);
}

// thanks to paxdiablo http://stackoverflow.com/users/14860/paxdiablo for the hexDump function
void hexDump (void *addr, int len) {
	int i;
	unsigned char buff[17];
	unsigned char *pc = (unsigned char*)addr;
	(void) buff;
	log_debug ("hexDump %p:\n", addr);

	if (len == 0) {
		log_debug("  ZERO LENGTH\n");
		return;
	}
	if (len < 0) {
		log_debug("  NEGATIVE LENGTH: %i\n",len);
		return;
	}

	// Process every byte in the data.
	for (i = 0; i < len; i++) {
		// Multiple of 16 means new line (with line offset).

		if ((i % 16) == 0) {
			// Just don't print ASCII for the zeroth line.
			if (i != 0)
				log_debug ("  %s\n", buff);

			// Output the offset.
			log_debug ("  %04x ", i);
		}

		// Now the hex code for the specific character.
		log_debug (" %02x", pc[i]);

		// And store a printable ASCII character for later.
		if ((pc[i] < 0x20) || (pc[i] > 0x7e))
			buff[i % 16] = '.';
		else
			buff[i % 16] = pc[i];
		buff[(i % 16) + 1] = '\0';
	}

	// Pad out last line if not exactly 16 characters.
	while ((i % 16) != 0) {
		log_debug ("   ");
		i++;
	}

	// And print the final ASCII bit.
	log_debug ("  %s\n", buff);
}

static uint16_t v2_pit_counter = 0xFFFF;
static uint16_t v2_pit_latch = 0;
static int v2_pit_lohi = 0;
// (#60 branch) PIT jitter: when enabled, latch steps alternate a small
// (0x0BF0 < 0x1BF8) and the normal (0x1C00) delta — the real 1.19MHz chip
// gives variable elapsed counts per loop, so BOTH sub_179a8 retry edges
// (79F5 JL) are live on hardware. Off by default: the calibrated unit
// expectations (12ef8 idle thresholds) ride on the constant step.
extern "C" int v2_fntest_pit_jitter;
extern "C" { int v2_fntest_pit_jitter = 0; }
static int v2_pit_jitter_phase = 0;
// (#61) native AIL channel hooks — see src/sdl/v2_native_opl.cpp
extern "C" int  v2_nopl_enabled(void);
extern "C" void v2_nopl_out(uint16_t port, uint8_t val);
extern "C" void v2_nopl_set_pit_divisor(uint32_t divisor);
static int v2_pit_mode_pending = 0;      // 0x34/0x36 written to 0x43 → next two 0x40 bytes = divisor
static uint32_t v2_pit_divisor_lo = 0;
static int v2_pit_divisor_phase = 0;

void asm2C_OUT(int16_t address, int data,_STATE* _state) {
	// (#61) AdLib register file → native OPL queue (game thread)
	if ((uint16_t)address == 0x388 || (uint16_t)address == 0x389) {
		v2_nopl_out((uint16_t)address, (uint8_t)data);
		return;
	}
	if ((uint16_t)address == 0x40 && v2_pit_mode_pending) {
		// PIT channel-0 divisor reload (lo, then hi) — the AIL driver
		// programs its tick rate here.
		if (v2_pit_divisor_phase == 0) {
			v2_pit_divisor_lo = (uint8_t)data;
			v2_pit_divisor_phase = 1;
		} else {
			uint32_t div = ((uint32_t)(uint8_t)data << 8) | v2_pit_divisor_lo;
			v2_pit_mode_pending = 0;
			v2_pit_divisor_phase = 0;
			v2_nopl_set_pit_divisor(div);
		}
		return;
	}
	if ((uint16_t)address == 0x43 && ((data & 0x30) == 0x30)) {
		// mode byte with both access bits (lo+hi reload, e.g. 0x34/0x36):
		// a rate program follows on port 0x40 — distinct from the 0x00
		// latch command modeled below.
		v2_pit_mode_pending = 1;
		v2_pit_divisor_phase = 0;
		return;
	}
	if ((uint16_t)address == 0x43) {           // PIT latch command
		uint16_t step = 0x1C00;
		// (#60 fact) elapsed is measured between latch pairs — the SECOND
		// latch's step decides it, so the small delta must land on EVEN calls.
		if (v2_fntest_pit_jitter && !(v2_pit_jitter_phase ^= 1))
			step = 0x0BF0;
		v2_pit_counter = (uint16_t)(v2_pit_counter - step);
		v2_pit_latch = v2_pit_counter;
		v2_pit_lohi = 0;
		return;
	}
}

// fn-test environment models (#47):
//  - port 0x40 (PIT counter, 8253 mode 2): a DOWN-counter — the real chip
//    decrements between reads; a constant would make the game's joystick
//    timer calibration (sub_179a8) spin forever.
//  - port 0x201 (game port): idle default is overridable per unit-case via
//    v2_fntest_set_in201 (axis bits need a non-idle byte).
// Idle game port reads 0xFF on real hardware (pull-ups, buttons active-low,
// no axis capacitors discharging) — 0 would read as "all buttons pressed"
// and a successful axis detect, flooding live input once the restored
// joystick detect (12989 tail) runs.
static int v2_fntest_in201 = 0xFF;
extern "C" void v2_fntest_set_in201(int v) { v2_fntest_in201 = v & 0xFF; }
// (#60 branch) port 0x60 (8042 keyboard data): scancode read by the INT9
// ISR body (seg000_6440_proc). Unit cases seed the byte the ISR sees.
static int v2_fntest_in60 = 0;
extern "C" void v2_fntest_set_in60(int v) { v2_fntest_in60 = v & 0xFF; }
// 8253 PIT model: OUT 0x43 latches the down-counter (mode-2 semantics),
// the two following IN 0x40 reads return the LATCHED lo then hi byte —
// reading the live counter without the latch protocol is what the real
// chip forbids. Coarse step per latch keeps sub_179a8's calibration finite.
// (The state lives above asm2C_OUT, which handles the 0x43 latch command.)
int8_t asm2C_IN(int16_t address,_STATE* _state) {
	if ((uint16_t)address == 0x40) {
		int8_t b = (int8_t)((v2_pit_lohi == 0) ? (v2_pit_latch & 0xFF)
		                                       : (v2_pit_latch >> 8));
		v2_pit_lohi ^= 1;
		return b;
	}
	if ((uint16_t)address == 0x201) return (int8_t)v2_fntest_in201;
	if ((uint16_t)address == 0x60) return (int8_t)v2_fntest_in60;
	// (#60 branch) port 0x3DA (CRT status): bit 3 = vertical retrace. The
	// real bit alternates with the beam; a constant would leave one edge of
	// every "wait for retrace" poll loop (sub_12eed) structurally dead.
	// Toggle per read: the loop spins at least once, then exits.
	if ((uint16_t)address == 0x3DA) {
		static int v2_crt_toggle = 0;
		return (int8_t)((v2_crt_toggle ^= 1) ? 0x08 : 0x00);
	}
	return 0;  // FIX: Return value required on ARM
}

uint16_t asm2C_INW(uint16_t address,_STATE* _state) {
	return 0;  // FIX: Return value required on ARM
}

bool is_little_endian_real_check() {
	union
	{
		uint16_t x;
		uint8_t y[2];
	} u;

	u.x = 1;
	return u.y[0];
}

/**
 * is_little_endian:
 *
 * Checks if the system is little endian or big-endian.
 *
 * Returns: greater than 0 if little-endian,
 * otherwise big-endian.
 **/
bool is_little_endian()
{
#if defined(__x86_64) || defined(__i386) || defined(_M_IX86) || defined(_M_X64)
	return 1;
#elif defined(MSB_FIRST)
	return 0;
#else
	return is_little_endian_real_check();
#endif
}


#ifndef __BORLANDC__ //TODO
//#if !CYGWIN
double realElapsedTime(void) {              // returns 0 first time called
//    static struct timeval t0;
    struct timeval tv;
    gettimeofday(&tv, 0);
 //   if (!t0.tv_sec)                         // one time initialization
   //     t0 = tv;
    return ((tv.tv_sec /*- t0.tv_sec*/ + (tv.tv_usec /* - t0.tv_usec*/)) / 1000000.) * 18.;
}
#endif



void asm2C_init() {
	isLittle=is_little_endian();
#ifdef MSB_FIRST
	if (isLittle) {
		log_error("Inconsistency: is_little_endian=true and MSB_FIRST defined.\n");
		exit(1);
	}
#endif
	if (isLittle!=is_little_endian_real_check()) {
		log_error("Inconsistency in little/big endianess detection. Please check if the Makefile sets MSB_FIRST properly for this architecture.\n");
		exit(1);
	}
	log_debug2("asm2C_init is_little_endian:%d\n",isLittle);
}


// FN-TEST: synthetic orig-UB inputs can drive INT 21h into terminate (4Ch)
// or MCB-corruption exits. Inside an isolated oracle call jump straight to
// the isolated-call recovery point (v2_fntest_escape_jump — a raise(SIGABRT)
// here raced with the SIGALRM watchdog and could strand the process in
// sigsuspend); outside fn-test this behaves exactly like the plain exit.
extern "C" void v2_fntest_escape_jump(void);
static void fntest_trap_or_exit(int code) {
	if (::v2_fntest_isolated_active) v2_fntest_escape_jump();
	exit(code);
}

// fn-test (#47): reproduce the orig IRQ0 audio-tick race — the real ISR
// sets cs:byte_128A8=1 when it fires while DOS is busy; sub_128a9's defer
// arm is unreachable without it under the deterministic INT models.
extern "C" { int v2_fntest_sim_int21_irq = 0; }
// fn-test (#47): FNSELFTEST processes skip m2c::init, so first_mcb stays 0
// and DosMemAlloc walks garbage MCBs from segment 0 (2s hang -> SIGALRM).
// Same mcb_init call as m2c::init performs.
extern "C" void v2_fntest_meminit(void) {
	mcb_init(seg_offset(heap), (HEAP_SIZE >> 4) - seg_offset(heap) - 1, MCB_LAST);
}
void asm2C_INT(struct _STATE* _state, int a) {
X86_REGREF
	static FILE * file;
	int i;
	AFFECT_CF(0);
	int rc;
#define SUCCESS         0       /* Function was successful      */
	log_debug2("INT %x ax=%x bx=%x cx=%x dx=%x\n",a,ax,bx,cx,dx);


	if (a == 0x21 && v2_fntest_sim_int21_irq) ::byte_128a8 = 1;
	switch(a) {
	case 0x10:
		// BIOS VIDEO. ah=0x1A al=0: GET DISPLAY COMBINATION — the game's VGA
		// detect (sub_12989 registration path). Real VGA BIOS echoes al=0x1A
		// and returns bl=8 (VGA color). Deterministic environment model in
		// the same spirit as the fixed INT21/2C time; other INT10 functions
		// stay no-ops (previous behavior via the default arm).
		if (ah == 0x1A && al == 0x00) { al = 0x1A; bl = 0x08; }
		return;
	case 0x21:
	{
		switch(ah) {
		case 0x48:
		{
      /* Allocate memory */
      if ((rc = DosMemAlloc(bx, mem_access_mode, &ax, &bx)) < 0)
      {
        DosMemLargest(&bx);
        if (DosMemCheck() != SUCCESS)
           {log_error("MCB chain corrupted\n");fntest_trap_or_exit(1);}
           AFFECT_CF(1);
           return;
      }
	AFFECT_CF(rc!=SUCCESS);
      ax++;   /* DosMemAlloc() returns seg of MCB rather than data */
      // Record allocation result + MCB header for v2 replay.
      // MCB sits at (ax-1)*16 in flat m2c memory.
      ::v2_record_alloc(ax, (const uint8_t*)&m2c::m + (uint32_t)(ax - 1) * 16);
	return;
			break;
		}
      /* Free memory */
	    case 0x49:
      if ((rc = DosMemFree(es - 1)) < SUCCESS)
      {
        if (DosMemCheck() != SUCCESS)
           {log_error("MCB chain corrupted\n");fntest_trap_or_exit(1);}
           AFFECT_CF(1);
      }
	AFFECT_CF(rc!=SUCCESS);
	return;
      break;

      /* Set memory block size */
		    case 0x4a:
        if (DosMemCheck() != SUCCESS)
           {log_error("before 4a: MCB chain corrupted\n");fntest_trap_or_exit(1);}

      if ((rc = DosMemChange(es, bx, &bx)) < 0)
      {
        if (DosMemCheck() != SUCCESS)
           {log_error("after 4a: MCB chain corrupted\n");fntest_trap_or_exit(1);}
           AFFECT_CF(1);
      }
      ax = es; /* Undocumented MS-DOS behaviour expected by BRUN45! */
	AFFECT_CF(rc!=SUCCESS);
	return;
      break;
		case 0x4c:
		{
			if (::v2_fntest_isolated_active) v2_fntest_escape_jump();  // fn-test: DOS terminate = escape
			stackDump(_state);
			jumpToBackGround = 1;
			executionFinished = 1;
			exitCode = al;
			log_error("Graceful exit al=%d\n",al);
			// #58 root: plain exit() runs static destructors, and
			// ~condition_variable (v2_cv_start) BLOCKS forever in
			// pthread_cond_destroy while the v2 game thread still waits
			// on it — the process "hangs" after the game already quit
			// (title-screen F10 = direct DOS quit, no prompt). Shut the
			// worker threads down first, flush gcov (COV builds merge the
			// live profile), then _exit to skip the destructor chain —
			// the same shutdown order as the headless max-frames path.
			need_quit = true;
			v2_game_thread_stop();
			headless_golden_dump();  // direction V: DOS terminate is a clean exit (all builds)
			fflush(stdout); fflush(stderr);
#ifdef FT_COV_BUILD
			__gcov_dump();
#endif
			_exit(al);
			return;
		}
		case 0x35: // GET INTERRUPT VECTOR — not implemented, return 0
			bx = 0; es = 0;
			AFFECT_CF(0);
			return;
		case 0x25: // SET INTERRUPT VECTOR — not implemented, ignore
			AFFECT_CF(0);
			return;
		case 0x2C: // GET CURRENT TIME — return fixed value for deterministic PRNG
			cx = 0x1234; dx = 0x5678;
			AFFECT_CF(0);
			return;
		case 0x58: // mem allocation policy
		{
#ifdef __DJGPP__
        call_dos_realint(_state, a);
			return;
#endif
			return;
		}
		default:
			break;
		}
	}
	}
	AFFECT_CF(1);
	log_debug("Error DOSInt 0x%x ah:0x%x al:0x%x: bx:0x%x not supported.\n",a,ah,al,bx);
}

const char* log_spaces(int n)
{
 static const char s[]="                                                                                          ";
//	memset(s, ' ', n);
//	*(s+n) = 0;
  return s+(88-n);
}


int init(struct _STATE *state);

void mainproc(_offsets _i, struct _STATE *state);


/*std::thread int8_thread;
void int8_thread_proc()
{
_STATE state;
_STATE* _state = &state;
X86_REGREF

//R(MOV(cs, seg_offset(_text)));	// mov cs,_TEXT

  R(MOV(ss, seg_offset(int8stack)));	// mov cs,_TEXT
#if _BITS == 32
  esp = ((dd)(db*)&m.int8stack[STACK_SIZE - 4]);
#else
  esp=0;
  sp = STACK_SIZE - 4;
#endif

  es=0;

while(true)
	{
		bx=*(dw *)realAddress(8*4,0);
//		es=(dw *)realAddress(8*4+2,0);

		if (bx)
		{

			CALL(static_cast<_offsets>(bx));
std::this_thread::sleep_for(std::chrono::microseconds(1));
		}
	}
}
*/
int init(struct _STATE* _state, struct _STATE* _render_state)
 {
    X86_REGREF

    log_debug("~~~ heap_size=%d heap_para=%x heap_seg=%x\n", HEAP_SIZE, (HEAP_SIZE >> 4), seg_offset(heap) );
    /* We expect ram_top as Kbytes, so convert to paragraphs */
    mcb_init(seg_offset(heap), (HEAP_SIZE >> 4) - seg_offset(heap) - 1, MCB_LAST);

    R(MOV(ss, seg_offset(stack)));
 #if _BITS == 32
    esp = ((dd)(db*)&stack[STACK_SIZE - 4]);
 #else
    esp = 0;
    sp = STACK_SIZE - 4;
    ds = es = 0x192; // dosbox PSP
    *(dw*)(raddr(0, 0x408)) = 0x378; //LPT
 #endif

//	*(dw *)realAddress(8*4,0)=k_int8old;
//    int8_thread = std::thread(int8_thread_proc);
//	int8_thread.detach();

	render_init((void*)_render_state);
	render_init_v2((void*)_render_state);  // Второе окно для тестов
	sound_init();

    return(0);
 }

 void log_regs_m2c(const char *file, int line, const char *instr, _STATE* _state)
 {
  ++counter;
  X86_REGREF
  log_debug("%x %05d %04X:%08X  %-54s EAX:%08X EBX:%08X ECX:%08X EDX:%08X ESI:%08X EDI:%08X EBP:%08X ESP:%08X DS:%04X ES:%04X FS:%04X GS:%04X SS:%04X CF:%d ZF:%d SF:%d OF:%d AF:%d PF:%d IF:%d\n", \
                         counter,line,cs,eip,instr,       eax,     ebx,     ecx,     edx,     esi,     edi,     ebp,     esp,     ds,     es,     fs,     gs,     ss,     GET_CF()   ,GET_ZF()   ,GET_SF()   ,GET_OF()   ,GET_AF()   ,GET_PF(),   GET_IF());
 }

}

#ifdef __linux__
#include <execinfo.h>
#endif
#ifndef _WIN32
#include <unistd.h>   // _exit
#endif
#include <exception>
#include <csignal>

// SIGINT/SIGTERM: m2c game thread loops forever in C++ goto chain — has no
// need_quit check. SDL render thread sets need_quit on SDL_QUIT but game
// thread ignores it, so process never exits on Ctrl-C. Force exit.
extern "C" int v2_fntest_selftest_env(void);  // v2_fn_test.cpp (FNSELFTEST env)
extern "C" void v2_midi_shutdown(void);       // v2_midi.cpp: UX stage 11, V2_MIDI_DUMP is written before the _exit below

static void asm_sigint_handler(int sig) {
    fprintf(stderr, "\nSignal %d received — exiting\n", sig);
    // Final reports: dump before _exit() bypasses atexit().
    extern void v2_audit_dump_final();
    extern void v2_dump_opcode_coverage();
    extern void v2_dump_psnap_summary();
    v2_audit_dump_final();
    v2_dump_opcode_coverage();
    v2_dump_psnap_summary();
    v2_midi_shutdown();
    _exit(128 + sig);
}

static void v2_terminate_handler() {
    fprintf(stderr, "\n=== std::terminate called ===\n");
#ifdef __linux__
    void* bt[40];
    int n = backtrace(bt, 40);
    backtrace_symbols_fd(bt, n, fileno(stderr));
#else
    fprintf(stderr, "(backtrace unavailable on this platform)\n");
#endif
    auto e = std::current_exception();
    if (e) {
        try { std::rethrow_exception(e); }
        catch (const std::exception& ex) { fprintf(stderr, "exception: %s\n", ex.what()); }
        catch (...) { fprintf(stderr, "unknown exception\n"); }
    } else {
        fprintf(stderr, "no active exception\n");
    }
    fflush(stderr);
    std::abort();
}

// Global "debug build" flag — set by --debug CLI option. Mirrors orig DOS
// debug-build conditional that gated F4 (INT 3), F5 (prev level cheat),
// F6 (next level cheat). orig had these enabled only in dev builds via
// non-zero word_286E2 (ds:0x202). Game logic at eip 0xFE: TEST word_286E2,
// 0xFFFFh; JZ skip — so any non-zero value enables.
bool g_debug_mode = false;

#ifndef HEADLESS
extern void v2_keymap_load(const char* path);
extern "C" void v2_input_recorder_init(const char*, const char*, int);
#endif

int main(int argc, char *argv[]) {
    std::set_terminate(v2_terminate_handler);
    signal(SIGINT, asm_sigint_handler);
    signal(SIGTERM, asm_sigint_handler);

    // FN-TEST synthetic-diff selftest (SYNTHETIC_DIFF_ANALYSIS.md): env
    // FNSELFTEST=<fn|all> runs isolated orig<->v2 differential tests on
    // generated inputs and exits — no game, no SDL, no threads. The static
    // EXE image in m2c::m is already populated (global Initializer ran
    // before main); m2c::init is intentionally NOT called here.
    {
        int _ft_rc = v2_fntest_selftest_env();
        if (_ft_rc >= 0) return _ft_rc;
    }

    // Task #19 aid: env V2_WP_LIN=<linear offset into m2c::m> — arm the HW
    // watchpoint on that byte from process start (catches level-load writers;
    // samples drain in the per-frame v2_hw_wp_drain calls).
    if (const char* _wp = getenv("V2_WP_LIN")) {
        extern void v2_hw_wp_arm(uint8_t* ptr, const char* label);
        v2_hw_wp_arm((uint8_t*)&m2c::m + strtoul(_wp, nullptr, 0), "V2_WP_LIN");
    }

#ifdef HEADLESS
    // HEADLESS init must run BEFORE any SDL call (sets SDL_VIDEODRIVER=dummy)
    // and BEFORE m2c::init (installs SIGSEGV handler, parses --replay-input).
    headless_init(argc, argv);
#endif

#ifndef HEADLESS
    // HEADLESS path loads keymap + recorder inside headless_init above.
    const char* keymap_path  = nullptr;
    const char* record_input = nullptr;
    const char* replay_input = nullptr;
    bool        strict_replay = false;
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            g_debug_mode = true;
            fprintf(stderr, "[main] --debug enabled: F4 (INT 3), F5 (prev level), F6 (next level) cheats active\n");
        }
#ifndef HEADLESS
        else if (strncmp(argv[i], "--keymap=", 9) == 0) {
            keymap_path = argv[i] + 9;
        }
        else if (strncmp(argv[i], "--record-input=", 15) == 0) {
            record_input = argv[i] + 15;
        }
        else if (strncmp(argv[i], "--replay-input=", 15) == 0) {
            replay_input = argv[i] + 15;
        }
        else if (strcmp(argv[i], "--replay-strict") == 0) {
            strict_replay = true;
        }
#endif
    }

#ifndef HEADLESS
    v2_keymap_load(keymap_path);
    // Input record/replay (test mode). render.cpp's event loop already
    // routes through v2_input_poll_event, so init alone enables it. Recording
    // here captures full m2c gameplay; the resulting .inp replays bit-for-bit
    // in vikings_headless (same asm.cpp code path).
    v2_input_recorder_init(record_input, replay_input, strict_replay ? 1 : 0);
#endif

    struct m2c::_STATE state;
    struct m2c::_STATE *_state = &state;
	struct m2c::_STATE render_state;
	struct m2c::_STATE *_render_state = &render_state;

    X86_REGREF

    eax = ebx = ecx = edx = ebp = esi = edi = fs = gs = 0; // according to ms-dos 6.22 debuger
    AFFECT_DF(0);
    AFFECT_CF(0);
    AFFECT_ZF(0);
    AFFECT_SF(0);
    AFFECT_OF(0);
    AFFECT_AF(0);
    AFFECT_PF(0);
    AFFECT_IF(0);
    cx = 0xff; // dummy size of executable

    // (HW watchpoint moved to v2 shadow_ds[0x34] hunt — armed in v2_vm.cpp after
    //  shadow_ds becomes available.)

    try {
        m2c::_indent = 0;
        //m2c::logDebug = fopen("asm.log", "w");

        m2c::init(_state, _render_state);

        if (argc >= 2) {
            db s = strlen(argv[1]);
            *(((db *) &m2c::m) + 0x80) = s + 1;
            strcpy(((char *) &m2c::m) + 0x81, argv[1]);
            *(dw *)((db*)&m2c::m + 0x81 + s) = 0xD;

        }
        (*m2c::_ENTRY_POINT_)((m2c::_offsets) 0, _state);
    }
    catch (const std::exception &e) {
        printf("std::exception& %s\n", e.what());
    }
    return (0);
}
