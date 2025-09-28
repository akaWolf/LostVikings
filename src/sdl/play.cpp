#include <SDL2/SDL.h>
#include <thread>
#include <atomic>
#include <cstdio>

#include "adlmidi.h"

extern bool need_quit;

// Per-slot async close flag. Set by stop_xmidi_external(num) or stop_all_sfx().
// Audio callback reads this per-iteration; on true: closes player and resets to false.
// Atomic since written from main game thread, read from SDL audio thread.
static std::atomic<bool> need_close[100] = {};

// Currently-playing music slot (set by set_music_handle, used by audio_callback to
// scale volume and to skip in stop_all_sfx). -1 = no music.
static std::atomic<int> dontstop_num{-1};

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
	  printf("player: created %p %p %i\n", curr_player, xmidi, seq_num);
	  if (!curr_player)
	  {
		  fprintf(stderr, "Couldn't initialize ADLMIDI: %s\n", adl_errorString());
		  return;
	  }

	  adl_switchEmulator(curr_player, ADLMIDI_EMU_NUKED);

	  /* Set using of embedded bank by ID */
	  adl_setBank(curr_player, 75);

	  adl_setLoopEnabled(curr_player, seq_num == -1 ? 1 : 0);

	      /* Open the MIDI (or MUS, IMF or CMF) file to play */
	if (adl_openData(curr_player, xmidi, len) < 0)
    {
        fprintf(stderr, "Couldn't open music file: %s\n", adl_errorInfo(curr_player));
        //SDL_CloseAudio();
        adl_close(curr_player);
        return;
	}

	  if (seq_num != -1)
		adl_selectSongNum(curr_player, seq_num);
		//adl_setTrackOptions(curr_player, seq_num, ADLMIDI_TrackOption_Solo);

	*midi_player = curr_player;

    /* wait until we're don't playing */
    while (*midi_player && !need_quit)
    {
        SDL_Delay(100);
    }

	printf("player: exiting %p\n", curr_player);
}

int play_xmidi(struct ADL_MIDIPlayer** midi_players, const void* xmidi, uint32_t len, int seq_num)
{
  int num = -1;
  	for (int i = 0; i < 100; i++)
	{
	  // Pick any free slot (midi_players[i] == nullptr). need_close[i] could still
	  // be true from a recent stop request that the audio callback hasn't processed
	  // yet — we clear it BEFORE spawning the thread so the new player isn't
	  // immediately closed by stale flag.
	  if (midi_players[i] == nullptr)
	  {
		need_close[i].store(false);
		std::thread midi_thread(midi_thread_proc, &midi_players[i], xmidi, len, seq_num);
		midi_thread.detach();
		num = i;
		break;
	  }
	}

	return num;
}

int play_xmidi_external(const void* xmidi, uint32_t len, int seq_num)
{
  printf("request to play %p %d %d\n", xmidi, len, seq_num);
  return play_xmidi(midi_players, xmidi, len, seq_num);
}

// Stop a specific player by handle (from play_xmidi_external return value).
// Non-blocking: marks slot for close; audio_callback closes asynchronously
// on next tick. If the handle was the music handle, clear dontstop too.
void stop_xmidi_external(uint8_t num)
{
  if (num >= 100) return;
  printf("request to stop player %x %p\n", num, midi_players[num]);
  if (dontstop_num.load() == num) dontstop_num.store(-1);
  need_close[num].store(true);
}

// Stop ALL non-music SFX. Music (dontstop_num) is preserved.
// Non-blocking. Used for mute toggle (sub_108c8 SFX path), sub_1782a / sub_17912
// "stop all" semantics.
void stop_all_sfx()
{
  printf("request to stop all SFX\n");
  int dn = dontstop_num.load();
  for (int i = 0; i < 100; i++)
    if (i != dn) need_close[i].store(true);
}

// Backward-compat: legacy "stop all" entry. Same semantics as stop_all_sfx().
void stop_xmidi_external()
{
  stop_all_sfx();
}

// Mark a player as the "music" player. Audio callback scales its volume to
// 60% and stop_all_sfx skips it. Pass -1 to clear (no music).
void set_dontstop_external(uint8_t num)
{
  if (num >= 100) return;
  printf("dontstop player: %x %p\n", num, midi_players[num]);
  dontstop_num.store((int)num);
}

// Returns true if a player handle is currently playing audio (not yet exited).
// Used by sub_177bb slot allocation to find a slot that's "done" so we can
// reuse it without leaving an orphaned player.
bool is_player_active(int num)
{
  if (num < 0 || num >= 100) return false;
  return midi_players[num] != nullptr && !need_close[num].load();
}

void sound_init()
{
  printf("init sound\n");
    static SDL_AudioSpec spec, obtained;

	for (int i = 0; i < 100; i++) {
	  midi_players[i] = nullptr;
	  need_close[i].store(false);
	}

    if(SDL_Init(SDL_INIT_AUDIO) < 0)
        return;

    spec.freq = MYFREQ;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 64;

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

static uint8_t myBuffer[0x2000];

void my_audio_callback(void *argument, Uint8 *stream, int len)
{
  //printf("size %x\n", len);
  if (len > 0x2000) {
	printf("SOUND ERROR, len = %d!\n", len);
	len = 0x2000;
  }

  memset(myBuffer, 0, len);
  memset(buffer, 0, len);

    int samples_count = len / s_audioFormat.containerSize;

	struct ADL_MIDIPlayer** midi_players = (struct ADL_MIDIPlayer**)argument;

	uint8_t count = 0;
	uint8_t volume;
	int dn = dontstop_num.load();

	for (int i = 0; i < 100; i++)
	{
		if (!midi_players[i])
		  continue;

		// Per-slot async close request from main thread.
		if (need_close[i].load())
		  goto close;

		samples_count = adl_playFormat(midi_players[i], samples_count,
									   buffer,
									   buffer + s_audioFormat.containerSize,
									   &s_audioFormat);

		if(samples_count <= 0)
		  goto close;

		volume = SDL_MIX_MAXVOLUME;
		if (i == dn)
		  volume = SDL_MIX_MAXVOLUME * 0.6;

		SDL_MixAudioFormat(myBuffer, buffer, myFormat, samples_count * s_audioFormat.containerSize, volume);

		count++;

		continue;

	close:
		{
		  printf("player: closing %p\n", midi_players[i]);
		  adl_close(midi_players[i]);
		  midi_players[i] = nullptr;
		  need_close[i].store(false);
		  // If we just closed the music player, clear dontstop so future stop_all_sfx
		  // doesn't try to skip a freed slot.
		  if (dn == i) dontstop_num.store(-1);
		  continue;
		}

	}

	SDL_memcpy(stream, myBuffer, len);
	//printf("count = %x\n", count);
}
