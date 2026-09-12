// v2_net.cpp — the lockstep transport (see v2_net.h). Plain TCP, one line per
// message, a receiver thread per connection, a mutex + condition variable
// around the per-player inboxes. Nothing here touches the game state: the
// recorder (v2_input_recorder.cpp) turns the batches into key events at the
// read, the way it replays a file.
//
// Lines:
//   W <players> <index> <delay> <opts...>   host -> client on connect (HELLO)
//   S                                       host -> clients: everybody is in
//   I <player> <read_n> <count> <frame>/<kind>/<action> ...   a batch (relayed by the host)
//   H <player> <frame> <hash>               client -> host, the DS hash of that frame
//   B <player>                              a player left (relayed by the host)
#include "v2_net.h"
#include "v2_coop.h"
#include "v2_ui.h"
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
static int  sock_send(sock_t s, const char* p, size_t n) { return (int)send(s, p, n, MSG_NOSIGNAL); }
static int  sock_recv(sock_t s, char* p, size_t n) { return (int)recv(s, p, n, 0); }
static void sock_nodelay(sock_t s) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
static void sock_reuse(sock_t s) { int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
#endif

extern bool need_quit;

namespace {

struct Peer {
    sock_t s = BAD_SOCK;
    int player = -1;                 // host side: the client's player; client side: 0 (the host)
    std::mutex send_mx;
    std::thread rx;
    std::atomic<bool> alive{false};
    std::string acc0;                // bytes the lobby read past its last line (the receiver starts with them)
};

std::vector<Peer*> g_peers;          // host: one per client; client: [0] = the host
bool g_active = false, g_host = false;
int  g_players = 1, g_local = 0, g_delay = 2;

std::mutex g_mx;
std::condition_variable g_cv;
std::map<long, std::vector<V2NetEvent>> g_inbox[V2_COOP_MAX];   // [player][read_n] -> the batch
bool g_gone[V2_COOP_MAX] = {false, false, false};
bool g_part[V2_COOP_MAX] = {false, false, false};   // the players whose batches a read waits for (a host/client game: everybody; solo: this player only)

// host: the hashes — mine per frame, theirs per player per frame
std::map<int, uint32_t> g_hash_mine;
std::map<int, uint32_t> g_hash_peer[V2_COOP_MAX];
int  g_desync = 0;
long g_hash_checked = 0;             // host: peer hashes compared with mine
long g_batches_sent = 0, g_batches_recv = 0;
std::chrono::steady_clock::time_point g_last_wait_log;

// ---------------------------------------------------------------- sockets
bool send_all(Peer* p, const std::string& line) {
    if (!p || p->s == BAD_SOCK) return false;
    std::lock_guard<std::mutex> lk(p->send_mx);
    size_t off = 0;
    while (off < line.size()) {
        int n = sock_send(p->s, line.data() + off, line.size() - off);
        if (n <= 0) { p->alive = false; return false; }
        off += (size_t)n;
    }
    return true;
}
// one line, blocking (the lobby: before the receiver threads exist)
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
    for (Peer* p : g_peers) if (p != from && p->alive) send_all(p, line);
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
    // forget old frames
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
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
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
    v2_options_ensure_loaded();      // the cfg first, the host's values over it (the game has not started)
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

} // namespace

// ================================================================== lobby
bool v2_net_host(int port, int players, int delay) {
    if (players < 2 || players > V2_COOP_MAX) { fprintf(stderr, "V2-NET: --host needs --coop=2 or 3\n"); return false; }
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
    g_players = players; g_local = 0; g_delay = delay; g_host = true;
    for (int k = 0; k < players; k++) g_part[k] = true;
    const std::string opts = options_string();
    fprintf(stderr, "V2-NET: hosting %d players on port %d (input delay %d reads), waiting for %d client(s)...\n",
            players, port, delay, players - 1);
    for (int k = 1; k < players; k++) {
        sockaddr_in ca; socklen_t cl = sizeof ca;
        sock_t cs = accept(ls, (sockaddr*)&ca, &cl);
        if (cs == BAD_SOCK) { fprintf(stderr, "V2-NET: accept() failed\n"); sock_close(ls); return false; }
        sock_nodelay(cs);
        Peer* p = new Peer; p->s = cs; p->player = k;
        g_peers.push_back(p);
        char hello[256];
        snprintf(hello, sizeof hello, "W %d %d %d %s\n", players, k, delay, opts.c_str());
        send_all(p, hello);
        fprintf(stderr, "V2-NET: player %d connected from %s\n", k + 1, inet_ntoa(ca.sin_addr));
    }
    sock_close(ls);
    for (Peer* p : g_peers) send_all(p, "S\n");
    for (Peer* p : g_peers) start_rx(p);
    g_active = true;
    g_last_wait_log = std::chrono::steady_clock::now();
    fprintf(stderr, "V2-NET: all players in — starting\n");
    return true;
}

bool v2_net_join(const char* host_port) {
    if (!sock_init()) { fprintf(stderr, "V2-NET: socket init failed\n"); return false; }
    std::string hp = host_port ? host_port : "";
    std::string host = hp, port = "7420";
    size_t colon = hp.rfind(':');
    if (colon != std::string::npos) { host = hp.substr(0, colon); port = hp.substr(colon + 1); }
    if (host.empty()) host = "127.0.0.1";
    addrinfo hints; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        fprintf(stderr, "V2-NET: cannot resolve %s:%s\n", host.c_str(), port.c_str()); return false;
    }
    sock_t s = BAD_SOCK;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == BAD_SOCK) continue;
        if (connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) break;
        sock_close(s); s = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) { fprintf(stderr, "V2-NET: cannot connect to %s:%s\n", host.c_str(), port.c_str()); return false; }
    sock_nodelay(s);
    std::string acc, line;
    if (!recv_line(s, acc, line) || line.size() < 2 || line[0] != 'W') {
        fprintf(stderr, "V2-NET: no HELLO from the host\n"); sock_close(s); return false;
    }
    int players = 0, index = 0, delay = 2, consumed = 0;
    if (sscanf(line.c_str(), "W %d %d %d %n", &players, &index, &delay, &consumed) < 3 || players < 2 || players > V2_COOP_MAX || index < 1 || index >= players) {
        fprintf(stderr, "V2-NET: bad HELLO '%s'\n", line.c_str()); sock_close(s); return false;
    }
    apply_options(line.c_str() + consumed);
    g_players = players; g_local = index; g_delay = delay; g_host = false;
    for (int k = 0; k < players; k++) g_part[k] = true;
    v2_coop_set_players(players);
    g_v2_local_player = index;
    fprintf(stderr, "V2-NET: joined %s:%s as player %d of %d (input delay %d reads), waiting for the start...\n",
            host.c_str(), port.c_str(), index + 1, players, delay);
    for (;;) {
        if (!recv_line(s, acc, line)) { fprintf(stderr, "V2-NET: the host went away before the start\n"); sock_close(s); return false; }
        if (line == "S") break;
    }
    Peer* p = new Peer; p->s = s; p->player = 0;
    p->acc0 = acc;                  // a batch the host sent right after START may already be here
    g_peers.push_back(p);
    start_rx(p);
    g_active = true;
    g_last_wait_log = std::chrono::steady_clock::now();
    fprintf(stderr, "V2-NET: started\n");
    return true;
}

void v2_net_solo(int delay) {
    g_players = 0;          // resolved lazily from g_v2_coop_players (a replay's `# coop N` header lands after main's arguments)
    g_local = 0; g_delay = delay; g_host = false; g_active = true;
    g_part[0] = true;       // this instance captures every player's events itself: its own batches are the only ones
    g_last_wait_log = std::chrono::steady_clock::now();
    fprintf(stderr, "V2-NET: solo lockstep, input delay %d reads\n", delay);
}

bool v2_net_active() { return g_active; }
int  v2_net_delay()  { return g_delay; }
int  v2_net_peer_count() { return (int)g_peers.size(); }

// ============================================================ the pipeline
void v2_net_send_batch(long read_n, const std::vector<V2NetEvent>& evs) {
    if (!g_active) return;
    if (g_players <= 0) g_players = g_v2_coop_players;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_inbox[g_local][read_n] = evs;
        g_batches_sent++;
    }
    if (g_peers.empty()) return;
    const std::string line = encode_batch(g_local, read_n, evs);
    for (Peer* p : g_peers) if (p->alive) send_all(p, line);
}

bool v2_net_wait_batch(long read_n, std::vector<V2NetEvent>& out) {
    out.clear();
    if (!g_active) return true;
    if (g_players <= 0) g_players = g_v2_coop_players;
    if (read_n <= g_delay) return true;              // the first d reads have no batches: everybody's first is read d+1
    std::unique_lock<std::mutex> lk(g_mx);
    auto ready = [&]() {
        for (int k = 0; k < g_players; k++)
            if (g_part[k] && !g_gone[k] && g_inbox[k].find(read_n) == g_inbox[k].end()) return false;
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
                    if (g_part[k] && !g_gone[k] && g_inbox[k].find(read_n) == g_inbox[k].end()) who += " P" + std::to_string(k + 1);
                fprintf(stderr, "V2-NET: read %ld waits for%s\n", read_n, who.c_str());
            }
        }
    }
    for (int k = 0; k < g_players; k++) {
        auto it = g_inbox[k].find(read_n);
        if (it == g_inbox[k].end()) continue;        // a player who left: an empty batch
        out.insert(out.end(), it->second.begin(), it->second.end());
        g_inbox[k].erase(it);
    }
    return true;
}

void v2_net_send_hash(int frame, uint32_t hash) {
    if (!g_active || g_peers.empty()) return;
    if (g_host) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_hash_mine[frame] = hash;
        check_hash(frame);
    } else {
        char b[64];
        snprintf(b, sizeof b, "H %d %d %08X\n", g_local, frame, (unsigned)hash);
        send_all(g_peers[0], b);
    }
}

void v2_net_shutdown() {
    if (!g_active) return;
    g_active = false;
    for (Peer* p : g_peers) {
        p->alive = false;
        if (p->s != BAD_SOCK) {
#ifdef _WIN32
            shutdown(p->s, SD_BOTH);
#else
            shutdown(p->s, SHUT_RDWR);
#endif
            sock_close(p->s);
            p->s = BAD_SOCK;
        }
        if (p->rx.joinable()) p->rx.join();
    }
    fprintf(stderr, "V2-NET: closed — %ld batches sent, %ld received, %ld peer hashes checked, %d desync frame(s)\n",
            g_batches_sent, g_batches_recv, g_hash_checked, g_desync);
}
