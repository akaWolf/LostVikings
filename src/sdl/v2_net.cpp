// v2_net.cpp — the lockstep transport (see v2_net.h). Plain TCP, one line per
// message (an image line is followed by its raw bytes), a receiver thread per
// connection, an accept thread on the host, a mutex + condition variable
// around the per-player inboxes. Nothing here touches the game state: the
// recorder (v2_input_recorder.cpp) turns the batches into key events at the
// read, the way it replays a file, and applies the images.
//
// Lines:
//   W <players> <index> <delay> <opts...>   host -> client on connect (HELLO)
//   S                                       host -> client: the game is on (after the lobby, or at once for a late joiner)
//   I <player> <read_n> <count> <frame>/<kind>/<action> ...   a batch (relayed by the host)
//   T <read_n> <len>\n<len bytes>           host -> client: the state image taken at read_n
//   J <player> <from_read>                  host -> all: that player's batches are awaited from that read on
//   H <player> <frame> <hash>               client -> host, the DS hash of that frame
//   P <ticks>                               client -> host -> client: a ping, echoed as is
//   B <player>                              a player left (relayed by the host)
#include "v2_net.h"
#include "v2_coop.h"
#include "v2_ui.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
static const sock_t BAD_SOCK = INVALID_SOCKET;
static bool sock_init() { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }
static void sock_close(sock_t s) { closesocket(s); }
static void sock_shutdown(sock_t s) { shutdown(s, SD_BOTH); }
static int  sock_send(sock_t s, const char* p, size_t n) { return send(s, p, (int)n, 0); }
static int  sock_recv(sock_t s, char* p, size_t n) { return recv(s, p, (int)n, 0); }
static void sock_nodelay(sock_t s) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one); }
static void sock_reuse(sock_t s) { int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one); }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int sock_t;
static const sock_t BAD_SOCK = -1;
static bool sock_init() { return true; }
static void sock_close(sock_t s) { close(s); }
static void sock_shutdown(sock_t s) { shutdown(s, SHUT_RDWR); }
static int  sock_send(sock_t s, const char* p, size_t n) { return (int)send(s, p, n, MSG_NOSIGNAL); }
static int  sock_recv(sock_t s, char* p, size_t n) { return (int)recv(s, p, n, 0); }
static void sock_nodelay(sock_t s) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
static void sock_reuse(sock_t s) { int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
#endif

extern bool need_quit;
extern "C" void v2_input_recorder_net(int local_player, int synced);   // v2_input_recorder.cpp
extern "C" void v2_nopl_force_frame_ticks(void);                       // v2_native_opl.cpp: the sequencer by frames (the audio clock is not shared)

namespace {

struct Peer {
    sock_t s = BAD_SOCK;
    int player = -1;                 // host side: the client's player; client side: 0 (the host)
    std::mutex send_mx;
    std::thread rx;
    std::atomic<bool> alive{false};
    std::atomic<bool> started{false};       // host: START sent (a lobby peer waits for the lobby to fill)
    std::atomic<bool> needs_image{false};   // host: a late joiner waiting for the state image
    std::string acc0;                // bytes the lobby read past its last line (the receiver starts with them)
};

std::vector<Peer*> g_peers;          // host: one per client; client: [0] = the host
std::mutex g_peers_mx;               // the host's accept thread adds peers while the game thread sends
std::atomic<bool> g_active{false};
bool g_host = false;
int  g_players = 1, g_local = 0, g_delay = 2, g_port = 0;
std::string g_host_name;

std::mutex g_mx;
std::condition_variable g_cv;
std::map<long, std::vector<V2NetEvent>> g_inbox[V2_COOP_MAX];   // [player][read_n] -> the batch
bool g_gone[V2_COOP_MAX] = {false, false, false};
bool g_part[V2_COOP_MAX] = {false, false, false};   // the players whose batches a read waits for
long g_part_from[V2_COOP_MAX] = {0, 0, 0};          // ... from this read on (a late joiner's first batch)

// the image channel
std::vector<uint8_t> g_pending_image; long g_pending_image_read = 0; bool g_has_image = false;
// a session joined from the menu, adopted by the game thread
std::atomic<bool> g_session_pending{false}; int g_session_players = 0, g_session_index = 0;

// host: the hashes — mine per frame, theirs per player per frame
std::map<int, uint32_t> g_hash_mine;
std::map<int, uint32_t> g_hash_peer[V2_COOP_MAX];
int  g_desync = 0;
long g_hash_checked = 0;             // host: peer hashes compared with mine
long g_batches_sent = 0, g_batches_recv = 0;
std::chrono::steady_clock::time_point g_last_wait_log;
std::atomic<int> g_rtt_ms{-1};
uint32_t g_last_ping = 0;

// the lobby / accept thread (host)
sock_t g_listen = BAD_SOCK;
std::thread g_accept;
std::atomic<bool> g_lobby_open{false};     // the lobby has not filled yet: new peers wait for START
bool g_late = false;                        // client: joined a running game (an image follows START)
std::atomic<bool> g_image_applied{false};   // client: a late joiner took the host's image (its hashes count from then on)
std::atomic<int>  g_connected{0};
std::atomic<bool> g_quit_net{false};

// ---------------------------------------------------------------- sockets
bool send_all(Peer* p, const char* data, size_t len) {
    if (!p || p->s == BAD_SOCK) return false;
    std::lock_guard<std::mutex> lk(p->send_mx);
    size_t off = 0;
    while (off < len) {
        int n = sock_send(p->s, data + off, len - off);
        if (n <= 0) { p->alive = false; return false; }
        off += (size_t)n;
    }
    return true;
}
bool send_all(Peer* p, const std::string& line) { return send_all(p, line.data(), line.size()); }
// one line, blocking (the lobby: before the receiver thread exists)
bool recv_line(sock_t s, std::string& acc, std::string& line) {
    for (;;) {
        size_t nl = acc.find('\n');
        if (nl != std::string::npos) { line = acc.substr(0, nl); acc.erase(0, nl + 1); return true; }
        char buf[512];
        int n = sock_recv(s, buf, sizeof buf);
        if (n <= 0) return false;
        acc.append(buf, (size_t)n);
    }
}
std::vector<Peer*> peers_snapshot() {
    std::lock_guard<std::mutex> lk(g_peers_mx);
    return g_peers;
}

// ---------------------------------------------------------------- messages
std::string encode_batch(int player, long read_n, const std::vector<V2NetEvent>& evs) {
    std::string s = "I " + std::to_string(player) + " " + std::to_string(read_n) + " " + std::to_string(evs.size());
    for (const V2NetEvent& e : evs) {
        s += " " + std::to_string(e.frame) + "/" + (e.kind == 0 ? "KD" : e.kind == 1 ? "KU" : "KR") + "/" + e.action;
    }
    s += "\n";
    return s;
}
bool decode_batch(const std::string& line, int& player, long& read_n, std::vector<V2NetEvent>& evs) {
    // "I p n c f/k/a f/k/a ..."
    const char* c = line.c_str() + 1;
    char* end = nullptr;
    player = (int)strtol(c, &end, 10); if (end == c) return false; c = end;
    read_n = strtol(c, &end, 10);      if (end == c) return false; c = end;
    long cnt = strtol(c, &end, 10);    if (end == c) return false; c = end;
    evs.clear();
    for (long i = 0; i < cnt; i++) {
        while (*c == ' ') c++;
        if (!*c) return false;
        V2NetEvent e;
        e.frame = (int)strtol(c, &end, 10); if (end == c || *end != '/') return false; c = end + 1;
        if      (!strncmp(c, "KD/", 3)) e.kind = 0;
        else if (!strncmp(c, "KU/", 3)) e.kind = 1;
        else if (!strncmp(c, "KR/", 3)) e.kind = 2;
        else return false;
        c += 3;
        const char* sp = c;
        while (*sp && *sp != ' ') sp++;
        e.action.assign(c, (size_t)(sp - c));
        c = sp;
        evs.push_back(e);
    }
    return true;
}

void relay(const std::string& line, Peer* from) {
    if (!g_host) return;
    for (Peer* p : peers_snapshot()) if (p != from && p->alive && p->started) send_all(p, line);
}

void note_gone(int player) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (player >= 0 && player < V2_COOP_MAX) g_gone[player] = true;
    }
    g_cv.notify_all();
}

void check_hash(int frame) {
    // host, g_mx held: compare every peer hash of `frame` with mine
    auto me = g_hash_mine.find(frame);
    if (me == g_hash_mine.end()) return;
    for (int k = 1; k < g_players; k++) {
        auto it = g_hash_peer[k].find(frame);
        if (it == g_hash_peer[k].end()) continue;
        g_hash_checked++;
        if (it->second != me->second) {
            g_desync++;
            if (g_desync <= 20)
                fprintf(stderr, "V2-NET-DESYNC: frame %d player %d hash %08X, host %08X (#%d)\n",
                        frame, k + 1, it->second, me->second, g_desync);
        }
        g_hash_peer[k].erase(it);
    }
    while (g_hash_mine.size() > 64) g_hash_mine.erase(g_hash_mine.begin());
}

void handle_line(Peer* from, const std::string& line) {
    if (line.empty()) return;
    switch (line[0]) {
    case 'I': {
        int player; long read_n; std::vector<V2NetEvent> evs;
        if (!decode_batch(line, player, read_n, evs) || player < 0 || player >= V2_COOP_MAX) {
            fprintf(stderr, "V2-NET: bad batch line '%.60s'\n", line.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_inbox[player][read_n] = std::move(evs);
            g_batches_recv++;
        }
        g_cv.notify_all();
        relay(line + "\n", from);
        break;
    }
    case 'H': {
        int player = 0, frame = 0; unsigned hash = 0;
        if (sscanf(line.c_str(), "H %d %d %x", &player, &frame, &hash) == 3 && g_host && player > 0 && player < V2_COOP_MAX) {
            std::lock_guard<std::mutex> lk(g_mx);
            g_hash_peer[player][frame] = hash;
            check_hash(frame);
        }
        break;
    }
    case 'J': {
        int player = -1; long from_read = 0;
        if (sscanf(line.c_str(), "J %d %ld", &player, &from_read) == 2 && player >= 0 && player < V2_COOP_MAX) {
            {
                std::lock_guard<std::mutex> lk(g_mx);
                g_part[player] = true; g_part_from[player] = from_read; g_gone[player] = false;
            }
            g_cv.notify_all();
            fprintf(stderr, "V2-NET: player %d takes part from read %ld\n", player + 1, from_read);
        }
        break;
    }
    case 'P': {
        if (g_host) send_all(from, line + "\n");                  // echo
        else {
            unsigned t = 0;
            if (sscanf(line.c_str(), "P %u", &t) == 1) g_rtt_ms = (int)(SDL_GetTicks() - t);
        }
        break;
    }
    case 'B': {
        int player = -1;
        if (sscanf(line.c_str(), "B %d", &player) == 1) {
            fprintf(stderr, "V2-NET: player %d left\n", player + 1);
            note_gone(player);
            relay(line + "\n", from);
        }
        break;
    }
    default:
        break;
    }
}

void rx_thread(Peer* p) {
    std::string acc = p->acc0, line;
    char buf[4096];
    for (;;) {
        for (;;) {
            size_t nl = acc.find('\n');
            if (nl == std::string::npos) break;
            line = acc.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == 'T') {
                // the state image: the line, then len raw bytes
                long read_n = 0; unsigned long len = 0;
                if (sscanf(line.c_str(), "T %ld %lu", &read_n, &len) != 2) { acc.erase(0, nl + 1); continue; }
                if (acc.size() < nl + 1 + len) break;              // more bytes needed
                {
                    std::lock_guard<std::mutex> lk(g_mx);
                    g_pending_image.assign(acc.begin() + (long)(nl + 1), acc.begin() + (long)(nl + 1 + len));
                    g_pending_image_read = read_n;
                    g_has_image = true;
                }
                fprintf(stderr, "V2-NET: state image of read %ld received (%lu bytes)\n", read_n, len);
                acc.erase(0, nl + 1 + len);
                continue;
            }
            acc.erase(0, nl + 1);
            handle_line(p, line);
        }
        if (!p->alive) break;
        int n = sock_recv(p->s, buf, sizeof buf);
        if (n <= 0) break;
        acc.append(buf, (size_t)n);
    }
    if (p->alive) {
        p->alive = false;
        int gone = g_host ? p->player : 0;      // the host lost a client / a client lost the host
        fprintf(stderr, "V2-NET: connection to player %d closed\n", gone + 1);
        note_gone(gone);
        if (g_host) relay("B " + std::to_string(gone) + "\n", p);
        else {
            // the host is gone: every other player's batches stop too
            for (int k = 0; k < V2_COOP_MAX; k++) if (k != g_local) note_gone(k);
        }
    }
}

void start_rx(Peer* p) {
    p->alive = true;
    p->rx = std::thread(rx_thread, p);
}

// ---------------------------------------------------------------- options
std::string options_string() {
    v2_options_ensure_loaded();
    char b[128];
    snprintf(b, sizeof b, "snes_balance=%d scenes=%d console_finale=%d wide=%d language=%s",
             (int)v2_options.snes_balance.load(), (int)v2_options.scenes.load(),
             (int)v2_options.console_finale.load(), v2_options.wide.load(), v2_options_lang_code);
    return b;
}
void apply_options(const char* opts) {
    v2_options_ensure_loaded();      // the cfg first, the host's values over it
    int v; char lang[16];
    const char* c = opts;
    while (*c) {
        while (*c == ' ') c++;
        if (sscanf(c, "snes_balance=%d", &v) == 1) v2_options.snes_balance = v != 0;
        else if (sscanf(c, "scenes=%d", &v) == 1) v2_options.scenes = v != 0;
        else if (sscanf(c, "console_finale=%d", &v) == 1) v2_options.console_finale = v != 0;
        else if (sscanf(c, "wide=%d", &v) == 1) v2_options.wide = (v >= 0 && v <= 2) ? v : 0;
        else if (sscanf(c, "language=%15[A-Za-z-]", lang) == 1) { strncpy(v2_options_lang_code, lang, 7); v2_options_lang_code[7] = 0; }
        while (*c && *c != ' ') c++;
    }
    fprintf(stderr, "V2-NET: the host's world: %s\n", opts);
}

// ---------------------------------------------------------------- host: accept
// "S" / "S I" + the participants as of now: <player>:<from_read> for the host
// and every started peer — the client waits for exactly these from its first
// read (a later joiner is announced by J). g_mx held by the caller.
std::string start_line(bool image) {
    std::string l = image ? "S I" : "S";
    l += " 0:0";
    for (Peer* q : peers_snapshot())
        if (q->alive && q->started && g_part[q->player]) l += " " + std::to_string(q->player) + ":" + std::to_string(g_part_from[q->player]);
    return l + "\n";
}
int free_player_slot() {
    // the lowest player number 1..players-1 without a live peer (a departed player's number is reused)
    for (int k = 1; k < g_players; k++) {
        bool taken = false;
        for (Peer* p : peers_snapshot()) if (p->alive && p->player == k) taken = true;
        if (!taken) return k;
    }
    return -1;
}
void accept_thread() {
    const std::string opts = options_string();
    while (!g_quit_net) {
        sockaddr_in ca; socklen_t cl = sizeof ca;
        sock_t cs = accept(g_listen, (sockaddr*)&ca, &cl);
        if (cs == BAD_SOCK) { if (g_quit_net) break; continue; }
        int k = free_player_slot();
        if (k < 0) { fprintf(stderr, "V2-NET: a connection refused — the game is full\n"); sock_close(cs); continue; }
        sock_nodelay(cs);
        Peer* p = new Peer; p->s = cs; p->player = k;
        char hello[256];
        snprintf(hello, sizeof hello, "W %d %d %d %s\n", g_players, k, g_delay, opts.c_str());
        send_all(p, hello);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_gone[k] = false;
        }
        {
            std::lock_guard<std::mutex> lk(g_peers_mx);
            g_peers.push_back(p);
        }
        g_connected++;
        fprintf(stderr, "V2-NET: player %d connected from %s%s\n", k + 1, inet_ntoa(ca.sin_addr),
                g_lobby_open ? " (lobby)" : " — joining the running game");
        if (!g_lobby_open) {
            // a late joiner: START now ("S I": an image follows), the state image at the host's next main read
            std::string sl;
            { std::lock_guard<std::mutex> lk(g_mx); sl = start_line(true); }
            send_all(p, sl);
            p->started = true;
            p->needs_image = true;
        }
        start_rx(p);
    }
}

bool start_listening(int port, int players, int delay) {
    if (players < 2 || players > V2_COOP_MAX) { fprintf(stderr, "V2-NET: hosting needs 2 or 3 players\n"); return false; }
    if (!sock_init()) { fprintf(stderr, "V2-NET: socket init failed\n"); return false; }
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == BAD_SOCK) { fprintf(stderr, "V2-NET: socket() failed\n"); return false; }
    sock_reuse(ls);
    sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((unsigned short)port);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) {
        fprintf(stderr, "V2-NET: cannot listen on port %d\n", port);
        sock_close(ls); return false;
    }
    g_listen = ls;
    v2_nopl_force_frame_ticks();
    g_players = players; g_local = 0; g_delay = delay; g_host = true; g_port = port;
    g_part[0] = true; g_part_from[0] = 0;
    g_last_wait_log = std::chrono::steady_clock::now();
    g_accept = std::thread(accept_thread);
    return true;
}

// ---------------------------------------------------------------- client: connect
bool connect_and_hello(const char* host_port, std::string& err) {
    if (!sock_init()) { err = "socket init failed"; return false; }
    std::string hp = host_port ? host_port : "";
    std::string host = hp, port = "7420";
    size_t colon = hp.rfind(':');
    if (colon != std::string::npos) { host = hp.substr(0, colon); port = hp.substr(colon + 1); }
    if (host.empty()) host = "127.0.0.1";
    addrinfo hints; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) { err = "cannot resolve " + host; return false; }
    sock_t s = BAD_SOCK;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == BAD_SOCK) continue;
        if (connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) break;
        sock_close(s); s = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) { err = "cannot connect to " + host + ":" + port; return false; }
    sock_nodelay(s);
    std::string acc, line;
    if (!recv_line(s, acc, line) || line.size() < 2 || line[0] != 'W') { err = "no HELLO from the host"; sock_close(s); return false; }
    int players = 0, index = 0, delay = 2, consumed = 0;
    if (sscanf(line.c_str(), "W %d %d %d %n", &players, &index, &delay, &consumed) < 3 || players < 2 || players > V2_COOP_MAX || index < 1 || index >= players) {
        err = "bad HELLO"; sock_close(s); return false;
    }
    apply_options(line.c_str() + consumed);
    v2_nopl_force_frame_ticks();
    g_players = players; g_local = index; g_delay = delay; g_host = false; g_host_name = host + ":" + port;
    fprintf(stderr, "V2-NET: joined %s as player %d of %d (input delay %d reads), waiting for the start...\n",
            g_host_name.c_str(), index + 1, players, delay);
    for (;;) {
        if (!recv_line(s, acc, line)) { err = "the host went away before the start"; sock_close(s); return false; }
        if (line.empty() || line[0] != 'S') continue;
        // "S k:from ..." (a lobby start: this client takes part from read 1) or
        // "S I k:from ..." (a running game: the host's image follows; the J after it says from which read this client counts)
        g_late = line.compare(0, 3, "S I") == 0;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            for (int k = 0; k < V2_COOP_MAX; k++) { g_part[k] = false; g_part_from[k] = 0; g_gone[k] = false; }
            const char* c = line.c_str() + (g_late ? 3 : 1);
            int k; long from; int used;
            while (sscanf(c, " %d:%ld%n", &k, &from, &used) == 2) {
                if (k >= 0 && k < V2_COOP_MAX) { g_part[k] = true; g_part_from[k] = from; }
                c += used;
            }
            if (!g_late) { g_part[index] = true; g_part_from[index] = 0; }
        }
        break;
    }
    Peer* p = new Peer; p->s = s; p->player = 0; p->started = true;
    p->acc0 = acc;                  // whatever the host sent right after START (an image, batches) is already here
    {
        std::lock_guard<std::mutex> lk(g_peers_mx);
        g_peers.push_back(p);
    }
    start_rx(p);
    g_last_wait_log = std::chrono::steady_clock::now();
    fprintf(stderr, "V2-NET: started\n");
    return true;
}

} // namespace

// ================================================================== lobby
bool v2_net_host(int port, int players, int delay, int lobby_wait) {
    if (lobby_wait < 0 || lobby_wait > players - 1) lobby_wait = players - 1;
    g_lobby_open = true;
    if (!start_listening(port, players, delay)) return false;
    fprintf(stderr, "V2-NET: hosting %d players on port %d (input delay %d reads), waiting for %d client(s) in the lobby...\n",
            players, port, delay, lobby_wait);
    while (g_connected.load() < lobby_wait && !need_quit) SDL_Delay(20);
    // the lobby is complete: everybody in it takes part from read 1; START (with the whole list) to each
    std::vector<Peer*> lobby = peers_snapshot();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (Peer* p : lobby) if (p->alive && !p->started) { p->started = true; g_part[p->player] = true; g_part_from[p->player] = 0; }
    }
    std::string sl;
    { std::lock_guard<std::mutex> lk(g_mx); sl = start_line(false); }
    for (Peer* p : lobby) if (p->alive) send_all(p, sl);
    g_lobby_open = false;
    g_active = true;
    fprintf(stderr, "V2-NET: %s — starting\n", lobby_wait == players - 1 ? "all players in" : "the lobby is in; the others may join the running game");
    return true;
}

bool v2_net_listen(int port, int players, int delay) {
    if (g_active) { fprintf(stderr, "V2-NET: already in a network game\n"); return false; }
    g_lobby_open = false;
    if (!start_listening(port, players, delay)) return false;
    g_active = true;
    fprintf(stderr, "V2-NET: hosting %d players on port %d (input delay %d reads) — players join the running game\n", players, port, delay);
    return true;
}

bool v2_net_join(const char* host_port) {
    std::string err;
    if (!connect_and_hello(host_port, err)) { fprintf(stderr, "V2-NET: %s\n", err.c_str()); return false; }
    v2_coop_set_players(g_players);
    g_v2_local_player = g_local;
    g_active = true;
    return true;
}

bool v2_net_joined_late() { return g_late; }

bool v2_net_join_async(const char* host_port) {
    if (g_active) { fprintf(stderr, "V2-NET: already in a network game\n"); return false; }
    std::string addr = host_port ? host_port : "";
    std::thread([addr]() {
        std::string err;
        if (!connect_and_hello(addr.c_str(), err)) {
            fprintf(stderr, "V2-NET: %s\n", err.c_str());
            v2_ui_toast("JOIN FAILED");
            return;
        }
        if (!g_late) {
            // a lobby host starts everybody at read 1 — this game is already running; only a running game can be joined from the menu
            fprintf(stderr, "V2-NET: the host is still in its lobby — join it with --join at start\n");
            v2_ui_toast("HOST IN LOBBY: USE --JOIN");
            for (Peer* p : peers_snapshot()) { p->alive = false; sock_shutdown(p->s); }
            return;
        }
        g_session_players = g_players; g_session_index = g_local;
        g_session_pending = true;
        g_active = true;
        v2_input_recorder_net(g_local, 0);     // the game thread adopts the session and waits for the image at its next main read
        v2_ui_toast("CONNECTED - SYNCING");
    }).detach();
    return true;
}

void v2_net_solo(int delay) {
    g_players = 0;          // resolved lazily from g_v2_coop_players (a replay's `# coop N` header lands after main's arguments)
    v2_nopl_force_frame_ticks();
    g_local = 0; g_delay = delay; g_host = false; g_active = true;
    g_part[0] = true;       // this instance captures every player's events itself: its own batches are the only ones
    g_last_wait_log = std::chrono::steady_clock::now();
    fprintf(stderr, "V2-NET: solo lockstep, input delay %d reads\n", delay);
}

bool v2_net_active() { return g_active.load(); }
int  v2_net_delay()  { return g_delay; }
int  v2_net_peer_count() { std::lock_guard<std::mutex> lk(g_peers_mx); return (int)g_peers.size(); }
bool v2_net_is_host() { return g_host; }
int  v2_net_rtt_ms() { return g_rtt_ms.load(); }

const char* v2_net_status() {
    static char buf[96];
    if (!g_active) { snprintf(buf, sizeof buf, "NET: OFF"); return buf; }
    int live = 0;
    std::vector<Peer*> peers = peers_snapshot();
    for (Peer* p : peers) if (p->alive) live++;
    if (g_host) snprintf(buf, sizeof buf, "NET: HOST PORT %d  %d/%d PLAYERS  DELAY %d", g_port, live + 1, g_players, g_delay);
    else if (peers.empty()) snprintf(buf, sizeof buf, "NET: SOLO LOCKSTEP  DELAY %d", g_delay);
    else {
        int rtt = g_rtt_ms.load();
        if (rtt >= 0) snprintf(buf, sizeof buf, "NET: PLAYER %d OF %d  %s  PING %d MS", g_local + 1, g_players, live ? "ON" : "HOST LOST", rtt);
        else          snprintf(buf, sizeof buf, "NET: PLAYER %d OF %d  %s", g_local + 1, g_players, live ? "ON" : "HOST LOST");
    }
    return buf;
}

// ============================================================ the pipeline
void v2_net_send_batch(long read_n, const std::vector<V2NetEvent>& evs) {
    if (!g_active) return;
    if (g_players <= 0) g_players = g_v2_coop_players;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_inbox[g_local][read_n] = evs;
        g_batches_sent++;
    }
    std::vector<Peer*> peers = peers_snapshot();
    if (peers.empty()) return;
    const std::string line = encode_batch(g_local, read_n, evs);
    for (Peer* p : peers) if (p->alive && p->started) send_all(p, line);
}

bool v2_net_wait_batch(long read_n, std::vector<V2NetEvent>& out) {
    out.clear();
    if (!g_active) return true;
    if (g_players <= 0) g_players = g_v2_coop_players;
    if (read_n <= g_delay) return true;              // the first d reads have no batches: everybody's first is read d+1
    std::unique_lock<std::mutex> lk(g_mx);
    auto awaited = [&](int k) { return g_part[k] && !g_gone[k] && read_n >= g_part_from[k]; };
    auto ready = [&]() {
        for (int k = 0; k < g_players; k++)
            if (awaited(k) && g_inbox[k].find(read_n) == g_inbox[k].end()) return false;
        return true;
    };
    while (!ready()) {
        if (need_quit) return false;
        if (g_cv.wait_for(lk, std::chrono::milliseconds(200)) == std::cv_status::timeout) {
            auto now = std::chrono::steady_clock::now();
            if (now - g_last_wait_log > std::chrono::seconds(2)) {
                g_last_wait_log = now;
                std::string who;
                for (int k = 0; k < g_players; k++)
                    if (awaited(k) && g_inbox[k].find(read_n) == g_inbox[k].end()) who += " P" + std::to_string(k + 1);
                fprintf(stderr, "V2-NET: read %ld waits for%s\n", read_n, who.c_str());
            }
        }
    }
    for (int k = 0; k < g_players; k++) {
        auto it = g_inbox[k].find(read_n);
        if (it == g_inbox[k].end()) continue;        // a player who left / has not joined yet: an empty batch
        out.insert(out.end(), it->second.begin(), it->second.end());
        g_inbox[k].erase(it);
    }
    return true;
}

// ============================================================ the image channel
bool v2_net_adopt_session() {
    if (!g_session_pending.exchange(false)) return false;
    v2_coop_set_players(g_session_players);
    g_v2_local_player = g_session_index;
    return true;
}

bool v2_net_snapshot_wanted() {
    if (!g_active || !g_host) return false;
    for (Peer* p : peers_snapshot()) if (p->alive && p->needs_image) return true;
    return false;
}

void v2_net_send_image(long read_n, const std::vector<uint8_t>& img, bool everyone) {
    if (!g_active) return;
    if (g_players <= 0) g_players = g_v2_coop_players;
    char head[64];
    snprintf(head, sizeof head, "T %ld %lu\n", read_n, (unsigned long)img.size());
    std::vector<Peer*> peers = peers_snapshot();
    if (everyone) {
        // the host's state load: every peer and the host itself apply it at read_n
        for (Peer* p : peers) if (p->alive && p->started) { send_all(p, head); send_all(p, (const char*)img.data(), img.size()); }
        std::lock_guard<std::mutex> lk(g_mx);
        g_pending_image = img; g_pending_image_read = read_n; g_has_image = true;
        fprintf(stderr, "V2-NET: state image for read %ld sent to %d peer(s)\n", read_n, (int)peers.size());
        return;
    }
    // the joiners: the image, then every batch already held for reads >= read_n, then everybody learns from which read they take part
    std::vector<std::string> future;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (int k = 0; k < g_players; k++)
            for (auto& kv : g_inbox[k]) if (kv.first >= read_n) future.push_back(encode_batch(k, kv.first, kv.second));
    }
    for (Peer* p : peers) {
        if (!p->alive || !p->needs_image) continue;
        send_all(p, head);
        send_all(p, (const char*)img.data(), img.size());
        for (const std::string& l : future) send_all(p, l);
        p->needs_image = false;
        const long from = read_n + g_delay;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_part[p->player] = true; g_part_from[p->player] = from; g_gone[p->player] = false;
        }
        const std::string j = "J " + std::to_string(p->player) + " " + std::to_string(from) + "\n";
        for (Peer* q : peers) if (q->alive && q->started) send_all(q, j);
        fprintf(stderr, "V2-NET: player %d joined at read %ld (%lu-byte image, %d pending batches); takes part from read %ld\n",
                p->player + 1, read_n, (unsigned long)img.size(), (int)future.size(), from);
    }
    g_cv.notify_all();
}

bool v2_net_take_image(std::vector<uint8_t>& img, long& read_n) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_has_image) return false;
    img.swap(g_pending_image);
    g_pending_image.clear();
    read_n = g_pending_image_read;
    g_has_image = false;
    g_image_applied = true;
    return true;
}

bool v2_net_synced() { return !g_late || g_image_applied.load(); }

void v2_net_send_hash(int frame, uint32_t hash) {
    if (!g_active) return;
    std::vector<Peer*> peers = peers_snapshot();
    if (peers.empty()) return;
    if (g_host) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_hash_mine[frame] = hash;
        check_hash(frame);
    } else {
        char b[64];
        snprintf(b, sizeof b, "H %d %d %08X\n", g_local, frame, (unsigned)hash);
        send_all(peers[0], b);
        // a ping once a second (echoed by the host; the menu shows the round trip)
        uint32_t now = SDL_GetTicks();
        if (now - g_last_ping >= 1000) {
            g_last_ping = now;
            snprintf(b, sizeof b, "P %u\n", (unsigned)now);
            send_all(peers[0], b);
        }
    }
}

void v2_net_shutdown() {
    if (!g_active.exchange(false)) return;
    g_quit_net = true;
    if (g_listen != BAD_SOCK) { sock_shutdown(g_listen); sock_close(g_listen); g_listen = BAD_SOCK; }
    if (g_accept.joinable()) g_accept.join();
    for (Peer* p : peers_snapshot()) {
        p->alive = false;
        if (p->s != BAD_SOCK) {
            sock_shutdown(p->s);
            sock_close(p->s);
            p->s = BAD_SOCK;
        }
        if (p->rx.joinable()) p->rx.join();
    }
    fprintf(stderr, "V2-NET: closed — %ld batches sent, %ld received, %ld peer hashes checked, %d desync frame(s)\n",
            g_batches_sent, g_batches_recv, g_hash_checked, g_desync);
}
