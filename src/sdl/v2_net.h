// v2_net.h — co-op (UX plan stage 8, step 3): lockstep networking as a
// "networked replay".
//
// The world has ONE input read point (v2_read_input_12352_iter, the mirror of
// sub_12352; the wait loops call it more often than once a frame) and the
// recorder already tags every key event with the number of the read that
// first sees it (g_sub12352_seq, the seq channel of tests/replays): a game is
// a function of that event stream alone. So every client is a recorder of its
// own player's events and a replayer of everybody's: at its read m it packs
// the events captured since the last read into the batch of read m + d
// (d = the input delay in reads, `--delay=`, default 2), sends it and queues
// it for itself; read n proceeds once the batch n of every participating
// player is there and injects all of them, in player order, through
// apply_replay_event — the same delivery the seq replays use, so the INT9
// letter channel ([28C]), the spec keys and the typematic repeats travel with
// the action bits. Every client applies the same events at the same read: the
// simulations are identical, and a per-frame hash of the DS (FRAME_END)
// proves it — a mismatch is reported as V2-NET-DESYNC.
//
// Topology: a star through the host (`--host=PORT --coop=N`, or HOST GAME in
// the F1 menu; clients `--join=HOST[:PORT]` / JOIN in the menu); the host is
// player 1, assigns the player numbers in connection order and relays every
// batch. Its HELLO also carries the options that shape the simulation (the
// SNES balance, the scenes, the console finale, the view width, the language)
// so the clients play the host's world. A peer that drops out leaves an idle
// viking: its missing batches count as empty.
//
// Joining a running game (the stage 8 tails): the host keeps listening; a
// client that connects mid-game gets HELLO + START at once and then the
// host's state image (the V2S1 blocks + the COOP block: g_coop, the cameras,
// the frame counter, the view), taken at the host's next MAIN read (the
// pre_vm read, never one of a wait loop) and tagged with that read number m,
// followed by every batch the host already holds for reads >= m. The joiner
// applies the image at its own next main read, makes that read m, and takes
// part from read m + d (`J <player> <from>` tells everybody from which read
// its batches are awaited). The same image channel carries the host's LOAD
// of a saved state to every peer (applied by all at the same read).
// `--delay=N` without host/join is the solo lockstep — the same pipeline with
// no peers, the reference the loopback benches (tests/coop_net*.sh) compare
// the networked instances against, byte for byte.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct V2NetEvent {
    int frame;              // the recorder's frame column (logs only; the read number rules delivery)
    uint8_t kind;           // 0 = KD, 1 = KU, 2 = KR (typematic repeat: the INT9 note only)
    std::string action;     // the recorder's action name ("RIGHT", "P2:ACTION", "p", "F10" ...)
};

// lobby
bool v2_net_host(int port, int players, int delay, int lobby_wait);   // listen; wait for lobby_wait clients (the rest may join later); HELLO + START; false = failed
bool v2_net_listen(int port, int players, int delay);                 // host without a lobby: listen and return (the menu's HOST GAME); clients join a running game
bool v2_net_join(const char* host_port);              // "host" or "host:port"; blocking: HELLO sets players / this player / delay / options; waits for START
bool v2_net_join_async(const char* host_port);        // the menu's JOIN: the same in a helper thread; the session is adopted on the game thread (v2_net_adopt_session)
void v2_net_solo(int delay);                          // the pipeline without peers
bool v2_net_active();
int  v2_net_delay();
int  v2_net_peer_count();     // 0 = the solo pipeline
bool v2_net_is_host();
bool v2_net_joined_late();    // client: the game was running when it joined (the host's image comes first)
bool v2_net_synced();         // this client runs the shared simulation (a late joiner: once the image is applied)
const char* v2_net_status();  // one line for the menu ("NET: OFF", "NET: HOST ...", "NET: PLAYER 2 ...")
int  v2_net_rtt_ms();         // a client's last round trip to the host, -1 = unknown

// the read pipeline (game thread, from v2_input_tick_12352)
void v2_net_send_batch(long read_n, const std::vector<V2NetEvent>& evs);   // this client's batch for read_n (queued locally too)
bool v2_net_wait_batch(long read_n, std::vector<V2NetEvent>& out);         // every participating player's batch for read_n, player order; false = quitting

// the image channel (game thread, at a main read)
bool v2_net_adopt_session();                                              // a joined session's players / this player -> the game (true once)
bool v2_net_snapshot_wanted();                                            // host: a joiner (or a state load) waits for an image
void v2_net_send_image(long read_n, const std::vector<uint8_t>& img, bool everyone);   // host: to the waiting joiners (with the batches >= read_n and a J for all), or to every peer + itself
bool v2_net_take_image(std::vector<uint8_t>& img, long& read_n);          // the pending image, if any

// the per-frame proof (game thread, FRAME_END)
void v2_net_send_hash(int frame, uint32_t hash);

void v2_net_shutdown();
