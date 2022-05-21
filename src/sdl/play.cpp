#include <SDL2/SDL.h>
#include <thread>

#include "adlmidi.h"

extern bool need_quit;
static bool need_stop = false;
static int num_to_stop = -1;
static int dontstop_num = -1;

void my_audio_callback(void *midi_player, Uint8 *stream, int len);

static Uint8 buffer[8192]; /* Audio buffer */
static struct ADLMIDI_AudioFormat s_audioFormat;
static SDL_AudioFormat myFormat;
const uint32_t MYFREQ = 44100;

static struct ADL_MIDIPlayer    *midi_players[100]; /* Instance of ADLMIDI player */


void midi_thread_proc(struct ADL_MIDIPlayer** midi_player, const void* xmidi, uint32_t len, int seq_num)
{
  	  /* Initialize ADLMIDI */
      auto curr_player = adl_init(MYFREQ);
	  printf("player: %p %p %i\n", curr_player, xmidi, seq_num);
	  if (!curr_player)
	  {
		  fprintf(stderr, "Couldn't initialize ADLMIDI: %s\n", adl_errorString());
		  return;
	  }

	  adl_switchEmulator(curr_player, ADLMIDI_EMU_NUKED);

	  /* Set using of embedded bank by ID */
	  adl_setBank(curr_player, 75);

	  adl_setLoopEnabled(curr_player, 0);

	      /* Open the MIDI (or MUS, IMF or CMF) file to play */
	if (adl_openData(curr_player, xmidi, len) < 0)
    {
        fprintf(stderr, "Couldn't open music file: %s\n", adl_errorInfo(curr_player));
        //SDL_CloseAudio();
        adl_close(curr_player);
        return;
	}

	  if (seq_num != -1)
		adl_setTrackOptions(curr_player, seq_num, ADLMIDI_TrackOption_Solo);

	*midi_player = curr_player;

    /* wait until we're don't playing */
    while (*midi_player && !need_quit)
    {
        SDL_Delay(100);
    }

	printf("exiting %p\n", curr_player);
}

int play_xmidi(struct ADL_MIDIPlayer** midi_players, const void* xmidi, uint32_t len, int seq_num)
{
  int num = -1;
  	for (int i = 0; i < 100; i++)
	{
	  if (midi_players[i] == nullptr)
	  {
		std::thread midi_thread(midi_thread_proc, &midi_players[i], xmidi, len, seq_num);
		midi_thread.detach();
		//SDL_Delay(10);
		num = i;
		break;
	  }
	}

	need_stop = false;

	return num;
}

int play_xmidi_external(const void* xmidi, uint32_t len, int seq_num)
{
  printf("request to play %p %d %d\n", xmidi, len, seq_num);
  return play_xmidi(midi_players, xmidi, len, seq_num);
}

void stop_xmidi_external()
{
  need_stop = true;
}

void stop_xmidi_external(uint8_t num)
{
  printf("request to stop %x %p\n", num, midi_players[num]);
  if (midi_players[num] != nullptr)
	num_to_stop = num;
  while (midi_players[num] != nullptr)
	SDL_Delay(10);
  if (dontstop_num == num)
	dontstop_num = -1;
}

void set_dontstop_external(uint8_t num)
{
  dontstop_num = num;
}

void sound_init()
{
    static SDL_AudioSpec spec, obtained;

	for (int i = 0; i < 100; i++)
	  midi_players[i] = nullptr;

    if(SDL_Init(SDL_INIT_AUDIO) < 0)
        return;

    spec.freq = MYFREQ;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 2048;

    spec.callback = my_audio_callback;
    spec.userdata = midi_players;

    if (SDL_OpenAudio(&spec, &obtained) < 0)
    {
        fprintf(stderr, "Couldn't open audio: %s\n", SDL_GetError());
        return;
    }

	myFormat = obtained.format;

    switch(obtained.format)
    {
    case AUDIO_S8:
        s_audioFormat.type = ADLMIDI_SampleType_S8;
        s_audioFormat.containerSize = sizeof(int8_t);
        s_audioFormat.sampleOffset = sizeof(int8_t) * 2;
        break;
    case AUDIO_U8:
        s_audioFormat.type = ADLMIDI_SampleType_U8;
        s_audioFormat.containerSize = sizeof(uint8_t);
        s_audioFormat.sampleOffset = sizeof(uint8_t) * 2;
        break;
    case AUDIO_S16:
        s_audioFormat.type = ADLMIDI_SampleType_S16;
        s_audioFormat.containerSize = sizeof(int16_t);
        s_audioFormat.sampleOffset = sizeof(int16_t) * 2;
        break;
    case AUDIO_U16:
        s_audioFormat.type = ADLMIDI_SampleType_U16;
        s_audioFormat.containerSize = sizeof(uint16_t);
        s_audioFormat.sampleOffset = sizeof(uint16_t) * 2;
        break;
    case AUDIO_S32:
        s_audioFormat.type = ADLMIDI_SampleType_S32;
        s_audioFormat.containerSize = sizeof(int32_t);
        s_audioFormat.sampleOffset = sizeof(int32_t) * 2;
        break;
    case AUDIO_F32:
        s_audioFormat.type = ADLMIDI_SampleType_F32;
        s_audioFormat.containerSize = sizeof(float);
        s_audioFormat.sampleOffset = sizeof(float) * 2;
        break;
    }

	/*const char* str_list[] = {"/home/akawolf/sources/libADLMIDI/music/TITLE.XMI", "/home/akawolf/sources/libADLMIDI/music/WARP.XMI"};
	for (int i = 0; i < 2; i++)
	{
	  play_xmidi(midi_players, str_list[i], -1);
	}
	play_xmidi(midi_players, "/home/akawolf/tmp/lostviking/chunks/520_208.bin", 30);*/

    SDL_PauseAudio(0);
    //SDL_CloseAudio();
}

static uint8_t myBuffer[0x1000];

void my_audio_callback(void *argument, Uint8 *stream, int len)
{
  //printf("size %x\n", len);
  if (len > 0x1000) {
	printf("SOUND ERROR!\n");
	len = 0x1000;
  }

  memset(myBuffer, 0, len);
  memset(buffer, 0, len);

    int samples_count = len / s_audioFormat.containerSize;

	struct ADL_MIDIPlayer** midi_players = (struct ADL_MIDIPlayer**)argument;

	uint8_t count = 0;

	for (int i = 0; i < 100; i++)
	{
		if (!midi_players[i])
		  continue;

		if (need_stop && (i != dontstop_num))
		  goto close;

		if (i == num_to_stop)
		{
		  goto close;
		}

		samples_count = adl_playFormat(midi_players[i], samples_count,
									   buffer,
									   buffer + s_audioFormat.containerSize,
									   &s_audioFormat);

		if(samples_count <= 0)
		  goto close;

		SDL_MixAudioFormat(myBuffer, buffer, myFormat, samples_count * s_audioFormat.containerSize, SDL_MIX_MAXVOLUME);

		count++;

		continue;

	close:
		{
		  printf("closing %p\n", midi_players[i]);
		  adl_close(midi_players[i]);
		  midi_players[i] = nullptr;
		  if (num_to_stop == i)
			num_to_stop = -1;
		  continue;
		}

	}

	SDL_memcpy(stream, myBuffer, len);
	//printf("count = %x\n", count);
}
