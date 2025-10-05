#include <SDL2/SDL.h>
#include <thread>
#include <cassert>
#include <cstdio>
#include <atomic>
#include <unistd.h>  // _exit

const int SCREEN_SCALE = 4;
const int SCREEN_WIDTH = 320;
const int SCREEN_HEIGHT = 240;
const int RENDER_WIDTH = 344;
const int RENDER_HEIGHT = 240;
uint32_t tempDrawBuffer[RENDER_WIDTH*RENDER_HEIGHT];
struct myDrawInfoS
{
  uint8_t drawBuffer[65536*4];
  SDL_Color drawPalette[256];
  uint32_t myOffset;
  uint8_t myPixelOffset;
};
struct myDrawInfoS* myDrawInfo = nullptr;
SDL_Window* myWindow = NULL;
SDL_Renderer* myRenderer = NULL;
SDL_Texture* myTexture = NULL;
SDL_PixelFormat *myFormat = NULL;

extern void render_callback(void *);
extern uint16_t input_keys_v2;
uint16_t input_keys = 0;
bool need_quit = false;

// SDL spec-key state (replaces orig int 9 ISR's writes to byte_31669 etc).
// Game logic ORs sdl_spec_get(off) at byte_316xx CMP/TEST sites.
//
// Two key roles need different semantics to mirror orig DOS keyboard ISR:
//
// MODIFIERS (ALT, CTRL): sticky — KEYDOWN sets atomic=1, KEYUP sets =0.
// Snapshot just copies. Stays 1 the whole time the key is held, so a trigger
// pressed later in the same hold combines correctly (e.g. ALT held, then S
// pressed → both register simultaneously in same frame).
//
// TRIGGERS (S, M, X, F10, DEL): edge — KEYDOWN sets atomic=1; snapshot
// consumes via exchange(0). Each press produces exactly one game-side
// toggle, even if user holds the key (no auto-repeat-driven retoggle).
//
// Two-stage atomic→snap to avoid orig/v2 desync within a frame: render
// thread updates atomic; v2_phase_frame_begin snapshots once per frame.
std::atomic<uint8_t> sdl_spec_state[256] = {};
static uint8_t sdl_spec_snap[256] = {};

static bool sdl_spec_is_modifier(uint16_t off) {
    return off == 0x91A4 /* ALT */ || off == 0x9189 /* CTRL */;
}

uint8_t sdl_spec_get(uint16_t off) {
    return sdl_spec_snap[off & 0xFF];
}

void sdl_spec_snapshot_take() {
    for (int i = 0; i < 256; i++) {
        // Modifiers: copy. Triggers: exchange-to-0 (consume one press).
        // Modifier offsets 0x9189 (CTRL low=0x89) and 0x91A4 (ALT low=0xA4).
        if (i == 0x89 || i == 0xA4) {
            sdl_spec_snap[i] = sdl_spec_state[i].load(std::memory_order_relaxed);
        } else {
            sdl_spec_snap[i] = sdl_spec_state[i].exchange(0, std::memory_order_relaxed);
        }
    }
}


unsigned int plane4_to_linear(unsigned int plane, unsigned int offset)
{
  return offset * 4 + plane;
}
uint32_t planar_to_linear(uint32_t x, uint32_t y)
{
  return (y * RENDER_WIDTH + x);
}
/*void drawPixel(uint32_t offset, uint8_t color)
{
  myDrawInfo->drawBuffer[offset] = color;
}
void drawPixel(uint32_t x, uint32_t y, uint8_t color)
{
  int offset = planar_to_linear(x, y);

  myDrawInfo->drawBuffer[offset] = color;
}
void setPalette(uint8_t color, uint8_t r, uint8_t g, uint8_t b)
{
  myDrawInfo->drawPalette[color] = {r, g, b, 255};
}*/
void updateDraw()
 {
   auto offset = myDrawInfo->myOffset * 4 + myDrawInfo->myPixelOffset;
  // Periodic dump of original viewport content
  {
    static int frame_counter = 0;
    frame_counter++;
    if (frame_counter % 66 == 0 && frame_counter <= 66*30) {
      char fname[64];
      snprintf(fname, sizeof(fname), "/tmp/orig_viewport_f%d.pgm", frame_counter);
      FILE* f = fopen(fname, "wb");
      if (f) {
        fprintf(f, "P5\n320 176\n255\n");
        // Extract 320x176 from planar drawBuffer at offset
        for (int y = 0; y < 176; y++) {
          for (int x = 0; x < 320; x++) {
            uint8_t c = myDrawInfo->drawBuffer[offset + y * RENDER_WIDTH + x];
            fputc(c, f);
          }
        }
        fclose(f);
        printf("V2-DBG-ORIG: Saved viewport dump to %s (offset=%x)\n", fname, offset);
      }
    }
  }
  for (int i = 0; i < 176 * RENDER_WIDTH; i++)
  {
	//myDrawInfo->myOffset=0x5be8;
	//myDrawInfo->myOffset=0xa1c8;
	//myDrawInfo->myOffset=0x66a8;
	//myDrawInfo->myOffset=0;
	auto color = myDrawInfo->drawBuffer[offset + i];
	auto sdl_color = myDrawInfo->drawPalette[color];
	tempDrawBuffer[i + 0 * RENDER_WIDTH] = SDL_MapRGBA(myFormat, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
  }
  for (int i = 0; i < RENDER_WIDTH * (RENDER_HEIGHT - 176); i++)
  {
	auto color = myDrawInfo->drawBuffer[0 + 0 + i];
	auto sdl_color = myDrawInfo->drawPalette[color];
	tempDrawBuffer[i + 176 * RENDER_WIDTH] = SDL_MapRGBA(myFormat, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
  }
  SDL_UpdateTexture(myTexture, NULL, tempDrawBuffer, RENDER_WIDTH*sizeof(uint32_t));
  SDL_RenderClear(myRenderer);
  SDL_Rect srcRect = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
  SDL_RenderCopy(myRenderer, myTexture, &srcRect, NULL);
  SDL_RenderPresent(myRenderer);
}

  std::thread render_thread;
  void render_thread_proc(void* _state)
  {
	myDrawInfo = (myDrawInfoS *)calloc(1, sizeof(myDrawInfoS));
	assert(myDrawInfo);

	printf("render: Starting initialization...\n");
	if( SDL_Init( SDL_INIT_VIDEO ) < 0 )
    {
	  printf( "SDL could not initialize! SDL_Error: %s\n", SDL_GetError() );
    }
	printf("render: Creating window...\n");
#ifdef V2_ONLY
	// V2_ONLY: hide orig window — orig m2c renders to myDrawInfo->drawBuffer for v2's
	// drawBuffer mirror but orig window itself is unused.
	uint32_t _window_flags = SDL_WINDOW_HIDDEN;
#else
	uint32_t _window_flags = SDL_WINDOW_SHOWN;
#endif
	// Позиционируем в левый верхний угол
	myWindow = SDL_CreateWindow( "FFFF", 0, 0, SCREEN_WIDTH * SCREEN_SCALE, SCREEN_HEIGHT * SCREEN_SCALE, _window_flags );
		if( myWindow == NULL )
		{
			printf( "Window could not be created! SDL_Error: %s\n", SDL_GetError() );
		}
		else
		{
		  printf("render: Window created successfully!\n");
		  printf("render: Creating renderer...\n");
		  //struct m2c::_STATE state;
		  //struct m2c::_STATE *_state = &state;
			  //    X86_REGREF

			myRenderer = SDL_CreateRenderer(myWindow, -1, SDL_RENDERER_ACCELERATED);

			myTexture = SDL_CreateTexture(myRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, RENDER_WIDTH, RENDER_HEIGHT);

			myFormat = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);

		   printf("render: Entering main loop...\n");
		   while (!need_quit)
			{
			   SDL_Event event;
			   uint16_t key_val = 0;
			   while (SDL_PollEvent(&event) > 0) {
				 // Spec-flag DS offset (orig DOS keyboard ISR special-mode handlers
				 // at eip 0x648E..0x6517). 0 = no flag for this key. Set only on
				 // KEYDOWN. Game logic gates processing by other state.
				 uint16_t spec_off = 0;
				 switch (event.type) {
				 case SDL_KEYDOWN:
				 case SDL_KEYUP:
				   switch( event.key.keysym.sym ){
					 case SDLK_LEFT:
					   key_val = 0x200;
					   break;
					 case SDLK_RIGHT:
					   key_val = 0x100;
					   break;
					 case SDLK_UP:
					   key_val = 0x800;
					   break;
					 case SDLK_DOWN:
					   key_val = 0x400;
					   break;
					 case SDLK_SPACE:
					 case SDLK_RETURN:
					   key_val = 0x8000;
					   break;
				     case SDLK_LCTRL:
					 case SDLK_RCTRL:
					   key_val = 0x20;
					   spec_off = 0x9189;  // sc 0x1D byte_31669
					   break;
					 case SDLK_TAB:
					   key_val = 0x2000;
					   break;
					 case SDLK_e:
					   key_val = 0x40;
					   break;
					 case SDLK_s:
					   key_val = 0x80;
					   spec_off = 0x918B;  // sc 0x1F byte_3166b — mute SFX
					   break;
					 case SDLK_d:
					   key_val = 0x4000;
					   break;
					 case SDLK_f:
					   key_val = 0x8000;
					   break;
					 case SDLK_ESCAPE:
					   key_val = 0x1000;
					   break;
					 case SDLK_m:
					   spec_off = 0x919E;  // sc 0x32 byte_3167e — mute music
					   break;
					 case SDLK_x:
					   spec_off = 0x9199;  // sc 0x2D byte_31679
					   break;
					 case SDLK_LALT:
					 case SDLK_RALT:
					   spec_off = 0x91A4;  // sc 0x38 byte_31684
					   break;
					 case SDLK_F10:
					   spec_off = 0x91B0;  // sc 0x44 byte_31690
					   break;
					 case SDLK_DELETE:
					   spec_off = 0x91BF;  // sc 0x53 byte_3169f
					   break;
				     default:
					   key_val = 0;
					   break;
				   }
				   if (event.type == SDL_KEYDOWN) {
					 input_keys |= key_val;
					 input_keys_v2 |= key_val;
					 // spec_off DS writes removed — they caused races between render
					 // thread writes to real_ds vs shadow_ds. Orig design: int 9 ISR
					 // (seg000_6440_proc, dead in m2c port) writes spec bytes on press.
					 // Game logic reads them (e.g. sub_108c8 reads ds:0x918B for S key).
					 // Without ISR, spec bytes stay 0 → mute toggles etc don't fire.
					 // To re-enable: route through atomic + game thread copy at barrier.
				   } else {
					 input_keys &= ~key_val;
					 input_keys_v2 &= ~key_val;
				   }
				   // Spec key SDL state. Modifiers: KEYDOWN sets, KEYUP clears.
				   // Triggers: KEYDOWN sets only (snapshot consumes via exchange).
				   if (event.type == SDL_KEYDOWN) {
					 fprintf(stderr, "KEY-DBG sym=%d spec_off=0x%04X repeat=%d\n",
					         (int)event.key.keysym.sym, spec_off, (int)event.key.repeat);
				   }
				   if (spec_off) {
					 if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
					   sdl_spec_state[spec_off & 0xFF].store(1, std::memory_order_relaxed);
					 } else if (event.type == SDL_KEYUP && sdl_spec_is_modifier(spec_off)) {
					   // Modifier release: clear sticky bit so future trigger combos
					   // don't see ghost-held modifier.
					   sdl_spec_state[spec_off & 0xFF].store(0, std::memory_order_relaxed);
					 }
				   }
				   break;

				 case SDL_QUIT:
				   fprintf(stderr, "SDL_QUIT received — _exit\n");
				   fflush(stderr);
				   _exit(0);
				 case SDL_WINDOWEVENT:
				   if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
					 fprintf(stderr, "SDL_WINDOWEVENT_CLOSE (windowID=%u) — _exit\n", event.window.windowID);
					 fflush(stderr);
					 _exit(0);
				   }
				   break;
			      }
			   }
			   //printf("VGA pan: %x %x\n", myDrawInfo->myOffset, myDrawInfo->myPixelOffset);
			   updateDraw();
			   // RESTORED from orig: render_callback (sub_1797b) DECs word_3287C from
			   // render thread at ~60Hz. Without this, game thread sub_10130 sleeps
			   // 16ms each call (3+ per frame) → severe slowdown.
			   { extern std::atomic<int64_t> v2_dbg_render_callback_calls; v2_dbg_render_callback_calls++; }
#ifndef V2_ONLY
			   render_callback(_state);
#endif
			   SDL_Delay(15);
		   }
		}

  }

void render_init(void* state)
{
    render_thread = std::thread(render_thread_proc, state);
	render_thread.detach();
}
