#include <SDL2/SDL.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include <chrono>

#include "adlmidi.h"

// Time-since-program-start in ms — for diagnostic timestamps in sound logs.
static auto _sound_t0 = std::chrono::steady_clock::now();
static uint32_t _sound_now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - _sound_t0).count();
}

// First-tick diagnostic: tracks when audio_callback first sees samples from each
// slot. Reset on close so reused slots re-log latency.
bool _sound_first_tick[100] = {};
void _sound_reset_first_tick(int i) { if (i >= 0 && i < 100) _sound_first_tick[i] = false; }

// Per-slot saved XMI buffer pointer + length, captured at play_xmidi time.
// Used to RE-OPEN the XMI when music track ends naturally (samples=0). Original
// DOS AIL handles XMI Branch (CC 0x77) loop events; adlmidi doesn't support them
// for these files. This is a workaround: full adl_openData re-init = same effect
// as restarting music from beginning, mimicking AIL's loop behavior.
const void* _sound_xmi_buf[100] = {};
uint32_t    _sound_xmi_len[100] = {};
int         _sound_xmi_seq[100] = {};

// Per-slot fade-out state. fade_volume is current scale (1.0 = full, 0.0 = silent).
// fade_step is per-callback decrement (positive). When fade_volume reaches 0.0,
// slot is marked for close. Used by fade_music() to gradually fade music out
// (orig sub_178f1 calls AIL fade over 1 sec for level transitions).
static std::atomic<float> _sound_fade_volume[100];
static std::atomic<float> _sound_fade_step[100];

extern bool need_quit;

// Per-slot async close flag. Set by stop_xmidi_external(num) or stop_all_sfx().
// Audio callback reads this per-iteration; on true: closes player and resets to false.
// Atomic since written from main game thread, read from SDL audio thread.
static std::atomic<bool> need_close[100] = {};

// Currently-playing music handle (unique ID, set by set_dontstop_external).
// 0 = no music. Used by stop_all_sfx (to skip music) and audio_callback volume.
static std::atomic<uint16_t> dontstop_handle{0};

// Unique handle table — mirrors orig AIL design. Each play_xmidi assigns a fresh
// handle (1..0xFFFE; 0 and 0xFFFF reserved as "free" sentinels). slot_handle[i]
// is the handle currently playing in slot i, or 0 if free. After natural-end or
// stop, slot_handle[i] = 0 — any subsequent stop_xmidi_external(stale_handle)
// scans the table, finds no match, no-op. This mirrors AIL's "stop accepts old
// handle silently" behavior — game stores handle in ds:[si-0x66F4] and never
// clears it, but a stale handle pointing to a freed sound is safely ignored.
static std::atomic<uint16_t> slot_handle[100] = {};
static std::atomic<uint16_t> g_next_handle{1};

static int slot_for_handle(uint16_t h) {
    if (h == 0 || h == 0xFFFF) return -1;
    for (int i = 0; i < 100; i++)
        if (slot_handle[i].load(std::memory_order_relaxed) == h) return i;
    return -1;
}

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

	{
	  int songs = adl_getSongsCount(curr_player);
	  double total_sec = adl_totalTimeLength(curr_player);
	  double loopstart = adl_loopStartTime(curr_player);
	  printf("[%ums] SOUND-OPEN: player=%p len=%u seq=%d songs=%d total_time=%.2fs loopStart=%.2fs\n",
	         _sound_now_ms(), (void*)curr_player, len, seq_num, songs, total_sec, loopstart);
	}
		//adl_setTrackOptions(curr_player, seq_num, ADLMIDI_TrackOption_Solo);

	*midi_player = curr_player;

    /* wait until we're don't playing */
    while (*midi_player && !need_quit)
    {
        SDL_Delay(100);
    }

	printf("player: exiting %p\n", curr_player);
}

// Returns unique 16-bit handle (1..0xFFFE). 0 = play failed, 0xFFFF reserved.
static uint16_t alloc_handle() {
    uint16_t h;
    do {
        h = g_next_handle.fetch_add(1, std::memory_order_relaxed);
        if (h == 0 || h == 0xFFFF) continue;  // skip reserved
        break;
    } while (true);
    return h;
}

int play_xmidi(struct ADL_MIDIPlayer** midi_players, const void* xmidi, uint32_t len, int seq_num)
{
  int slot = -1;
  uint16_t handle = 0;
  for (int i = 0; i < 100; i++)
  {
	// Pick any free slot (midi_players[i] == nullptr). need_close[i] could still
	// be true from a recent stop request that the audio callback hasn't processed
	// yet — we clear it BEFORE spawning the thread so the new player isn't
	// immediately closed by stale flag.
	if (midi_players[i] == nullptr)
	{
	  bool stale = need_close[i].load();
	  need_close[i].store(false);
	  // Reset fade state for this slot (no fade in progress for new player).
	  _sound_fade_volume[i].store(1.0f);
	  _sound_fade_step[i].store(0.0f);
	  // Save XMI ptr+len+seq for later reload-on-end (used by audio_callback
	  // to re-init music when adlmidi reaches end-of-track).
	  _sound_xmi_buf[i] = xmidi;
	  _sound_xmi_len[i] = len;
	  _sound_xmi_seq[i] = seq_num;
	  // Assign fresh unique handle (mirrors AIL: every play gets a new handle).
	  handle = alloc_handle();
	  slot_handle[i].store(handle, std::memory_order_relaxed);
	  std::thread midi_thread(midi_thread_proc, &midi_players[i], xmidi, len, seq_num);
	  midi_thread.detach();
	  printf("[%ums] SOUND-PLAY: slot=%d handle=%04X seq=%d len=%u%s\n",
	         _sound_now_ms(), i, handle, seq_num, len, stale ? " (cleared stale need_close)" : "");
	  slot = i;
	  break;
	}
  }
  if (slot < 0) {
	printf("[%ums] SOUND-PLAY-FAIL: no free slot (all 100 occupied!)\n", _sound_now_ms());
	return 0;  // 0 = no handle, mirrors AIL "play failed"
  }
  return (int)handle;
}

int play_xmidi_external(const void* xmidi, uint32_t len, int seq_num)
{
  printf("request to play %p %d %d\n", xmidi, len, seq_num);
  return play_xmidi(midi_players, xmidi, len, seq_num);
}

// Stop a specific player by HANDLE (unique ID from play_xmidi_external).
// Non-blocking: marks slot for close; audio_callback closes asynchronously.
// Stale handle (slot was reused for another sound) → no-op, mirroring AIL.
void stop_xmidi_external(uint16_t handle)
{
  if (handle == 0 || handle == 0xFFFF) return;
  int slot = slot_for_handle(handle);
  if (slot < 0) {
    // Stale handle — sound already ended naturally or was stopped. AIL would
    // silently no-op here. We log for diagnostics but otherwise do nothing.
    printf("[%ums] SOUND-STOP: handle=%04X stale (no slot) — no-op\n",
           _sound_now_ms(), handle);
    return;
  }
  uint16_t dn = dontstop_handle.load();
  printf("[%ums] SOUND-STOP: handle=%04X slot=%d player=%p (dontstop=%04X)\n",
         _sound_now_ms(), handle, slot, (void*)midi_players[slot], dn);
  if (dn == handle) dontstop_handle.store(0);
  need_close[slot].store(true);
}

// Stop ALL non-music SFX. Music (dontstop_handle) is preserved.
// Non-blocking. Used for mute toggle (sub_108c8 SFX path), sub_1782a / sub_17912
// "stop all" semantics.
void stop_all_sfx()
{
  uint16_t dn = dontstop_handle.load();
  int dn_slot = slot_for_handle(dn);
  int marked = 0;
  for (int i = 0; i < 100; i++) {
    if (i != dn_slot && midi_players[i] != nullptr) {
      need_close[i].store(true);
      marked++;
    }
  }
  printf("[%ums] SOUND-STOP-ALL: dontstop=%04X marked %d active slots\n",
         _sound_now_ms(), dn, marked);
}

// Backward-compat: legacy "stop all" entry. Same semantics as stop_all_sfx().
void stop_xmidi_external()
{
  stop_all_sfx();
}

// Fade music out over duration_ms milliseconds. After fade completes, the
// music slot is closed automatically by audio_callback. AIL DOS sub_1C7BD
// (set_sequence_tempo) was used for fades in original — we approximate via
// volume ramp in audio callback. duration_ms <= 0 = immediate stop.
void fade_music(int duration_ms)
{
  uint16_t dn = dontstop_handle.load();
  int slot = slot_for_handle(dn);
  if (slot < 0) {
    printf("[%ums] SOUND-FADE: no music to fade\n", _sound_now_ms());
    return;
  }
  if (duration_ms <= 0) {
    printf("[%ums] SOUND-FADE: duration<=0 → immediate stop_xmidi_external(%04X)\n",
           _sound_now_ms(), dn);
    stop_xmidi_external(dn);
    return;
  }
  const float ticks_per_sec = (float)MYFREQ / 64.0f;
  const float total_ticks   = (duration_ms / 1000.0f) * ticks_per_sec;
  const float step          = (total_ticks > 0) ? (1.0f / total_ticks) : 1.0f;
  _sound_fade_volume[slot].store(1.0f);
  _sound_fade_step[slot].store(step);
  printf("[%ums] SOUND-FADE: handle=%04X slot=%d duration=%dms step=%.5f/tick (%.0f ticks)\n",
         _sound_now_ms(), dn, slot, duration_ms, step, total_ticks);
}

// Mark a player as the "music" player by HANDLE. Audio callback scales its
// volume to 60% and stop_all_sfx skips its slot.
void set_dontstop_external(uint16_t handle)
{
  if (handle == 0 || handle == 0xFFFF) return;
  int slot = slot_for_handle(handle);
  if (slot < 0) {
    printf("[%ums] SOUND-DONTSTOP: handle=%04X stale (no slot) — no-op\n",
           _sound_now_ms(), handle);
    return;
  }
  bool was_marked = need_close[slot].load();
  printf("[%ums] SOUND-DONTSTOP: handle=%04X slot=%d player=%p%s\n",
         _sound_now_ms(), handle, slot, (void*)midi_players[slot],
         was_marked ? " (cleared stale need_close — music save)" : "");
  dontstop_handle.store(handle);
  need_close[slot].store(false);
}

bool is_player_active(uint16_t handle)
{
  int slot = slot_for_handle(handle);
  if (slot < 0) return false;
  return midi_players[slot] != nullptr && !need_close[slot].load();
}

// Returns the current music HANDLE (or 0 if none).
uint16_t get_music_handle()
{
  return dontstop_handle.load();
}

void sound_init()
{
  printf("init sound\n");
    static SDL_AudioSpec spec, obtained;

	for (int i = 0; i < 100; i++) {
	  midi_players[i] = nullptr;
	  need_close[i].store(false);
	  _sound_fade_volume[i].store(1.0f);
	  _sound_fade_step[i].store(0.0f);
	  slot_handle[i].store(0);
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

    const int requested_samples = len / s_audioFormat.containerSize;

	struct ADL_MIDIPlayer** midi_players = (struct ADL_MIDIPlayer**)argument;

	uint8_t count = 0;
	uint8_t volume;
	uint16_t dn = dontstop_handle.load();
	int dn_slot = slot_for_handle(dn);
	int samples_count = 0;

	for (int i = 0; i < 100; i++)
	{
		if (!midi_players[i])
		  continue;

		// Per-slot async close request from main thread.
		if (need_close[i].load())
		  goto close;

		// BUG FIX: samples_count must be RESET to requested per-iteration.
		// Old code reused samples_count across iterations: if first player
		// returned 0 (track ended), subsequent calls got passed 0 → all
		// returned 0 → all closed prematurely. Music would die after one
		// adl_playFormat call returned 0 even though track had 28-45 sec left.
		samples_count = adl_playFormat(midi_players[i], requested_samples,
									   buffer,
									   buffer + s_audioFormat.containerSize,
									   &s_audioFormat);

		// First-tick log for each player: when did audio_callback first see
		// samples from this slot? Helps detect adlmidi init latency. Reset on close.
		extern bool _sound_first_tick[100];
		if (!_sound_first_tick[i]) {
		  _sound_first_tick[i] = true;
		  printf("[%ums] SOUND-FIRST-TICK: slot=%d samples=%d (adlmidi init latency observed)\n",
		         _sound_now_ms(), i, samples_count);
		}
		if (samples_count <= 0) {
		  printf("[%ums] SOUND-NO-SAMPLES: slot=%d (samples=%d) — closing as natural-end%s\n",
		         _sound_now_ms(), i, samples_count, (i == dn_slot) ? " (was music)" : "");
		  _sound_first_tick[i] = false;
		  goto close;
		}


		volume = SDL_MIX_MAXVOLUME;
		if (i == dn_slot)
		  volume = SDL_MIX_MAXVOLUME * 0.6;

		{
		  // Fade-out: per-callback decrement of volume scale. When reaches <= 0,
		  // mark slot for close so audio thread closes it on next iter.
		  float fade_v = _sound_fade_volume[i].load();
		  float fade_s = _sound_fade_step[i].load();
		  if (fade_s > 0.0f) {
		    fade_v -= fade_s;
		    if (fade_v <= 0.0f) {
		      fade_v = 0.0f;
		      _sound_fade_step[i].store(0.0f);
		      need_close[i].store(true);
		      printf("[%ums] SOUND-FADE-DONE: slot=%d → marking need_close\n",
		             _sound_now_ms(), i);
		    }
		    _sound_fade_volume[i].store(fade_v);
		    volume = (uint8_t)(volume * fade_v);
		  }
		}

		{
		  // Mix: samples_count is per-slot return; convert to bytes via container size.
		  int mix_bytes = samples_count * s_audioFormat.containerSize;
		  if (mix_bytes > len) mix_bytes = len;
		  SDL_MixAudioFormat(myBuffer, buffer, myFormat, mix_bytes, volume);
		}

		count++;

		continue;

	close:
		{
		  // Why-closing diagnostic: distinguish need_close request from natural end.
		  const char* reason = need_close[i].load() ? "stop-request" : "natural-end";
		  printf("[%ums] SOUND-CLOSE: slot=%d handle=%04X player=%p reason=%s%s\n",
		         _sound_now_ms(), i, slot_handle[i].load(), (void*)midi_players[i], reason,
		         (i == dn_slot) ? " (was music!)" : "");
		  adl_close(midi_players[i]);
		  midi_players[i] = nullptr;
		  need_close[i].store(false);
		  // Clear unique handle mapping — any stale handle stored elsewhere
		  // (in DS slots) will now fail slot_for_handle() lookup → no-op stop.
		  // This is the core mechanism that makes the unique-handle design
		  // mirror AIL's "stop with stale handle = no-op" behavior.
		  slot_handle[i].store(0, std::memory_order_relaxed);
		  // Reset first-tick diagnostic so next play in this slot re-logs.
		  extern void _sound_reset_first_tick(int);
		  _sound_reset_first_tick(i);
		  // If we just closed the music player, clear dontstop so future stop_all_sfx
		  // doesn't try to skip a freed slot.
		  if (i == dn_slot) dontstop_handle.store(0);
		  continue;
		}

	}

	SDL_memcpy(stream, myBuffer, len);
	//printf("count = %x\n", count);
}
