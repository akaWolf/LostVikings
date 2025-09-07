#include <SDL2/SDL.h>
#include <thread>
#include <cassert>
#include <cstdio>

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
uint16_t input_keys = 0;
bool need_quit = false;


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
	// Позиционируем в левый верхний угол
	myWindow = SDL_CreateWindow( "FFFF", 0, 0, SCREEN_WIDTH * SCREEN_SCALE, SCREEN_HEIGHT * SCREEN_SCALE, SDL_WINDOW_SHOWN );
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
					   break;
					 case SDLK_TAB:
					   key_val = 0x2000;
					   break;
					 case SDLK_e:
					   key_val = 0x40;
					   break;
					 case SDLK_s:
					   key_val = 0x80;
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
				     default:
					   key_val = 0;
					   break;
				   }
				   if (event.type == SDL_KEYDOWN)
					 input_keys |= key_val;
				   else
					 input_keys &= ~key_val;
				   break;

				 case SDL_QUIT:
				   need_quit = true;
				   printf("quitting\n");
				   //return;
			      }
			   }
			   //printf("VGA pan: %x %x\n", myDrawInfo->myOffset, myDrawInfo->myPixelOffset);
			   updateDraw();
			   //SDL_Delay(20);
			   render_callback(_state);
			   //std::this_thread::sleep_for(std::chrono::milliseconds(15));
			   SDL_Delay(15);
		   }
		}

  }

void render_init(void* state)
{
    render_thread = std::thread(render_thread_proc, state);
	render_thread.detach();
}
