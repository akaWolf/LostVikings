#include <SDL2/SDL.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <cstring>

#include "adlmidi.h"

// ============================================================================
// DECOUPLED MIXER ARCHITECTURE (per AUDIO_IMPL_ANALYSIS.md Option C)
// ============================================================================
// Each slot has:
//   - SPSC lock-free ring buffer (~46ms ahead capacity)
//   - Worker thread that continuously fills ring via adl_playFormat
//   - Audio callback: reads from ring, mixes (cheap memcpy + SDL_MixAudioFormat)
//
// Music slot has its OWN worker → SFX CPU spike doesn't starve music.
// All sounds play (no cap, no stealing) — only constraint is per-slot worker
// keeping up with sample consumption rate.
// ============================================================================

// SPSC ring buffer for int16 samples. Single producer (worker thread), single
// consumer (audio callback). Lock-free via atomic positions.
// CAPACITY = 8192 int16 = 4096 stereo frames = ~92ms @ 44100Hz. Provides enough
// headroom to absorb worker thread scheduling jitter.
template<size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;
    int16_t buffer[Capacity];
    alignas(64) std::atomic<size_t> write_pos{0};
    alignas(64) std::atomic<size_t> read_pos{0};
public:
    // Producer side: write up to count samples. Returns # actually written.
    size_t write(const int16_t* data, size_t count) {
        size_t w = write_pos.load(std::memory_order_relaxed);
        size_t r = read_pos.load(std::memory_order_acquire);
        size_t used = w - r;
        size_t free_space = Capacity - 1 - used;
        size_t n = (count < free_space) ? count : free_space;
        for (size_t i = 0; i < n; i++) buffer[(w + i) & MASK] = data[i];
        write_pos.store(w + n, std::memory_order_release);
        return n;
    }
    // Consumer side: read up to count samples. Returns # actually read.
    size_t read(int16_t* dest, size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        size_t avail = w - r;
        size_t n = (count < avail) ? count : avail;
        for (size_t i = 0; i < n; i++) dest[i] = buffer[(r + i) & MASK];
        read_pos.store(r + n, std::memory_order_release);
        return n;
    }
    // Producer side: free space in samples.
    size_t free_space() const {
        size_t w = write_pos.load(std::memory_order_relaxed);
        size_t r = read_pos.load(std::memory_order_acquire);
        return Capacity - 1 - (w - r);
    }
    // Consumer side: samples available.
    size_t available() const {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        return w - r;
    }
    // Reset to empty (only safe when producer + consumer both idle).
    void reset() {
        read_pos.store(0, std::memory_order_relaxed);
        write_pos.store(0, std::memory_order_relaxed);
    }
};

// Per-slot ring buffer (4096 stereo frames = ~92ms ahead).
static SpscRing<8192> g_rings[100];

// Per-slot state for decoupled mixer:
//   stop_requested: set by stop_xmidi_external/main thread; worker checks each loop.
//   producer_alive: true while worker thread running; false after worker exits.
//   player_active:  true while logical playback active (set by play_xmidi, cleared
//                   by audio_callback after ring drained AND producer dead).
static std::atomic<bool> g_stop_requested[100] = {};
static std::atomic<bool> g_producer_alive[100] = {};
static std::atomic<bool> g_player_active[100] = {};
// Muted slots: reserved for tracking only (no producer thread, no audio).
// Used by v2 in default mode for DS verify symmetry. audio_callback skips
// natural-end SLOT-DRAIN for muted slots — they only close on explicit
// stop_xmidi_external (need_close=true).
static std::atomic<bool> g_slot_muted[100] = {};

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

// Iterate ALL slots matching handle. Multiple slots may share a handle in default
// mode where orig (real producer) and v2 (muted reservation) both reserve a slot
// with the same deterministic handle. Operations like stop/fade/set_dontstop must
// affect all of them; muted slots have no audio so are no-op for audio side.
template<class Fn>
static int for_each_slot_with_handle(uint16_t h, Fn&& fn) {
    if (h == 0 || h == 0xFFFF) return 0;
    int matched = 0;
    for (int i = 0; i < 100; i++) {
        if (slot_handle[i].load(std::memory_order_relaxed) == h) { fn(i); matched++; }
    }
    return matched;
}

void my_audio_callback(void *midi_player, Uint8 *stream, int len);

static Uint8 buffer[16384]; /* Audio buffer (cherry-pick 3f2114a — fix audio glitches) */
static struct ADLMIDI_AudioFormat s_audioFormat;
static SDL_AudioFormat myFormat;
const uint32_t MYFREQ = 44100;

static struct ADL_MIDIPlayer    *midi_players[100]; /* Instance of ADLMIDI player */


// Decoupled mixer producer thread. Owns the ADL_MIDIPlayer for slot `i`.
// Continuously generates samples via adl_playFormat into per-slot ring buffer.
// Audio callback consumes from ring asynchronously — never blocks producer.
//
// Lifecycle:
//   1. Init adlmidi + load XMI
//   2. Publish player into midi_players[i] (signal audio callback can use slot)
//   3. Loop: generate CHUNK samples → write to ring (with throttle when full)
//   4. Exit on stop_requested OR adl_playFormat returns 0 (natural end)
//   5. Close player; clear midi_players[i]; mark producer_alive=false
// Audio callback drains remaining ring data, then sets player_active=false.
void midi_thread_proc(struct ADL_MIDIPlayer** midi_player, const void* xmidi, uint32_t len, int seq_num)
{
	// Find slot index from midi_player pointer (used for ring/atomic access).
	int slot = -1;
	extern struct ADL_MIDIPlayer* midi_players[];
	for (int i = 0; i < 100; i++) if (&midi_players[i] == midi_player) { slot = i; break; }
	if (slot < 0) {
		fprintf(stderr, "midi_thread_proc: BUG — slot not found for player ptr %p\n", (void*)midi_player);
		g_producer_alive[0].store(false);  // safety
		return;
	}

	auto _t0 = std::chrono::steady_clock::now();
	auto curr_player = adl_init(MYFREQ);
	auto _t1 = std::chrono::steady_clock::now();
	printf("[%ums] SOUND-PRODUCER-START: slot=%d player=%p xmidi=%p seq=%d\n",
	       _sound_now_ms(), slot, (void*)curr_player, xmidi, seq_num);
	if (!curr_player) {
		fprintf(stderr, "Couldn't initialize ADLMIDI: %s\n", adl_errorString());
		g_producer_alive[slot].store(false, std::memory_order_release);
		g_player_active[slot].store(false, std::memory_order_release);
		return;
	}

	adl_switchEmulator(curr_player, ADLMIDI_EMU_NUKED);
	adl_setBank(curr_player, 75);
	adl_setLoopEnabled(curr_player, seq_num == -1 ? 1 : 0);
	auto _t2 = std::chrono::steady_clock::now();

	if (adl_openData(curr_player, xmidi, len) < 0) {
		fprintf(stderr, "Couldn't open music file: %s\n", adl_errorInfo(curr_player));
		adl_close(curr_player);
		g_producer_alive[slot].store(false, std::memory_order_release);
		g_player_active[slot].store(false, std::memory_order_release);
		return;
	}

	if (seq_num != -1) adl_selectSongNum(curr_player, seq_num);

	auto _t3 = std::chrono::steady_clock::now();
	{
		int songs = adl_getSongsCount(curr_player);
		double total_sec = adl_totalTimeLength(curr_player);
		double loopstart = adl_loopStartTime(curr_player);
		double init_us = std::chrono::duration<double, std::micro>(_t1 - _t0).count();
		double cfg_us  = std::chrono::duration<double, std::micro>(_t2 - _t1).count();
		double open_us = std::chrono::duration<double, std::micro>(_t3 - _t2).count();
		printf("[%ums] SOUND-OPEN: slot=%d player=%p len=%u seq=%d songs=%d total_time=%.2fs loopStart=%.2fs "
		       "init=%.0fμs cfg=%.0fμs openData=%.0fμs total=%.0fμs\n",
		       _sound_now_ms(), slot, (void*)curr_player, len, seq_num, songs, total_sec, loopstart,
		       init_us, cfg_us, open_us, init_us + cfg_us + open_us);
	}

	// Publish player. Audio callback was checking player_active (set by play_xmidi)
	// but waited until midi_players[slot] != nullptr to actually consume. Order:
	// store player ptr last so consumer sees fully-init player.
	*midi_player = curr_player;

	// Producer loop: generate CHUNK samples per iteration. CHUNK chosen to be
	// small enough to react quickly to stop_requested but large enough to
	// amortize adl_playFormat overhead. 1024 stereo frames = 2048 int16 samples.
	constexpr int CHUNK_FRAMES = 1024;
	constexpr int CHUNK_SAMPLES = CHUNK_FRAMES * 2;  // stereo: 2 int16 per frame
	int16_t local[CHUNK_SAMPLES];

	while (!g_stop_requested[slot].load(std::memory_order_acquire) && !need_quit) {
		// Throttle: wait if ring doesn't have room for another chunk.
		while (g_rings[slot].free_space() < (size_t)CHUNK_SAMPLES) {
			if (g_stop_requested[slot].load(std::memory_order_acquire) || need_quit) goto exit_loop;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}

		// adl_playFormat sampleCount = total samples L+R interleaved (matches
		// existing audio_callback convention from before refactor).
		int got = adl_playFormat(curr_player, CHUNK_SAMPLES,
		                         (Uint8*)local,
		                         (Uint8*)local + s_audioFormat.containerSize,
		                         &s_audioFormat);
		if (got <= 0) {
			// Sequence ended naturally. For music (seq_num == -1) with loop enabled
			// adlmidi handles loop internally — got<=0 means truly done. For SFX,
			// natural end after one play.
			printf("[%ums] SOUND-PRODUCER-END: slot=%d (natural end, got=%d)\n",
			       _sound_now_ms(), slot, got);
			break;
		}

		// Write to ring. Loop in case worker generated more than ring can take
		// in one go (shouldn't happen since we checked free_space, but defensive).
		size_t written = 0;
		while (written < (size_t)got) {
			size_t w = g_rings[slot].write(local + written, got - written);
			written += w;
			if (written < (size_t)got) {
				if (g_stop_requested[slot].load(std::memory_order_acquire) || need_quit) goto exit_loop;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	}
exit_loop:

	printf("[%ums] SOUND-PRODUCER-EXIT: slot=%d stop=%d quit=%d\n",
	       _sound_now_ms(), slot,
	       (int)g_stop_requested[slot].load(), (int)need_quit);

	// Cleanup: close player. Audio callback sees midi_players[slot]=nullptr +
	// producer_alive=false → drains remaining ring → marks player_active=false.
	*midi_player = nullptr;
	adl_close(curr_player);
	g_producer_alive[slot].store(false, std::memory_order_release);
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

// Generalized play. Either auto-allocates handle (handle_in==0) or uses given.
// mute=true: reserves slot for tracking but doesn't spawn producer thread / no
// adl_init / no real playback. Used by v2_sub_177bb_v2 in default mode where
// orig handles real audio output but v2 still needs slot tracking for DS verify.
int play_xmidi_with_handle_and_mute(struct ADL_MIDIPlayer** midi_players,
                                     const void* xmidi, uint32_t len, int seq_num,
                                     uint16_t handle_in, bool mute)
{
  int slot = -1;
  uint16_t handle = 0;
  for (int i = 0; i < 100; i++)
  {
	if (midi_players[i] == nullptr && !g_player_active[i].load(std::memory_order_acquire))
	{
	  bool stale = need_close[i].load();
	  need_close[i].store(false);
	  g_stop_requested[i].store(false, std::memory_order_relaxed);
	  g_rings[i].reset();
	  g_producer_alive[i].store(!mute, std::memory_order_relaxed);  // mute → no producer
	  g_slot_muted[i].store(mute, std::memory_order_relaxed);
	  g_player_active[i].store(true, std::memory_order_release);
	  _sound_fade_volume[i].store(1.0f);
	  _sound_fade_step[i].store(0.0f);
	  _sound_xmi_buf[i] = xmidi;
	  _sound_xmi_len[i] = len;
	  _sound_xmi_seq[i] = seq_num;
	  // Use given handle (deterministic from audit) or auto-allocate.
	  handle = (handle_in != 0 && handle_in != 0xFFFF) ? handle_in : alloc_handle();
	  slot_handle[i].store(handle, std::memory_order_relaxed);
	  if (!mute) {
	    std::thread midi_thread(midi_thread_proc, &midi_players[i], xmidi, len, seq_num);
	    midi_thread.detach();
	  }
	  printf("[%ums] SOUND-PLAY%s: slot=%d handle=%04X seq=%d len=%u%s\n",
	         _sound_now_ms(), mute ? "-MUTE" : "", i, handle, seq_num, len,
	         stale ? " (cleared stale need_close)" : "");
	  slot = i;
	  break;
	}
  }
  if (slot < 0) {
	printf("[%ums] SOUND-PLAY-FAIL: no free slot (all 100 occupied!)\n", _sound_now_ms());
	return 0;
  }
  return (int)handle;
}

int play_xmidi(struct ADL_MIDIPlayer** midi_players, const void* xmidi, uint32_t len, int seq_num)
{
  return play_xmidi_with_handle_and_mute(midi_players, xmidi, len, seq_num, 0, false);
}

int play_xmidi_external(const void* xmidi, uint32_t len, int seq_num)
{
  int _active = 0;
  for (int i = 0; i < 100; i++) if (g_player_active[i].load(std::memory_order_acquire)) _active++;
  printf("[%ums] SOUND-REQ: xmidi=%p len=%u seq=%d active_slots=%d\n",
         _sound_now_ms(), xmidi, len, seq_num, _active);
  return play_xmidi(midi_players, xmidi, len, seq_num);
}

// New API: external handle + mute flag. Used by audit infra.
int play_xmidi_external_with_handle_and_mute(const void* xmidi, uint32_t len, int seq_num,
                                              uint16_t handle, bool mute)
{
  int _active = 0;
  for (int i = 0; i < 100; i++) if (g_player_active[i].load(std::memory_order_acquire)) _active++;
  printf("[%ums] SOUND-REQ%s: xmidi=%p len=%u seq=%d handle=%04X active_slots=%d\n",
         _sound_now_ms(), mute ? "-MUTE" : "", xmidi, len, seq_num, handle, _active);
  return play_xmidi_with_handle_and_mute(midi_players, xmidi, len, seq_num, handle, mute);
}

// Stop a specific player by HANDLE (unique ID from play_xmidi_external).
// Non-blocking: marks slot for close; audio_callback closes asynchronously.
// Stale handle (slot was reused for another sound) → no-op, mirroring AIL.
// In default mode, multiple slots may share the same handle (orig real + v2
// muted reservation). Iterate all matching slots and mark each for close.
void stop_xmidi_external(uint16_t handle)
{
  if (handle == 0 || handle == 0xFFFF) return;
  uint16_t dn = dontstop_handle.load();
  int marked = for_each_slot_with_handle(handle, [&](int i) {
    printf("[%ums] SOUND-STOP: handle=%04X slot=%d player=%p (dontstop=%04X)\n",
           _sound_now_ms(), handle, i, (void*)midi_players[i], dn);
    need_close[i].store(true);
  });
  if (marked == 0) {
    printf("[%ums] SOUND-STOP: handle=%04X stale (no slot) — no-op\n",
           _sound_now_ms(), handle);
    return;
  }
  if (dn == handle) dontstop_handle.store(0);
}

// Stop ALL non-music SFX. Music (dontstop_handle) is preserved.
// Non-blocking. Used for mute toggle (sub_108c8 SFX path), sub_1782a / sub_17912
// "stop all" semantics. Skip ALL slots whose handle matches dontstop_handle
// (multiple slots may share music handle in default mode).
void stop_all_sfx()
{
  uint16_t dn = dontstop_handle.load();
  int marked = 0;
  for (int i = 0; i < 100; i++) {
    if (g_player_active[i].load(std::memory_order_acquire) &&
        slot_handle[i].load(std::memory_order_relaxed) != dn) {
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
  if (dn == 0 || dn == 0xFFFF) {
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
  // Iterate ALL slots with this handle (orig real + v2 muted reservation).
  // Muted slots have no audio so fade params are no-op; real slot fades normally.
  int faded = for_each_slot_with_handle(dn, [&](int i) {
    _sound_fade_volume[i].store(1.0f);
    _sound_fade_step[i].store(step);
    printf("[%ums] SOUND-FADE: handle=%04X slot=%d duration=%dms step=%.5f/tick (%.0f ticks)\n",
           _sound_now_ms(), dn, i, duration_ms, step, total_ticks);
  });
  if (faded == 0) {
    printf("[%ums] SOUND-FADE: handle=%04X no slot found\n", _sound_now_ms(), dn);
  }
}

// Mark a player as the "music" player by HANDLE. Audio callback scales its
// volume to 60% and stop_all_sfx skips its slot.
// Iterate ALL slots matching handle (orig real + v2 muted reservation in default
// mode). Clear need_close on each so neither the real nor muted slot is closed.
void set_dontstop_external(uint16_t handle)
{
  if (handle == 0 || handle == 0xFFFF) return;
  int marked = for_each_slot_with_handle(handle, [&](int i) {
    bool was_marked = need_close[i].load();
    printf("[%ums] SOUND-DONTSTOP: handle=%04X slot=%d player=%p%s\n",
           _sound_now_ms(), handle, i, (void*)midi_players[i],
           was_marked ? " (cleared stale need_close — music save)" : "");
    need_close[i].store(false);
  });
  if (marked == 0) {
    printf("[%ums] SOUND-DONTSTOP: handle=%04X stale (no slot) — no-op\n",
           _sound_now_ms(), handle);
    return;
  }
  dontstop_handle.store(handle);
}

bool is_player_active(uint16_t handle)
{
  // Iterate all matching slots; return true if any has a live producer
  // (midi_players != nullptr means a producer thread exists, ie not muted).
  // Muted reservation slots have midi_players==nullptr so they're naturally
  // skipped — only real audio counts as "active".
  bool any_active = false;
  for_each_slot_with_handle(handle, [&](int i) {
    if (midi_players[i] != nullptr && !need_close[i].load()) any_active = true;
  });
  return any_active;
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
	  g_stop_requested[i].store(false);
	  g_producer_alive[i].store(false);
	  g_player_active[i].store(false);
	  g_rings[i].reset();
	}

    if(SDL_Init(SDL_INIT_AUDIO) < 0)
        return;

    spec.freq = MYFREQ;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 1024;  // cherry-pick 3f2114a — was 64, larger reduces audio glitches

    spec.callback = my_audio_callback;
    spec.userdata = midi_players;

    if (SDL_OpenAudio(&spec, &obtained) < 0)
    {
        fprintf(stderr, "Couldn't open audio: %s\n", SDL_GetError());
        return;
    }

	printf("SOUND-INIT: requested freq=%d format=0x%X channels=%d samples=%d\n",
	       spec.freq, spec.format, spec.channels, spec.samples);
	printf("SOUND-INIT: obtained  freq=%d format=0x%X channels=%d samples=%d size=%u\n",
	       obtained.freq, obtained.format, obtained.channels, obtained.samples, obtained.size);

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

static uint8_t myBuffer[16384];  // cherry-pick 3f2114a

void my_audio_callback(void *argument, Uint8 *stream, int len)
{
  // Callback budget = samples_in_buffer / sample_rate. For 1024 samples @44100Hz
  // = 23.2ms. If we exceed this, audio will glitch (underrun).
  auto _cb_start = std::chrono::steady_clock::now();
  static int _cb_call_count = 0;
  static int _cb_overrun_count = 0;
  static int _cb_max_slots_seen = 0;
  _cb_call_count++;

  // DIRECT UNDERRUN MEASUREMENT: gap between callback START times. If audio device
  // drains buffer faster than callback can refill, gap > expected period = underrun.
  // Normal: gap ≈ samples / sample_rate. Underrun: gap > 1.5x normal.
  static auto _cb_last_start = std::chrono::steady_clock::time_point{};
  static int _cb_underrun_count = 0;
  static double _cb_underrun_total_ms = 0.0;
  if (_cb_last_start.time_since_epoch().count() != 0) {
    double interval_ms = std::chrono::duration<double, std::milli>(_cb_start - _cb_last_start).count();
    double expected_ms = ((double)len / (double)s_audioFormat.sampleOffset) * 1000.0 / (double)MYFREQ;
    if (interval_ms > expected_ms * 1.5) {
      double gap_ms = interval_ms - expected_ms;
      _cb_underrun_count++;
      _cb_underrun_total_ms += gap_ms;
      printf("[%ums] SOUND-UNDERRUN: gap=%.1fms (expected=%.1fms, +%.1fms missing) "
             "underruns=%d total_silence=%.1fms\n",
             _sound_now_ms(), interval_ms, expected_ms, gap_ms,
             _cb_underrun_count, _cb_underrun_total_ms);
    }
  }
  _cb_last_start = _cb_start;

  // Compute callback budget (μs). Frames per callback = len / sampleOffset
  // (stride per stereo frame). Time available = frames / sample_rate.
  // For 1024 frames @44100 = ~23.2ms.
  const double _cb_budget_us = ((double)len / (double)s_audioFormat.sampleOffset) * 1e6 / (double)MYFREQ;
  const double _cb_warn_us = _cb_budget_us * 0.9;

  //printf("size %x\n", len);
  if (len > (int)sizeof(myBuffer)) {
	printf("SOUND ERROR, len = %d!\n", len);
	len = sizeof(myBuffer);
  }

  memset(myBuffer, 0, len);

  // DEBUG: NO_AUDIO_WORK env var — disable all adlmidi sample generation, output
  // silence. Used to test whether audio CPU is the cause of game FPS drop.
  static int _no_audio_work = -1;
  if (_no_audio_work == -1) {
    _no_audio_work = (getenv("NO_AUDIO_WORK") != nullptr) ? 1 : 0;
    if (_no_audio_work) printf("[%ums] SOUND-DEBUG: NO_AUDIO_WORK=1 — silence-only mode\n", _sound_now_ms());
  }
  if (_no_audio_work) {
    SDL_memcpy(stream, myBuffer, len);
    return;
  }
  // memset(buffer) moved INSIDE the slot loop (cherry-pick 3f2114a) — buffer is
  // reused per slot; without re-zeroing, leftover from previous slot bleeds
  // into next slot's mix.

    // requested_samples = TOTAL samples adlmidi generates across both channels
    // (adl_play/adl_playFormat sampleCount semantic: combined L+R count).
    // For stereo S16: len bytes / containerSize (2) = sample slots = frames × 2.
    const int requested_samples = len / s_audioFormat.containerSize;

	struct ADL_MIDIPlayer** midi_players = (struct ADL_MIDIPlayer**)argument;

	uint8_t count = 0;
	uint8_t volume;
	uint16_t dn = dontstop_handle.load();

	// DECOUPLED MIXER audio_callback: each slot reads from its SPSC ring buffer
	// (filled by per-slot worker thread). No adl_playFormat here → callback CPU
	// is just N × memcpy + SDL_MixAudioFormat = always fast (no overrun).
	// In default mode multiple slots may share the music handle (orig real + v2
	// muted reservation). Music check uses slot_handle[i]==dn so all matching
	// slots get music treatment.
	for (int i = 0; i < 100; i++)
	{
		// Skip slots that are truly inactive.
		if (!g_player_active[i].load(std::memory_order_acquire))
		  continue;

		bool slot_is_music = (dn != 0 && dn != 0xFFFF &&
		                      slot_handle[i].load(std::memory_order_relaxed) == dn);

		// Translate legacy need_close flag into stop_requested for producer.
		// (need_close set by stop_xmidi_external — main thread writes it.)
		if (need_close[i].load() && !g_stop_requested[i].load()) {
		  g_stop_requested[i].store(true, std::memory_order_release);
		}

		// Read from ring. May get less than requested if producer behind or done.
		int got = (int)g_rings[i].read((int16_t*)buffer, requested_samples);

		// First-tick log when ring first delivers data.
		extern bool _sound_first_tick[100];
		if (!_sound_first_tick[i] && got > 0) {
		  _sound_first_tick[i] = true;
		  printf("[%ums] SOUND-FIRST-TICK: slot=%d samples=%d (decoupled-mixer ready)\n",
		         _sound_now_ms(), i, got);
		}

		// SLOT-DRAIN check: real slots close when producer dead + ring empty.
		// Muted slots have no producer (alive=false from start) but should NOT
		// auto-close — they exist purely for DS verify tracking and close only
		// when explicit stop is requested (need_close=true).
		bool producer_dead = !g_producer_alive[i].load(std::memory_order_acquire);
		bool ring_empty = (g_rings[i].available() == 0);
		bool muted = g_slot_muted[i].load(std::memory_order_relaxed);
		bool should_close = muted ? need_close[i].load()
		                          : (producer_dead && ring_empty);
		if (should_close) {
		  // Final cleanup: clear handle, dontstop if music, log close.
		  const char* reason = need_close[i].load() ? "stop-request" : "natural-end";
		  uint16_t closed_handle = slot_handle[i].load();
		  printf("[%ums] SOUND-CLOSE%s: slot=%d handle=%04X reason=%s%s\n",
		         _sound_now_ms(), muted ? "-MUTE" : "", i, closed_handle, reason,
		         slot_is_music ? " (was music!)" : "");
		  need_close[i].store(false);
		  slot_handle[i].store(0, std::memory_order_relaxed);
		  g_slot_muted[i].store(false, std::memory_order_relaxed);
		  extern void _sound_reset_first_tick(int);
		  _sound_reset_first_tick(i);
		  // Clear dontstop only if NO other slot still holds the music handle
		  // (in default mode, orig + v2 muted may both have it; closing one
		  // shouldn't drop dontstop while the other is still alive).
		  if (slot_is_music) {
		    bool other_holds = false;
		    for (int k = 0; k < 100; k++) {
		      if (k == i) continue;
		      if (slot_handle[k].load(std::memory_order_relaxed) == closed_handle) {
		        other_holds = true; break;
		      }
		    }
		    if (!other_holds) dontstop_handle.store(0);
		  }
		  // Mark slot free LAST — play_xmidi waits on this.
		  g_player_active[i].store(false, std::memory_order_release);
		  continue;
		}

		// PARTIAL FILL DETECTION: producer can't keep up → audible glitch in this slot.
		if (got < requested_samples) {
		  static int _partial_log_count = 0;
		  if (slot_is_music || _partial_log_count++ < 50) {
		    printf("[%ums] SOUND-PARTIAL: slot=%d (%s) requested=%d got=%d (%.1f%% fill)\n",
		           _sound_now_ms(), i, slot_is_music ? "MUSIC" : "sfx",
		           requested_samples, got,
		           requested_samples > 0 ? 100.0 * got / requested_samples : 0.0);
		  }
		}

		if (got <= 0) continue;  // nothing to mix this iter (producer not ready yet)

		// Per-slot volume + fade.
		volume = SDL_MIX_MAXVOLUME;
		if (slot_is_music)
		  volume = SDL_MIX_MAXVOLUME * 0.6;
		{
		  float fade_v = _sound_fade_volume[i].load();
		  float fade_s = _sound_fade_step[i].load();
		  if (fade_s > 0.0f) {
		    fade_v -= fade_s;
		    if (fade_v <= 0.0f) {
		      fade_v = 0.0f;
		      _sound_fade_step[i].store(0.0f);
		      need_close[i].store(true);
		      g_stop_requested[i].store(true, std::memory_order_release);
		      printf("[%ums] SOUND-FADE-DONE: slot=%d → marking stop_requested\n",
		             _sound_now_ms(), i);
		    }
		    _sound_fade_volume[i].store(fade_v);
		    volume = (uint8_t)(volume * fade_v);
		  }
		}

		// Mix: got = total samples L+R combined; bytes = got × containerSize.
		int mix_bytes = got * s_audioFormat.containerSize;
		if (mix_bytes > len) mix_bytes = len;
		SDL_MixAudioFormat(myBuffer, buffer, myFormat, mix_bytes, volume);

		count++;

	}

	// Peak amplitude detection in mix output — to verify clipping hypothesis.
	int16_t* samples = (int16_t*)myBuffer;
	int16_t peak_pos = 0, peak_neg = 0;
	int clipped_pos = 0, clipped_neg = 0;
	int sample_count_int16 = len / 2;  // assume int16 stereo
	for (int s = 0; s < sample_count_int16; s++) {
	  if (samples[s] > peak_pos) peak_pos = samples[s];
	  if (samples[s] < peak_neg) peak_neg = samples[s];
	  if (samples[s] == 32767) clipped_pos++;
	  if (samples[s] == -32768) clipped_neg++;
	}

	SDL_memcpy(stream, myBuffer, len);

	// Total callback time + per-slot worst time. Print detail when count >= 3
	// (where user reports "lag"). Helps see if specific slot is CPU hog or if
	// it's a clipping issue (peak at +-32767).
	auto _cb_end = std::chrono::steady_clock::now();
	double _cb_us = std::chrono::duration<double, std::micro>(_cb_end - _cb_start).count();
	if (count > _cb_max_slots_seen) {
	  _cb_max_slots_seen = count;
	  printf("[%ums] SOUND-CB-MAX-SLOTS: count=%u (new max)\n", _sound_now_ms(), count);
	}
	// When 3+ active slots, log every callback details (clipping + timing).
	if (count >= 3) {
	  static int _detail_count = 0;
	  if (++_detail_count <= 100) {  // first 100 only to avoid log flood
	    int clip_pct = clipped_pos + clipped_neg;
	    printf("[%ums] SOUND-CB-DETAIL: slots=%u total=%.0fμs peak=%+d/%d clipped=%d/%d samples\n",
	           _sound_now_ms(), count, _cb_us, peak_pos, peak_neg,
	           clip_pct, sample_count_int16);
	  }
	}
	if (_cb_us > _cb_warn_us) {
	  _cb_overrun_count++;
	  printf("[%ums] SOUND-CB-OVERRUN: took %.0fμs (budget=%.0fμs, %.1f%%) slots=%u "
	         "overruns=%d/total=%d\n",
	         _sound_now_ms(), _cb_us, _cb_budget_us, 100.0 * _cb_us / _cb_budget_us,
	         count, _cb_overrun_count, _cb_call_count);
	}
}
