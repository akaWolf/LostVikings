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
// it for itself; read n proceeds once the batch n of every player is there
// and injects all of them, in player order, through apply_replay_event — the
// same delivery the seq replays use, so the INT9 letter channel ([28C]), the
// spec keys and the typematic repeats travel with the action bits. Every
// client applies the same events at the same read: the simulations are
// identical, and a per-frame hash of the DS (FRAME_END) proves it — a
// mismatch is reported as V2-NET-DESYNC.
//
// Topology: a star through the host (`--host=PORT --coop=N`; clients
// `--join=HOST[:PORT]`); the host is player 1, assigns the player numbers in
// connection order and relays every batch. Its HELLO also carries the options
// that shape the simulation (the SNES balance, the scenes, the console finale,
// the view width, the language) so the clients play the host's world. A peer
// that drops out leaves an idle viking: its missing batches count as empty.
// `--delay=N` without host/join is the solo lockstep — the same pipeline with
// no peers, the reference the loopback bench (tests/coop_net.sh) compares
// three networked instances against, byte for byte.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct V2NetEvent {
    int frame;              // the recorder's frame column (logs only; the read number rules delivery)
    uint8_t kind;           // 0 = KD, 1 = KU, 2 = KR (typematic repeat: the INT9 note only)
    std::string action;     // the recorder's action name ("RIGHT", "P2:ACTION", "p", "F10" ...)
};

// lobby (main thread, before the game starts)
bool v2_net_host(int port, int players, int delay);   // listen, wait for players-1 clients, HELLO + START; false = failed
bool v2_net_join(const char* host_port);              // "host" or "host:port"; HELLO sets players / local player / delay / options; waits for START
void v2_net_solo(int delay);                          // the pipeline without peers
bool v2_net_active();
int  v2_net_delay();
int  v2_net_peer_count();     // 0 = the solo pipeline

// the read pipeline (game thread, from v2_input_tick_12352)
void v2_net_send_batch(long read_n, const std::vector<V2NetEvent>& evs);   // this client's batch for read_n (queued locally too)
bool v2_net_wait_batch(long read_n, std::vector<V2NetEvent>& out);         // every player's batch for read_n, player order; false = quitting

// the per-frame proof (game thread, FRAME_END)
void v2_net_send_hash(int frame, uint32_t hash);

void v2_net_shutdown();
