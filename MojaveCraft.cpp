// MojaveCraft.cpp - complete single-file NVSE plugin (no xNVSE headers needed).
// Protocol definitions are embedded below (identical to mojave_shm.h).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <stdint.h>
#include <stddef.h>

// ======================================================================= protocol (mojave_shm.h)
// mojave_shm.h - MojaveCraft shared-memory protocol v2
// Shared by the FNV plugin (32-bit MinGW/GCC) and mirrored in Java (EventRing.java).
//
// Rules that keep 32-bit and 64-bit builds byte-identical:
//   * fixed-width types only, no pointers, no 64-bit fields
//   * every struct has static_asserts on size and key offsets
//   * little-endian, natural alignment
//
// File: %LOCALAPPDATA%\MojaveCraft\mojave.shm, 64 KiB, created + zeroed by the FNV side.
//
//   0x00000  MojaveTelemetry   64 B   latest-wins state, seqlock (v1-compatible offsets 0..24)
//   0x00040  MojaveControl     64 B   session id, pids, heartbeats, drop counters
//   0x00100  F2M ring header  128 B   FNV -> Minecraft event queue
//   0x00200  F2M ring data     16 KiB
//   0x04200  M2F ring header  128 B   Minecraft -> FNV event queue
//   0x04300  M2F ring data     16 KiB
//   0x08300  .. 0x0FFFF       reserved (frame transfer, etc.)
//
// Design split:
//   * continuous state (position, look direction, velocity)  -> telemetry block, latest wins
//   * discrete things that must not be lost (jump, hit, damage) -> event rings, ordered
//
// COORDINATE CONVENTION: every position/vector in the protocol is in FNV-native units
// and axes (X east, Y north, Z up, 1 unit ~ 1/70 m). Minecraft does all scaling, axis
// swapping and origin offsets. The plugin never needs to know about the mapping.

#include <stdint.h>
#include <stddef.h>

#define MOJAVE_MAGIC        0x4D4F4A56u   // same as v1
#define MOJAVE_PROTO_VER    2u
#define MOJAVE_SHM_SIZE     0x10000u

#define MOJAVE_OFF_TELEMETRY 0x00000u
#define MOJAVE_OFF_CONTROL   0x00040u
#define MOJAVE_OFF_F2M_HDR   0x00100u
#define MOJAVE_OFF_F2M_DATA  0x00200u
#define MOJAVE_OFF_M2F_HDR   0x04200u
#define MOJAVE_OFF_M2F_DATA  0x04300u
#define MOJAVE_RING_SIZE     0x4000u      // power of two; divides 2^32 so u32 indices wrap cleanly

// ---------------------------------------------------------------- telemetry (v1 layout kept)
// Writer (FNV): seq |= 1 -> write fields -> seq += 1 (even). Reader retries on odd or changed seq.
struct MojaveTelemetry {
    uint32_t magic;          //  0
    uint32_t version;        //  4  = MOJAVE_PROTO_VER
    uint32_t seq;            //  8  odd = mid-write; frame = seq / 2
    float    x, y, z;        // 12 16 20
    float    hp;             // 24
    float    yaw, pitch;     // 28 32  radians (FNV convention; verify against what the plugin reads)
    float    fov;            // 36  degrees
    uint32_t cell_form_id;   // 40
    uint32_t state_flags;    // 44  MOJAVE_ST_*
    float    vx, vy, vz;     // 48 52 56  units/second
    uint32_t reserved;       // 60
};
static_assert(sizeof(MojaveTelemetry) == 64, "telemetry size");
static_assert(offsetof(MojaveTelemetry, hp) == 24, "v1 compat");
static_assert(offsetof(MojaveTelemetry, vx) == 48, "telemetry layout");

enum MojaveStateFlags : uint32_t {
    MOJAVE_ST_PAUSED    = 1u << 0,
    MOJAVE_ST_IN_AIR    = 1u << 1,
    MOJAVE_ST_SNEAKING  = 1u << 2,
    MOJAVE_ST_SPRINTING = 1u << 3,
    MOJAVE_ST_IN_COMBAT = 1u << 4,
    MOJAVE_ST_DEAD      = 1u << 5,
    MOJAVE_ST_MENU_OPEN = 1u << 6,
};

// ---------------------------------------------------------------- control block
struct MojaveControl {
    uint32_t layout_size;        //  0  = MOJAVE_SHM_SIZE
    uint32_t session_id;         //  4  random, set by FNV at init; Java resets cursors when it changes
    uint32_t fnv_pid;            //  8
    uint32_t mc_pid;             // 12
    uint32_t fnv_heartbeat_ms;   // 16  FNV writes GetTickCount() each frame
    uint32_t mc_heartbeat_ms;    // 20  Java writes its own tick count
    uint32_t f2m_dropped;        // 24  events dropped because the ring was full
    uint32_t m2f_dropped;        // 28
    uint32_t reserved[8];        // 32..63
};
static_assert(sizeof(MojaveControl) == 64, "control size");

// ---------------------------------------------------------------- ring header
// Single-producer / single-consumer byte ring. Indices are monotonically increasing u32
// byte counters; position = index & (MOJAVE_RING_SIZE - 1). write_idx and read_idx sit on
// separate cache lines. Only the producer stores write_idx; only the consumer stores read_idx.
struct MojaveRingHeader {
    volatile uint32_t write_idx;   //  0  producer-owned
    uint32_t          capacity;    //  4  = MOJAVE_RING_SIZE (set once at init)
    uint8_t           pad0[56];    //  8
    volatile uint32_t read_idx;    // 64  consumer-owned
    uint8_t           pad1[60];    // 68
};
static_assert(sizeof(MojaveRingHeader) == 128, "ring header size");
static_assert(offsetof(MojaveRingHeader, read_idx) == 64, "ring header layout");

// ---------------------------------------------------------------- event packet
// Every record is 8-byte aligned. PAD records only need the first 4 bytes (size, type).
struct MojaveEventHdr {
    uint16_t size;     // total record bytes incl. this header, multiple of 8
    uint16_t type;     // MojaveEventType
    uint32_t seq;      // per-direction counter (detect loss/reorder)
    uint32_t tick;     // sender's frame/tick counter at emit time
    uint32_t entity;   // 0 = the local player; otherwise sender-side id (FNV refID / MC entity id)
};
static_assert(sizeof(MojaveEventHdr) == 16, "event header size");

enum MojaveEventType : uint16_t {
    MOJAVE_EV_PAD            = 0,      // internal: skip to start of ring
    MOJAVE_EV_HELLO          = 1,      // either way; payload MojaveHello

    // FNV -> Minecraft
    MOJAVE_EV_F_ACTION       = 0x10,   // MojaveAction     jump, attack, reload, sneak toggle...
    MOJAVE_EV_F_DAMAGE_TAKEN = 0x11,   // MojaveDamage     FNV player was hurt
    MOJAVE_EV_F_DAMAGE_DEALT = 0x12,   // MojaveDamage     FNV player hurt something
    MOJAVE_EV_F_CELL_CHANGE  = 0x13,   // MojaveCellChange
    MOJAVE_EV_F_ENTITY       = 0x14,   // MojaveEntityState  nearby FNV actor for a proxy in MC
    MOJAVE_EV_F_DEATH        = 0x15,   // no payload beyond header (payload size 0)

    // Minecraft -> FNV
    MOJAVE_EV_M_ACTION       = 0x80,   // MojaveAction     MC-side player did something
    MOJAVE_EV_M_DAMAGE       = 0x81,   // MojaveDamage     request: apply damage to FNV target
    MOJAVE_EV_M_CONTACT      = 0x82,   // MojaveContact    MC collision contact for an FNV entity
    MOJAVE_EV_M_ENTITY       = 0x83,   // MojaveEntityState  MC entity to show as a proxy in FNV
};

enum MojaveActionId : uint32_t {
    MOJAVE_ACT_JUMP = 1, MOJAVE_ACT_LAND, MOJAVE_ACT_ATTACK, MOJAVE_ACT_RELOAD,
    MOJAVE_ACT_SNEAK_ON, MOJAVE_ACT_SNEAK_OFF, MOJAVE_ACT_EQUIP, MOJAVE_ACT_USE,
};

struct MojaveHello       { uint32_t proto_version; uint32_t caps; };                               //  8 B
struct MojaveAction      { uint32_t action; uint32_t arg; };                                       //  8 B (arg: weapon form id for EQUIP, etc.)
struct MojaveCellChange  { uint32_t cell_form_id; uint32_t worldspace_form_id; };                  //  8 B
struct MojaveDamage {                                                                              // 32 B
    float    amount;
    uint32_t damage_type;      // 0 generic, 1 melee, 2 ballistic, 3 fall, 4 energy, 5 explosion
    uint32_t source_id;        // 0 = player
    uint32_t target_id;        // 0 = player
    float    hit_x, hit_y, hit_z;   // FNV units
    uint32_t flags;            // bit0 = critical
};
struct MojaveContact {                                                                             // 32 B
    float    px, py, pz;       // contact point, FNV units
    float    nx, ny, nz;       // unit normal pointing out of the surface
    float    depth;            // penetration, FNV units
    uint32_t flags;
};
struct MojaveEntityState {                                                                         // 32 B
    float    x, y, z;          // feet position, FNV units
    float    yaw;
    float    half_x, half_y, half_z;   // axis-aligned half extents of the bounding box
    uint32_t kind;             // sender-defined type id
};
static_assert(sizeof(MojaveDamage) == 32 && sizeof(MojaveContact) == 32 && sizeof(MojaveEntityState) == 32, "payload sizes");

// ---------------------------------------------------------------- ring helpers (GCC/Clang builtins)
// Producer: returns false if the ring is full (caller should bump the dropped counter).
inline bool mojave_ring_write(MojaveRingHeader* h, uint8_t* data,
                              uint16_t type, uint32_t seq, uint32_t tick, uint32_t entity,
                              const void* payload, uint16_t payload_size)
{
    const uint32_t cap   = MOJAVE_RING_SIZE;
    const uint32_t total = (uint32_t(sizeof(MojaveEventHdr)) + payload_size + 7u) & ~7u;
    uint32_t w = h->write_idx;                                   // only we write it
    uint32_t r = __atomic_load_n(&h->read_idx, __ATOMIC_ACQUIRE);
    uint32_t used = w - r;
    uint32_t pos  = w & (cap - 1);
    uint32_t tail = cap - pos;
    uint32_t need = total + (tail < total ? tail : 0u);          // may need a PAD record first
    if (cap - used < need) return false;

    if (tail < total) {                                          // record would straddle the end
        MojaveEventHdr* pad = reinterpret_cast<MojaveEventHdr*>(data + pos);
        pad->size = uint16_t(tail);
        pad->type = MOJAVE_EV_PAD;
        w  += tail;
        pos = 0;
    }
    MojaveEventHdr* e = reinterpret_cast<MojaveEventHdr*>(data + pos);
    e->size = uint16_t(total); e->type = type; e->seq = seq; e->tick = tick; e->entity = entity;
    uint8_t* p = reinterpret_cast<uint8_t*>(e + 1);
    for (uint16_t i = 0; i < payload_size; ++i) p[i] = static_cast<const uint8_t*>(payload)[i];
    for (uint32_t i = payload_size; i < total - sizeof(MojaveEventHdr); ++i) p[i] = 0;

    __atomic_store_n(&h->write_idx, w + total, __ATOMIC_RELEASE);  // publish
    return true;
}

// Consumer: calls fn(const MojaveEventHdr&, const uint8_t* payload) for each event; returns count.
template <class F>
inline uint32_t mojave_ring_drain(MojaveRingHeader* h, const uint8_t* data, F&& fn)
{
    const uint32_t cap = MOJAVE_RING_SIZE;
    uint32_t r = h->read_idx;                                    // only we write it
    uint32_t w = __atomic_load_n(&h->write_idx, __ATOMIC_ACQUIRE);
    uint32_t n = 0;
    while (r != w) {
        const MojaveEventHdr* e = reinterpret_cast<const MojaveEventHdr*>(data + (r & (cap - 1)));
        uint32_t sz = e->size;
        if (sz < 8 || (sz & 7) || sz > cap) { r = w; break; }    // corrupt: resync by discarding
        if (e->type != MOJAVE_EV_PAD) { fn(*e, reinterpret_cast<const uint8_t*>(e + 1)); ++n; }
        r += sz;
    }
    __atomic_store_n(&h->read_idx, r, __ATOMIC_RELEASE);
    return n;
}

// ======================================================================= plugin

// =====================================================================================
//  MojaveCraft NVSE plugin  (protocol v2)
//  Build (32-bit! FalloutNV.exe is a 32-bit process):
//    MSYS2 MINGW32 shell:  pacman -S mingw-w64-i686-gcc
//    g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ -o MojaveCraft.dll MojaveCraft.cpp
//  Install: copy MojaveCraft.dll into the folder  <FNV>/Data/NVSE/Plugins
//  Log:     %LOCALAPPDATA%/MojaveCraft/plugin.log
//
//  What is VERIFIED (recovered from the original working DLL):
//    * player singleton pointer at 0x011DEA3C, position floats at player+0x30/0x34/0x38
//    * file/mapping parameters (CreateFileA OPEN_ALWAYS, PAGE_READWRITE, FILE_MAP_ALL_ACCESS)
//    * Query checks FalloutNV.exe is loaded; plugin name "MojaveCraft"
//  What is NOT verified in-game (taken from memory of the NVSE headers - check these):
//    * rotation floats at player+0x24/0x28/0x2C (rotX = pitch, rotZ = heading, radians)
//    * parentCell pointer at player+0x40, TESForm refID at +0x0C
//    * FOV is a constant (kDefaultFov); HP is not read yet (written as 0, like the original)
//  Events are DERIVED from telemetry (jump/land from vertical velocity, cell change from the
//  cell id). They are heuristics, not game hooks, and will be noisy (stairs, slopes).
// =====================================================================================

#ifndef MOJAVE_ALLOW_64BIT
static_assert(sizeof(void*) == 4, "FalloutNV.exe is 32-bit: build with the i686 / MINGW32 toolchain");
#endif

// ----------------------------------------------------------------- minimal NVSE types
// Only the prefix of the real structs is declared; this is all Query/Load need.
struct PluginInfo { uint32_t infoVersion; const char* name; uint32_t version; };
struct NVSEInterface { uint32_t nvseVersion; uint32_t runtimeVersion; uint32_t editorVersion; uint32_t isEditor; };

// ----------------------------------------------------------------- tunables
static const uintptr_t kAddrPlayerPtr  = 0x011DEA3C;  // verified
static const uint32_t  kOffRotX        = 0x24;        // unverified (pitch)
static const uint32_t  kOffRotY        = 0x28;        // unverified
static const uint32_t  kOffRotZ        = 0x2C;        // unverified (heading / yaw)
static const uint32_t  kOffPosX        = 0x30;        // verified
static const uint32_t  kOffPosY        = 0x34;        // verified
static const uint32_t  kOffPosZ        = 0x38;        // verified
static const uint32_t  kOffParentCell  = 0x40;        // unverified
static const uint32_t  kOffRefID       = 0x0C;        // unverified
static const bool      kReadCell       = true;        // set false if cell ids look like garbage
static const float     kDefaultFov     = 75.0f;       // degrees; not read from the game yet
static const DWORD     kPollMs         = 16;          // ~60 Hz

static const bool      kDeriveMotionEvents = true;    // heuristic JUMP / LAND events
static const float     kJumpVz         = 200.0f;      // units/s upward to count as a jump
static const float     kFallVz         = -250.0f;     // units/s downward = airborne (no event)
static const float     kLandVz         = 40.0f;       // |vz| below this while airborne = landing
static const float     kVelSmoothing   = 0.5f;        // EMA factor for derived velocity
static const float     kTeleportUnits  = 2000.0f;     // jump larger than this between polls = teleport

// ----------------------------------------------------------------- globals
static MojaveTelemetry*  g_tel      = nullptr;
static MojaveControl*    g_ctl      = nullptr;
static MojaveRingHeader* g_f2mHdr   = nullptr;
static uint8_t*          g_f2mData  = nullptr;
static MojaveRingHeader* g_m2fHdr   = nullptr;
static uint8_t*          g_m2fData  = nullptr;
static uint32_t          g_f2mSeq   = 0;
static uint32_t          g_tick     = 0;
static char              g_dir[MAX_PATH] = {0};

// ----------------------------------------------------------------- logging
static void Logf(const char* fmt, ...)
{
    if (!g_dir[0]) return;
    char path[MAX_PATH + 16];
    snprintf(path, sizeof path, "%s\\plugin.log", g_dir);
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ----------------------------------------------------------------- shared memory
static bool OpenSharedMemory()
{
    char base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (n == 0 || n >= sizeof base) return false;

    snprintf(g_dir, sizeof g_dir, "%s\\MojaveCraft", base);
    CreateDirectoryA(g_dir, nullptr);                       // ok if it already exists

    char path[MAX_PATH + 16];
    snprintf(path, sizeof path, "%s\\mojave.shm", g_dir);

    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { Logf("CreateFile failed (%lu)", GetLastError()); return false; }

    HANDLE map = CreateFileMappingA(file, nullptr, PAGE_READWRITE, 0, MOJAVE_SHM_SIZE, nullptr);
    if (!map) { Logf("CreateFileMapping failed (%lu)", GetLastError()); CloseHandle(file); return false; }

    uint8_t* view = static_cast<uint8_t*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, MOJAVE_SHM_SIZE));
    if (!view) { Logf("MapViewOfFile failed (%lu)", GetLastError()); CloseHandle(map); CloseHandle(file); return false; }
    // Handles are intentionally kept open for the life of the process.

    memset(view, 0, MOJAVE_SHM_SIZE);                       // fresh session: empty rings, no stale data

    g_tel     = reinterpret_cast<MojaveTelemetry*>(view + MOJAVE_OFF_TELEMETRY);
    g_ctl     = reinterpret_cast<MojaveControl*>(view + MOJAVE_OFF_CONTROL);
    g_f2mHdr  = reinterpret_cast<MojaveRingHeader*>(view + MOJAVE_OFF_F2M_HDR);
    g_f2mData = view + MOJAVE_OFF_F2M_DATA;
    g_m2fHdr  = reinterpret_cast<MojaveRingHeader*>(view + MOJAVE_OFF_M2F_HDR);
    g_m2fData = view + MOJAVE_OFF_M2F_DATA;

    g_f2mHdr->capacity = MOJAVE_RING_SIZE;
    g_m2fHdr->capacity = MOJAVE_RING_SIZE;

    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    g_ctl->layout_size = MOJAVE_SHM_SIZE;
    g_ctl->fnv_pid     = GetCurrentProcessId();
    g_ctl->session_id  = (uint32_t)qpc.LowPart ^ (g_ctl->fnv_pid << 16) ^ GetTickCount();
    if (g_ctl->session_id == 0) g_ctl->session_id = 1;

    g_tel->version = MOJAVE_PROTO_VER;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_tel->magic = MOJAVE_MAGIC;                            // magic last: readers ignore the file until now

    Logf("Shared memory ready (%u bytes, session %08x)", MOJAVE_SHM_SIZE, g_ctl->session_id);
    return true;
}

// ----------------------------------------------------------------- game memory
struct Snapshot {
    float x, y, z;
    float pitch, yaw;
    uint32_t cell;
};

static bool ReadPlayer(Snapshot& s)
{
    if (IsBadReadPtr(reinterpret_cast<const void*>(kAddrPlayerPtr), 4)) return false;
    uintptr_t player = *reinterpret_cast<const uint32_t*>(kAddrPlayerPtr);
    if (!player) return false;                              // main menu / loading
    if (IsBadReadPtr(reinterpret_cast<const void*>(player), 0x44)) return false;

    const uint8_t* b = reinterpret_cast<const uint8_t*>(player);
    memcpy(&s.x,     b + kOffPosX, 4);
    memcpy(&s.y,     b + kOffPosY, 4);
    memcpy(&s.z,     b + kOffPosZ, 4);
    memcpy(&s.pitch, b + kOffRotX, 4);
    memcpy(&s.yaw,   b + kOffRotZ, 4);
    if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z)) return false;
    if (!std::isfinite(s.pitch)) s.pitch = 0.0f;
    if (!std::isfinite(s.yaw))   s.yaw   = 0.0f;

    s.cell = 0;
    if (kReadCell) {
        uint32_t cellPtr = 0;
        memcpy(&cellPtr, b + kOffParentCell, 4);
        if (cellPtr && !IsBadReadPtr(reinterpret_cast<const void*>(cellPtr), 0x10))
            memcpy(&s.cell, reinterpret_cast<const uint8_t*>(cellPtr) + kOffRefID, 4);
    }
    return true;
}

// ----------------------------------------------------------------- telemetry (seqlock writer)
static void WriteTelemetry(const Snapshot& s, float vx, float vy, float vz, uint32_t flags)
{
    MojaveTelemetry* t = g_tel;
    __atomic_fetch_add(&t->seq, 1u, __ATOMIC_SEQ_CST);      // odd: write in progress
    t->x = s.x; t->y = s.y; t->z = s.z;
    t->hp = 0.0f;                                           // not read yet (original also wrote 0)
    t->yaw = s.yaw; t->pitch = s.pitch; t->fov = kDefaultFov;
    t->cell_form_id = s.cell;
    t->state_flags = flags;
    t->vx = vx; t->vy = vy; t->vz = vz;
    __atomic_fetch_add(&t->seq, 1u, __ATOMIC_SEQ_CST);      // even: consistent
}

// ----------------------------------------------------------------- events (FNV -> Minecraft)
static void Emit(uint16_t type, const void* payload, uint16_t size)
{
    uint32_t seq = g_f2mSeq++;                              // always advance so the reader can see loss
    if (!mojave_ring_write(g_f2mHdr, g_f2mData, type, seq, g_tick, 0, payload, size))
        g_ctl->f2m_dropped++;
}

struct Motion {
    bool   have = false;
    float  px = 0, py = 0, pz = 0;
    double t = 0;
    float  vx = 0, vy = 0, vz = 0;
    bool   air = false;
    int    calm = 0;
    double nextJumpAllowed = 0;
    uint32_t cell = 0;
};
static Motion g_m;

static uint32_t UpdateMotion(const Snapshot& s, double now)
{
    // cell change -> event, and drop velocity history so a teleport doesn't look like a sprint
    if (s.cell != 0 && s.cell != g_m.cell) {
        MojaveCellChange cc = { s.cell, 0 };
        Emit(MOJAVE_EV_F_CELL_CHANGE, &cc, sizeof cc);
        g_m.cell = s.cell;
        g_m.have = false;
        g_m.vx = g_m.vy = g_m.vz = 0; g_m.air = false; g_m.calm = 0;
    }

    if (!g_m.have) {
        g_m.have = true; g_m.px = s.x; g_m.py = s.y; g_m.pz = s.z; g_m.t = now;
    } else {
        double dt = now - g_m.t;
        if (dt > 0.004) {
            float dx = s.x - g_m.px, dy = s.y - g_m.py, dz = s.z - g_m.pz;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist > kTeleportUnits) {
                g_m.vx = g_m.vy = g_m.vz = 0; g_m.air = false; g_m.calm = 0;
            } else {
                float inv = (float)(1.0 / dt);
                g_m.vx += kVelSmoothing * (dx * inv - g_m.vx);
                g_m.vy += kVelSmoothing * (dy * inv - g_m.vy);
                g_m.vz += kVelSmoothing * (dz * inv - g_m.vz);
            }
            g_m.px = s.x; g_m.py = s.y; g_m.pz = s.z; g_m.t = now;

            if (kDeriveMotionEvents) {
                if (!g_m.air && g_m.vz > kJumpVz && now >= g_m.nextJumpAllowed) {
                    g_m.air = true; g_m.calm = 0; g_m.nextJumpAllowed = now + 0.3;
                    MojaveAction a = { MOJAVE_ACT_JUMP, 0 };
                    Emit(MOJAVE_EV_F_ACTION, &a, sizeof a);
                } else if (!g_m.air && g_m.vz < kFallVz) {
                    g_m.air = true; g_m.calm = 0;           // walked off a ledge: airborne, no event
                } else if (g_m.air) {
                    if (fabsf(g_m.vz) < kLandVz) {
                        if (++g_m.calm >= 2) {
                            g_m.air = false; g_m.calm = 0;
                            MojaveAction a = { MOJAVE_ACT_LAND, 0 };
                            Emit(MOJAVE_EV_F_ACTION, &a, sizeof a);
                        }
                    } else g_m.calm = 0;
                }
            }
        }
    }
    return g_m.air ? MOJAVE_ST_IN_AIR : 0u;
}

// ----------------------------------------------------------------- events (Minecraft -> FNV)
static uint32_t g_mcEventsSeen = 0;

static void HandleMinecraftEvent(const MojaveEventHdr& e, const uint8_t* p)
{
    ++g_mcEventsSeen;
    bool verbose = g_mcEventsSeen <= 50;                    // don't flood the log
    switch (e.type) {
    case MOJAVE_EV_HELLO: {
        MojaveHello h; memcpy(&h, p, sizeof h);
        Logf("MC hello: proto %u caps %08x", h.proto_version, h.caps);
        break; }
    case MOJAVE_EV_M_ACTION: {
        MojaveAction a; memcpy(&a, p, sizeof a);
        if (verbose) Logf("MC action %u arg %u (entity %u)", a.action, a.arg, e.entity);
        // TODO: perform the action in FNV (needs game function addresses / xNVSE headers)
        break; }
    case MOJAVE_EV_M_DAMAGE: {
        MojaveDamage d; memcpy(&d, p, sizeof d);
        if (verbose) Logf("MC damage request %.1f -> target %u", d.amount, d.target_id);
        // TODO: apply damage to the FNV actor
        break; }
    case MOJAVE_EV_M_CONTACT: {
        MojaveContact c; memcpy(&c, p, sizeof c);
        if (verbose) Logf("MC contact depth %.1f normal (%.2f %.2f %.2f)", c.depth, c.nx, c.ny, c.nz);
        // TODO: push the FNV player/actor out along the normal
        break; }
    case MOJAVE_EV_M_ENTITY: {
        if (verbose) Logf("MC entity state (entity %u)", e.entity);
        // TODO: show a proxy for the MC entity in FNV
        break; }
    default:
        if (verbose) Logf("MC event type 0x%x ignored", e.type);
    }
}

// ----------------------------------------------------------------- poll thread
static DWORD WINAPI PollThread(LPVOID)
{
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    const double invFreq = 1.0 / (double)freq.QuadPart;

    MojaveHello hello = { MOJAVE_PROTO_VER, 0x3u };         // caps: bit0 telemetry v2, bit1 derived events
    Emit(MOJAVE_EV_HELLO, &hello, sizeof hello);

    for (;;) {
        ++g_tick;
        g_ctl->fnv_heartbeat_ms = GetTickCount();
        mojave_ring_drain(g_m2fHdr, g_m2fData, HandleMinecraftEvent);

        Snapshot s;
        if (ReadPlayer(s)) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            uint32_t flags = UpdateMotion(s, (double)now.QuadPart * invFreq);
            WriteTelemetry(s, g_m.vx, g_m.vy, g_m.vz, flags);
        }
        Sleep(kPollMs);
    }
    return 0;
}

// ----------------------------------------------------------------- NVSE entry points
extern "C" {

__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
    if (!GetModuleHandleA("FalloutNV.exe")) return false;   // same check as the original plugin
    if (nvse && nvse->isEditor) return false;
    info->infoVersion = 1;
    info->name        = "MojaveCraft";
    info->version     = 2;
    return true;
}

__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface*)
{
    if (!OpenSharedMemory()) return false;
    HANDLE th = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    if (!th) { Logf("CreateThread failed"); return false; }
    CloseHandle(th);
    Logf("MojaveCraft plugin loaded (protocol v%u)", MOJAVE_PROTO_VER);
    return true;
}

} // extern "C"
