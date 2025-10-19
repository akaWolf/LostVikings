#include <SDL2/SDL.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <cstring>

#include "adlmidi.h"
#include "play.h"

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


// Two independent audio pools — orig and v2. Each has fully independent slot
// state, handles, dontstop. audio_callback iterates BOTH pools and mixes outputs.
//
// Default mode: orig audible, v2 globally_muted=true (DS verify symmetry only).
// V2_ONLY: only v2 exists, globally_muted=false. Pool muting set in sound_init.
// Non-static so v2_vm.cpp can extern these and call v2_pool.method() directly.
AudioPool orig_pool;
AudioPool v2_pool;

// Forward decls used by macros / helpers below.
static auto _sound_t0 = std::chrono::steady_clock::now();
static uint32_t _sound_now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - _sound_t0).count();
}
extern bool need_quit;

// =============================================================================
// Pool-parametrized helpers — operate on the given AudioPool independently.
// Defined BEFORE the legacy macros so they can use `p.X` access without macro
// conflict. Public API (both orig and v2 variants) delegates to these helpers
// with the appropriate pool ref.
// =============================================================================

// =============================================================================
// AudioPool method definitions. Methods called via `orig_pool.X(...)` or
// `v2_pool.X(...)` — same code path, different `this`.
// =============================================================================

// Allocate unique 16-bit handle (1..0xFFFE; 0 and 0xFFFF reserved as sentinels).
// Thread-safe atomic counter advance with skip of reserved values.
uint16_t AudioPool::alloc_handle() {
    uint16_t h;
    do {
        h = g_next_handle.fetch_add(1, std::memory_order_relaxed);
        if (h == 0 || h == 0xFFFF) continue;
        break;
    } while (true);
    return h;
}

// Call fn(i) for every slot in this pool whose slot_handle matches `h`. Returns
// number of slots matched. Handles 0 / 0xFFFF (sentinels) return 0 without
// iterating. Used by stop_xmidi / fade_music / set_dontstop to operate on all
// slots reserved with the same deterministic handle.
template<class Fn>
int AudioPool::for_each_slot_with_handle(uint16_t h, Fn&& fn) {
    if (h == 0 || h == 0xFFFF) return 0;
    int matched = 0;
    for (int i = 0; i < 100; i++) {
        if (slot_handle[i].load(std::memory_order_relaxed) == h) { fn(i); matched++; }
    }
    return matched;
}

// Forward decl — pool-aware producer thread defined later in file.
static void midi_thread_proc_in_pool(AudioPool* p, struct ADL_MIDIPlayer** midi_player,
                                      const void* xmidi, uint32_t len, int seq_num);

// Core play implementation. Either auto-allocates handle (handle_in==0) or uses
// given. mute=true: reserves slot for tracking but doesn't spawn producer thread
// / no adl_init / no real playback (used by v2_pool in default mode for DS verify
// symmetry where orig_pool produces the actual audio).
// Returns the handle (or 0 on failure: all slots occupied).
int AudioPool::play_xmidi_with_handle_and_mute(const void* xmidi, uint32_t len, int seq_num,
                                                uint16_t handle_in, bool mute) {
    int slot = -1;
    uint16_t handle = 0;
    const char* pool_tag = (this == &v2_pool) ? "-V2" : "";
    for (int i = 0; i < 100; i++) {
        if (midi_players[i] == nullptr && !g_player_active[i].load(std::memory_order_acquire)) {
            bool stale = need_close[i].load();
            need_close[i].store(false);
            g_stop_requested[i].store(false, std::memory_order_relaxed);
            g_rings[i].reset();
            g_producer_alive[i].store(!mute, std::memory_order_relaxed);
            g_slot_muted[i].store(mute, std::memory_order_relaxed);
            g_player_active[i].store(true, std::memory_order_release);
            _sound_fade_volume[i].store(1.0f);
            _sound_fade_step[i].store(0.0f);
            _sound_xmi_buf[i] = xmidi;
            _sound_xmi_len[i] = len;
            _sound_xmi_seq[i] = seq_num;
            handle = (handle_in != 0 && handle_in != 0xFFFF) ? handle_in : alloc_handle();
            slot_handle[i].store(handle, std::memory_order_relaxed);
            if (!mute) {
                std::thread midi_thread(midi_thread_proc_in_pool, this, &midi_players[i],
                                         xmidi, len, seq_num);
                midi_thread.detach();
            }
            printf("[%ums] SOUND-PLAY%s%s: slot=%d handle=%04X seq=%d len=%u%s\n",
                   _sound_now_ms(), pool_tag, mute ? "-MUTE" : "",
                   i, handle, seq_num, len,
                   stale ? " (cleared stale need_close)" : "");
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        printf("[%ums] SOUND-PLAY-FAIL%s: no free slot (all 100 occupied!)\n",
               _sound_now_ms(), pool_tag);
        return 0;
    }
    return (int)handle;
}

// Public: play XMI, auto-allocate handle. Returns handle.
int AudioPool::play_xmidi_external(const void* xmidi, uint32_t len, int seq_num) {
    int _active = 0;
    for (int i = 0; i < 100; i++)
        if (g_player_active[i].load(std::memory_order_acquire)) _active++;
    printf("[%ums] SOUND-REQ%s: xmidi=%p len=%u seq=%d active_slots=%d\n",
           _sound_now_ms(), (this == &v2_pool) ? "-V2" : "",
           xmidi, len, seq_num, _active);
    return play_xmidi_with_handle_and_mute(xmidi, len, seq_num, 0, false);
}

// Public: play XMI with external handle + mute flag (from audit infrastructure).
int AudioPool::play_xmidi_external_with_handle_and_mute(const void* xmidi, uint32_t len,
                                                         int seq_num, uint16_t handle, bool mute) {
    int _active = 0;
    for (int i = 0; i < 100; i++)
        if (g_player_active[i].load(std::memory_order_acquire)) _active++;
    printf("[%ums] SOUND-REQ%s%s: xmidi=%p len=%u seq=%d handle=%04X active_slots=%d\n",
           _sound_now_ms(), (this == &v2_pool) ? "-V2" : "",
           mute ? "-MUTE" : "", xmidi, len, seq_num, handle, _active);
    return play_xmidi_with_handle_and_mute(xmidi, len, seq_num, handle, mute);
}

// Stop a specific slot in this pool by handle. Non-blocking — sets need_close,
// audio_callback closes asynchronously. Stale handle (no matching slot) → no-op
// (mirrors AIL "stop accepts stale handle silently"). Auto-clears
// dontstop_handle if music was being stopped.
void AudioPool::stop_xmidi(uint16_t handle) {
    if (handle == 0 || handle == 0xFFFF) return;
    uint16_t dn = dontstop_handle.load();
    int marked = for_each_slot_with_handle(handle, [&](int i) {
        printf("[%ums] SOUND-STOP%s: handle=%04X slot=%d player=%p (dontstop=%04X)\n",
               _sound_now_ms(), (this == &v2_pool) ? "-V2" : "",
               handle, i, (void*)midi_players[i], dn);
        need_close[i].store(true);
    });
    if (marked == 0) {
        printf("[%ums] SOUND-STOP%s: handle=%04X stale (no slot) — no-op\n",
               _sound_now_ms(), (this == &v2_pool) ? "-V2" : "", handle);
        return;
    }
    if (dn == handle) dontstop_handle.store(0);
}

// Stop ALL non-music SFX in this pool. Music slots (handle matching
// dontstop_handle) preserved. Non-blocking. Used for mute toggle and "stop all"
// semantics from sub_108c8 / sub_1782a / sub_17912.
void AudioPool::stop_all_sfx() {
    uint16_t dn = dontstop_handle.load();
    int marked = 0;
    for (int i = 0; i < 100; i++) {
        if (g_player_active[i].load(std::memory_order_acquire) &&
            slot_handle[i].load(std::memory_order_relaxed) != dn) {
            need_close[i].store(true);
            marked++;
        }
    }
    printf("[%ums] SOUND-STOP-ALL%s: dontstop=%04X marked %d active slots\n",
           _sound_now_ms(), (this == &v2_pool) ? "-V2" : "", dn, marked);
}

// Fade music in this pool over duration_ms. <=0 → immediate stop. Sets per-slot
// fade params; audio_callback ramps volume each iteration until 0 → close.
// AIL DOS sub_1C7BD (set_sequence_tempo) was orig fade — we approximate via
// volume ramp.
void AudioPool::fade_music(int duration_ms) {
    uint16_t dn = dontstop_handle.load();
    const char* pool_tag = (this == &v2_pool) ? "-V2" : "";
    if (dn == 0 || dn == 0xFFFF) {
        printf("[%ums] SOUND-FADE%s: no music to fade\n", _sound_now_ms(), pool_tag);
        return;
    }
    if (duration_ms <= 0) {
        printf("[%ums] SOUND-FADE%s: duration<=0 → immediate stop(%04X)\n",
               _sound_now_ms(), pool_tag, dn);
        stop_xmidi(dn);
        return;
    }
    extern const uint32_t MYFREQ;
    const float ticks_per_sec = (float)MYFREQ / 64.0f;
    const float total_ticks = (duration_ms / 1000.0f) * ticks_per_sec;
    const float step = (total_ticks > 0) ? (1.0f / total_ticks) : 1.0f;
    int faded = for_each_slot_with_handle(dn, [&](int i) {
        _sound_fade_volume[i].store(1.0f);
        _sound_fade_step[i].store(step);
        printf("[%ums] SOUND-FADE%s: handle=%04X slot=%d duration=%dms step=%.5f/tick (%.0f ticks)\n",
               _sound_now_ms(), pool_tag, dn, i, duration_ms, step, total_ticks);
    });
    if (faded == 0) {
        printf("[%ums] SOUND-FADE%s: handle=%04X no slot found\n", _sound_now_ms(), pool_tag, dn);
    }
}

// Mark a player as the "music" player (60% volume scaling, preserved by
// stop_all_sfx). Iterates ALL slots matching handle (multiple slots in same pool
// could share handle in theory — though typically only one). Skips slots already
// marked for close to preserve explicit stop intent on handle collisions.
void AudioPool::set_dontstop(uint16_t handle) {
    if (handle == 0 || handle == 0xFFFF) return;
    const char* pool_tag = (this == &v2_pool) ? "-V2" : "";
    int marked = 0;
    int skipped_closing = 0;
    for_each_slot_with_handle(handle, [&](int i) {
        if (need_close[i].load()) {
            skipped_closing++;
            printf("[%ums] SOUND-DONTSTOP%s: handle=%04X slot=%d SKIP (need_close=true)\n",
                   _sound_now_ms(), pool_tag, handle, i);
            return;
        }
        printf("[%ums] SOUND-DONTSTOP%s: handle=%04X slot=%d player=%p\n",
               _sound_now_ms(), pool_tag, handle, i, (void*)midi_players[i]);
        marked++;
    });
    if (marked == 0 && skipped_closing == 0) {
        printf("[%ums] SOUND-DONTSTOP%s: handle=%04X stale (no slot) — no-op\n",
               _sound_now_ms(), pool_tag, handle);
        return;
    }
    if (marked > 0) dontstop_handle.store(handle);
}

// True if any slot in this pool matching handle has a live producer thread (real
// audio, not muted). Muted reservation slots are naturally skipped — only audible
// playback counts as "active".
bool AudioPool::is_player_active(uint16_t handle) {
    bool any_active = false;
    for_each_slot_with_handle(handle, [&](int i) {
        if (midi_players[i] != nullptr && !need_close[i].load()) any_active = true;
    });
    return any_active;
}

// Initialize all per-slot state to inactive defaults. Called once per pool at
// startup from sound_init. globally_muted_flag controls whether audio_callback
// will produce audible output for this pool's slots (v2_pool=true in default
// mode for verify symmetry; false in V2_ONLY where v2 is the sole audio source).
void AudioPool::init(bool globally_muted_flag) {
    globally_muted = globally_muted_flag;
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
        _sound_first_tick[i] = false;
    }
}

// Reset first-tick log flag for a slot (called after close so reused slots
// re-log latency). Non-method since audio_callback's per-slot loop calls this
// with explicit pool ref.
static void reset_first_tick_in_pool(AudioPool& p, int i) {
    if (i >= 0 && i < 100) p._sound_first_tick[i] = false;
}

void my_audio_callback(void *midi_player, Uint8 *stream, int len);

static Uint8 buffer[16384]; /* Audio buffer (cherry-pick 3f2114a — fix audio glitches) */
static struct ADLMIDI_AudioFormat s_audioFormat;
static SDL_AudioFormat myFormat;
const uint32_t MYFREQ = 44100;

// midi_players[100] now lives in AudioPool struct (orig_pool.midi_players /
// v2_pool.midi_players). Each function takes pool ref explicitly.


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
// Pool-aware version: uses given pool's slot arrays.
static void midi_thread_proc_in_pool(AudioPool* p, struct ADL_MIDIPlayer** midi_player,
                                      const void* xmidi, uint32_t len, int seq_num)
{
	const char* pool_tag = (p == &v2_pool) ? "-V2" : "";
	// Find slot index from midi_player pointer (used for ring/atomic access).
	int slot = -1;
	for (int i = 0; i < 100; i++) if (&p->midi_players[i] == midi_player) { slot = i; break; }
	if (slot < 0) {
		fprintf(stderr, "midi_thread_proc%s: BUG — slot not found for player ptr %p\n",
		        pool_tag, (void*)midi_player);
		p->g_producer_alive[0].store(false);  // safety
		return;
	}

	auto _t0 = std::chrono::steady_clock::now();
	auto curr_player = adl_init(MYFREQ);
	auto _t1 = std::chrono::steady_clock::now();
	printf("[%ums] SOUND-PRODUCER-START%s: slot=%d player=%p xmidi=%p seq=%d\n",
	       _sound_now_ms(), pool_tag, slot, (void*)curr_player, xmidi, seq_num);
	if (!curr_player) {
		fprintf(stderr, "Couldn't initialize ADLMIDI: %s\n", adl_errorString());
		p->g_producer_alive[slot].store(false, std::memory_order_release);
		p->g_player_active[slot].store(false, std::memory_order_release);
		return;
	}

	adl_switchEmulator(curr_player, ADLMIDI_EMU_NUKED);
	adl_setBank(curr_player, 75);
	adl_setLoopEnabled(curr_player, seq_num == -1 ? 1 : 0);
	auto _t2 = std::chrono::steady_clock::now();

	if (adl_openData(curr_player, xmidi, len) < 0) {
		fprintf(stderr, "Couldn't open music file: %s\n", adl_errorInfo(curr_player));
		adl_close(curr_player);
		p->g_producer_alive[slot].store(false, std::memory_order_release);
		p->g_player_active[slot].store(false, std::memory_order_release);
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
		printf("[%ums] SOUND-OPEN%s: slot=%d player=%p len=%u seq=%d songs=%d total_time=%.2fs loopStart=%.2fs "
		       "init=%.0fμs cfg=%.0fμs openData=%.0fμs total=%.0fμs\n",
		       _sound_now_ms(), pool_tag, slot, (void*)curr_player, len, seq_num, songs, total_sec, loopstart,
		       init_us, cfg_us, open_us, init_us + cfg_us + open_us);
	}

	*midi_player = curr_player;

	constexpr int CHUNK_FRAMES = 1024;
	constexpr int CHUNK_SAMPLES = CHUNK_FRAMES * 2;
	int16_t local[CHUNK_SAMPLES];

	while (!p->g_stop_requested[slot].load(std::memory_order_acquire) && !need_quit) {
		while (p->g_rings[slot].free_space() < (size_t)CHUNK_SAMPLES) {
			if (p->g_stop_requested[slot].load(std::memory_order_acquire) || need_quit) goto exit_loop;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		int got = adl_playFormat(curr_player, CHUNK_SAMPLES,
		                         (Uint8*)local,
		                         (Uint8*)local + s_audioFormat.containerSize,
		                         &s_audioFormat);
		if (got <= 0) {
			printf("[%ums] SOUND-PRODUCER-END%s: slot=%d (natural end, got=%d)\n",
			       _sound_now_ms(), pool_tag, slot, got);
			break;
		}
		size_t written = 0;
		while (written < (size_t)got) {
			size_t w = p->g_rings[slot].write(local + written, got - written);
			written += w;
			if (written < (size_t)got) {
				if (p->g_stop_requested[slot].load(std::memory_order_acquire) || need_quit) goto exit_loop;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	}
exit_loop:

	printf("[%ums] SOUND-PRODUCER-EXIT%s: slot=%d stop=%d quit=%d\n",
	       _sound_now_ms(), pool_tag, slot,
	       (int)p->g_stop_requested[slot].load(), (int)need_quit);

	*midi_player = nullptr;
	adl_close(curr_player);
	p->g_producer_alive[slot].store(false, std::memory_order_release);
}

// =============================================================================
// Free-function wrappers for orig path — seg000.cpp SDL inlines call these by
// existing names. Each delegates to orig_pool.method(). v2 callers use
// v2_pool.method() directly (no separate free function variants).
// =============================================================================
int play_xmidi_with_handle_and_mute(const void* xmidi, uint32_t len, int seq_num,
                                     uint16_t handle_in, bool mute) {
    return orig_pool.play_xmidi_with_handle_and_mute(xmidi, len, seq_num, handle_in, mute);
}
int play_xmidi(const void* xmidi, uint32_t len, int seq_num) {
    return orig_pool.play_xmidi_with_handle_and_mute(xmidi, len, seq_num, 0, false);
}
int play_xmidi_external(const void* xmidi, uint32_t len, int seq_num) {
    return orig_pool.play_xmidi_external(xmidi, len, seq_num);
}
int play_xmidi_external_with_handle_and_mute(const void* xmidi, uint32_t len, int seq_num,
                                              uint16_t handle, bool mute) {
    return orig_pool.play_xmidi_external_with_handle_and_mute(xmidi, len, seq_num, handle, mute);
}
void stop_xmidi_external(uint16_t handle) { orig_pool.stop_xmidi(handle); }
void stop_all_sfx()                        { orig_pool.stop_all_sfx(); }
void stop_xmidi_external()                 { orig_pool.stop_all_sfx(); }
void fade_music(int duration_ms)           { orig_pool.fade_music(duration_ms); }
void set_dontstop_external(uint16_t handle){ orig_pool.set_dontstop(handle); }
bool is_player_active(uint16_t handle)     { return orig_pool.is_player_active(handle); }
uint16_t get_music_handle()                { return orig_pool.get_music_handle(); }

void sound_init()
{
  printf("init sound\n");
    static SDL_AudioSpec spec, obtained;

	// Init pools. In V2_ONLY mode only v2_pool is initialized (orig path doesn't
	// execute → orig_pool stays default-constructed, unused). In default mode
	// orig_pool is audible producer, v2_pool muted for DS verify symmetry.
#ifdef V2_ONLY
	v2_pool.init(false);    // v2 audible (sole producer)
#else
	orig_pool.init(false);  // orig audible
	v2_pool.init(true);     // v2 muted for DS verify symmetry
#endif

    if(SDL_Init(SDL_INIT_AUDIO) < 0)
        return;

    spec.freq = MYFREQ;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 1024;  // cherry-pick 3f2114a — was 64, larger reduces audio glitches

    spec.callback = my_audio_callback;
    spec.userdata = nullptr;  // unused — audio_callback iterates orig_pool + v2_pool internally

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

// Process one pool's slots in audio callback. Returns count of mixed slots.
// Skips if pool.globally_muted (no audio mixing — but slot lifecycle still runs
// for need_close handling so v2 stop calls still close v2 muted slots cleanly).
static uint8_t process_pool_in_callback(AudioPool& p, Uint8* mix_buffer, int len, int requested_samples) {
    uint8_t count = 0;
    uint16_t dn = p.dontstop_handle.load();
    for (int i = 0; i < 100; i++) {
        if (!p.g_player_active[i].load(std::memory_order_acquire)) continue;

        bool slot_is_music = (dn != 0 && dn != 0xFFFF &&
                              p.slot_handle[i].load(std::memory_order_relaxed) == dn);

        if (p.need_close[i].load() && !p.g_stop_requested[i].load()) {
            p.g_stop_requested[i].store(true, std::memory_order_release);
        }

        int got = (int)p.g_rings[i].read((int16_t*)buffer, requested_samples);

        if (!p._sound_first_tick[i] && got > 0) {
            p._sound_first_tick[i] = true;
            printf("[%ums] SOUND-FIRST-TICK%s: slot=%d samples=%d (decoupled-mixer ready)\n",
                   _sound_now_ms(), (&p == &v2_pool) ? "-V2" : "", i, got);
        }

        bool producer_dead = !p.g_producer_alive[i].load(std::memory_order_acquire);
        bool ring_empty = (p.g_rings[i].available() == 0);
        bool muted = p.g_slot_muted[i].load(std::memory_order_relaxed);
        bool should_close = muted ? p.need_close[i].load() : (producer_dead && ring_empty);
        if (should_close) {
            const char* reason = p.need_close[i].load() ? "stop-request" : "natural-end";
            uint16_t closed_handle = p.slot_handle[i].load();
            printf("[%ums] SOUND-CLOSE%s%s: slot=%d handle=%04X reason=%s%s\n",
                   _sound_now_ms(), (&p == &v2_pool) ? "-V2" : "",
                   muted ? "-MUTE" : "", i, closed_handle, reason,
                   slot_is_music ? " (was music!)" : "");
            p.need_close[i].store(false);
            p.slot_handle[i].store(0, std::memory_order_relaxed);
            p.g_slot_muted[i].store(false, std::memory_order_relaxed);
            reset_first_tick_in_pool(p, i);
            if (slot_is_music) {
                bool other_holds = false;
                for (int k = 0; k < 100; k++) {
                    if (k == i) continue;
                    if (p.slot_handle[k].load(std::memory_order_relaxed) == closed_handle) {
                        other_holds = true; break;
                    }
                }
                if (!other_holds) p.dontstop_handle.store(0);
            }
            p.g_player_active[i].store(false, std::memory_order_release);
            continue;
        }

        // Skip audio mixing for globally-muted pool (v2 in default mode).
        if (p.globally_muted) continue;

        if (got < requested_samples) {
            static int _partial_log_count = 0;
            if (slot_is_music || _partial_log_count++ < 50) {
                printf("[%ums] SOUND-PARTIAL%s: slot=%d (%s) requested=%d got=%d (%.1f%% fill)\n",
                       _sound_now_ms(), (&p == &v2_pool) ? "-V2" : "",
                       i, slot_is_music ? "MUSIC" : "sfx", requested_samples, got,
                       requested_samples > 0 ? 100.0 * got / requested_samples : 0.0);
            }
        }
        if (got <= 0) continue;

        uint8_t volume = SDL_MIX_MAXVOLUME;
        if (slot_is_music) volume = SDL_MIX_MAXVOLUME * 0.6;
        {
            float fade_v = p._sound_fade_volume[i].load();
            float fade_s = p._sound_fade_step[i].load();
            if (fade_s > 0.0f) {
                fade_v -= fade_s;
                if (fade_v <= 0.0f) {
                    fade_v = 0.0f;
                    p._sound_fade_step[i].store(0.0f);
                    p.need_close[i].store(true);
                    p.g_stop_requested[i].store(true, std::memory_order_release);
                    printf("[%ums] SOUND-FADE-DONE%s: slot=%d → marking stop_requested\n",
                           _sound_now_ms(), (&p == &v2_pool) ? "-V2" : "", i);
                }
                p._sound_fade_volume[i].store(fade_v);
                volume = (uint8_t)(volume * fade_v);
            }
        }

        int mix_bytes = got * s_audioFormat.containerSize;
        if (mix_bytes > len) mix_bytes = len;
        SDL_MixAudioFormat(mix_buffer, buffer, myFormat, mix_bytes, volume);
        count++;
    }
    return count;
}

void my_audio_callback(void *argument, Uint8 *stream, int len)
{
  auto _cb_start = std::chrono::steady_clock::now();
  static int _cb_call_count = 0;
  static int _cb_overrun_count = 0;
  static int _cb_max_slots_seen = 0;
  _cb_call_count++;

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

  const double _cb_budget_us = ((double)len / (double)s_audioFormat.sampleOffset) * 1e6 / (double)MYFREQ;
  const double _cb_warn_us = _cb_budget_us * 0.9;

  if (len > (int)sizeof(myBuffer)) {
    printf("SOUND ERROR, len = %d!\n", len);
    len = sizeof(myBuffer);
  }
  memset(myBuffer, 0, len);

  static int _no_audio_work = -1;
  if (_no_audio_work == -1) {
    _no_audio_work = (getenv("NO_AUDIO_WORK") != nullptr) ? 1 : 0;
    if (_no_audio_work) printf("[%ums] SOUND-DEBUG: NO_AUDIO_WORK=1 — silence-only mode\n", _sound_now_ms());
  }
  if (_no_audio_work) {
    SDL_memcpy(stream, myBuffer, len);
    return;
  }

  const int requested_samples = len / s_audioFormat.containerSize;
  (void)argument;

  // Process both pools. Each operates independently — slots/handles/dontstop
  // are per-pool. Globally-muted pool (v2 in default mode) skips audio mixing
  // but still runs slot lifecycle so stop/need_close cleanup works.
  uint8_t count = 0;
  count += process_pool_in_callback(orig_pool, myBuffer, len, requested_samples);
  count += process_pool_in_callback(v2_pool,   myBuffer, len, requested_samples);

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
