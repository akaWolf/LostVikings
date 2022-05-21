#include "asm.h"

#include <exception>

#include <sys/time.h>

#include <cassert>
#include <ctime>

extern void render_init(void *);

bool from_callf=false;

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

void asm2C_OUT(int16_t address, int data,_STATE* _state) {
}

int8_t asm2C_IN(int16_t address,_STATE* _state) {
}

uint16_t asm2C_INW(uint16_t address,_STATE* _state) {
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


void asm2C_INT(struct _STATE* _state, int a) {
X86_REGREF
	static FILE * file;
	int i;
	AFFECT_CF(0);
	int rc;
#define SUCCESS         0       /* Function was successful      */
	log_debug2("INT %x ax=%x bx=%x cx=%x dx=%x\n",a,ax,bx,cx,dx);


	switch(a) {
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
           {log_error("MCB chain corrupted\n");exit(1);}
           AFFECT_CF(1);
           return;
      }
	AFFECT_CF(rc!=SUCCESS);
      ax++;   /* DosMemAlloc() returns seg of MCB rather than data */
	return;
			break;
		}
      /* Free memory */
	    case 0x49:
      if ((rc = DosMemFree(es - 1)) < SUCCESS)
      {
        if (DosMemCheck() != SUCCESS)
           {log_error("MCB chain corrupted\n");exit(1);}
           AFFECT_CF(1);
      }
	AFFECT_CF(rc!=SUCCESS);
	return;
      break;

      /* Set memory block size */
		    case 0x4a:
        if (DosMemCheck() != SUCCESS)
           {log_error("before 4a: MCB chain corrupted\n");exit(1);}

      if ((rc = DosMemChange(es, bx, &bx)) < 0)
      {
        if (DosMemCheck() != SUCCESS)
           {log_error("after 4a: MCB chain corrupted\n");exit(1);}
           AFFECT_CF(1);
      }
      ax = es; /* Undocumented MS-DOS behaviour expected by BRUN45! */
	AFFECT_CF(rc!=SUCCESS);
	return;
      break;
		case 0x4c:
		{
			stackDump(_state);
			jumpToBackGround = 1;
			executionFinished = 1;
			exitCode = al;
			log_error("Graceful exit al=%d\n",al);
			exit(al);
			return;
		}
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

    return(0);
 }

 void log_regs_m2c(const char *file, int line, const char *instr, _STATE* _state)
 {
  X86_REGREF
  log_debug("%05d %04X:%08X  %-54s EAX:%08X EBX:%08X ECX:%08X EDX:%08X ESI:%08X EDI:%08X EBP:%08X ESP:%08X DS:%04X ES:%04X FS:%04X GS:%04X SS:%04X CF:%d ZF:%d SF:%d OF:%d AF:%d PF:%d IF:%d\n", \
                         line,cs,eip,instr,       eax,     ebx,     ecx,     edx,     esi,     edi,     ebp,     esp,     ds,     es,     fs,     gs,     ss,     GET_CF()   ,GET_ZF()   ,GET_SF()   ,GET_OF()   ,GET_AF()   ,GET_PF(),   GET_IF());
 }

}

int main(int argc, char *argv[]) {
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
    catch (...) {
        printf("some exception\n");
    }
    return (0);
}
