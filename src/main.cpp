#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <mmsystem.h>
#include <winver.h>

#undef ERROR

#include <RE/Starfield.h>
#include <RE/T/TESTopicInfo.h>
#include <RE/T/TESResponse.h>
#include <RE/T/TESForm.h>
#include <SFSE/SFSE.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

#include "UnityChat.h"
#include "UnityComm.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "version.lib")

// Shown in the in-game status line and the log. Bump for each build you hand out.
static constexpr const char* kPluginVersion = "0.1.0";
// The Wwise path/cookie hook offsets below are for this game version only.
static constexpr const char* kSupportedGameVersion = "1.16.244";

// --- Path configuration ---
// Nothing here is machine-specific any more. Everything resolves from the
// GAME folder (the folder containing Starfield.exe, found via
// GetModuleFileNameA(nullptr)), which is also what Papyrus's relative
// ".\\Data\\SFSE\\MantellaStarfield\\" path resolves against. That matters
// for MO2: inside the game process MO2's virtual filesystem maps
// <game>\Data\SFSE\MantellaStarfield\ onto <MO2>\overwrite\SFSE\MantellaStarfield\,
// so the plugin and Papyrus land in the same real folder with zero config,
// whether or not MO2 is used. Only the Python watcher (which runs OUTSIDE the
// game and can't see MO2's virtual Data) needs to be told the real folder.
//
// Optional overrides, in MantellaStarfield_Config.ini next to this DLL:
//   [Paths]
//   BridgeDir=...            ; default <game>\Data\SFSE\MantellaStarfield\ (trailing slash)
//   VoiceDir1=...            ; default <game>\Data\Sound\Voice\MantellaStarfield.esp
//   VoiceDir2=...            ; optional second voice base (e.g. the MO2 mod folder)
// Populated once by LoadPathConfig() during SFSE_PLUGIN_LOAD.
static std::string g_bridgeDir;
static std::string g_bridgeIni;
// Queue file — Python is the SOLE writer (write_index + [line_N] sections);
// C++ is the sole reader. Separate file from Bridge.ini so the two processes
// never write the same file (no read-modify-write race).
static std::string g_queueIni;
static const char* BRIDGE_DIR = nullptr;  // set to g_bridgeDir.c_str() by LoadPathConfig()
static const char* BRIDGE_INI = nullptr;  // set to g_bridgeIni.c_str() by LoadPathConfig()
static const char* QUEUE_INI = nullptr;   // set to g_queueIni.c_str() by LoadPathConfig()

// Voice output bases ActivateLipsForCurrentLine() copies ffxanims into.
static std::vector<std::string> g_voiceBases;
// MantellaStarfield_Config.ini next to the DLL (may not exist - every key has a default).
static std::filesystem::path g_configPath;

static std::string ReadConfigString(const std::filesystem::path& configPath,
                                    const char* key, const std::string& fallback)
{
    char buffer[MAX_PATH] = {};
    GetPrivateProfileStringA("Paths", key, fallback.c_str(),
                             buffer, sizeof(buffer), configPath.string().c_str());
    return std::string(buffer);
}

static void LoadPathConfig()
{
    // This DLL's own folder (for the config file), resolved via an address
    // inside this module so the DLL's filename doesn't matter.
    char modulePath[MAX_PATH] = {};
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&LoadPathConfig), &hSelf);
    GetModuleFileNameA(hSelf, modulePath, MAX_PATH);
    std::filesystem::path pluginDir = std::filesystem::path(modulePath).parent_path();
    std::filesystem::path configPath = pluginDir / "MantellaStarfield_Config.ini";
    g_configPath = configPath;

    // The game folder (Starfield.exe's folder).
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path gameDir = std::filesystem::path(exePath).parent_path();

    const std::string defaultBridgeDir = (gameDir / "Data" / "SFSE" / "MantellaStarfield").string() + "\\";
    const std::string defaultVoiceDir  = (gameDir / "Data" / "Sound" / "Voice" / "MantellaStarfield.esp").string();

    g_bridgeDir = ReadConfigString(configPath, "BridgeDir", defaultBridgeDir);
    if (!g_bridgeDir.empty() && g_bridgeDir.back() != '\\' && g_bridgeDir.back() != '/') g_bridgeDir += '\\';
    g_bridgeIni = g_bridgeDir + "Bridge.ini";
    g_queueIni = g_bridgeDir + "MantellaQueue.ini";
    BRIDGE_DIR = g_bridgeDir.c_str();
    BRIDGE_INI = g_bridgeIni.c_str();
    QUEUE_INI = g_queueIni.c_str();

    std::error_code ec;
    std::filesystem::create_directories(g_bridgeDir, ec);

    g_voiceBases.clear();
    std::string v1 = ReadConfigString(configPath, "VoiceDir1", defaultVoiceDir);
    std::string v2 = ReadConfigString(configPath, "VoiceDir2", "");
    if (!v1.empty()) g_voiceBases.push_back(v1);
    if (!v2.empty() && v2 != v1) g_voiceBases.push_back(v2);

    std::error_code existsErr;
    const bool haveConfig = std::filesystem::exists(configPath, existsErr);
    REX::INFO("Mantella: game folder '{}'", gameDir.string());
    REX::INFO("Mantella: config file '{}' ({})", configPath.string(),
              haveConfig ? "found" : "not found - using defaults");
    REX::INFO("Mantella: Bridge directory resolved to '{}'{}", g_bridgeDir,
              g_bridgeDir == defaultBridgeDir ? " (default)" : " (from config)");
    for (const auto& b : g_voiceBases) {
        REX::INFO("Mantella: voice base '{}'", b);
    }
}

std::string GetBridgeDir()
{
    return g_bridgeDir;
}

static std::atomic<bool> g_conversationActive{ false };
// Bumped every time a NEW conversation start is accepted. A running
// RunConversationLoop captures the generation value active when it began and
// checks it every iteration. Because std::thread::detach() means the plugin
// has no way to know whether a previous loop thread has actually finished
// (only whether g_conversationActive currently says it should have), Escape
// resetting g_conversationActive alone doesn't guarantee a stale thread has
// noticed and exited before a fresh G-press spawns a second, concurrent loop.
// Two loops racing to write Bridge.ini's request/nonce fields is what
// produces the "have to press G/Escape a few times before it works"
// symptom. Checking generation (not just the boolean) makes a superseded
// loop self-terminate on its very next check, regardless of how many new
// starts have been accepted since.
static std::atomic<uint64_t> g_conversationGeneration{ 0 };
static std::atomic<bool> g_mantellaSpeaking{ false };
static std::atomic<std::uint32_t> g_currentSpeakerFormID{ 0 };
static const SFSE::TaskInterface* g_taskInterface = nullptr;

// -----------------------------------------------------------------------
// Activated NPC roster (session-only, manual activation).
//
// Mirrors CHIM's AIAgentManager pattern (Plugin/Globals.h): a roster of
// NPCs the player has explicitly opted into AI-driven behavior, tracked by
// FormID. This is the prerequisite for any multi-NPC feature (group
// conversations, later rechat/bored-event support) - none of that can work
// until the plugin has *some* notion of "which NPCs are eligible", since
// today only a single current-speaker FormID is ever tracked.
//
// Deliberately NOT persisted across saves (session-only) - matches CHIM's
// "manually added" roster concept without taking on cosave serialization
// work until/unless persistence is actually needed.
// -----------------------------------------------------------------------
struct ActivatedNpc {
    std::uint32_t formId;
    std::string name;
    // Bored-event bookkeeping (see MaybeTriggerBoredEvent below). Session-
    // only, same as the roster itself - resets to 0 on toggle-off/on or
    // process restart, which is fine since fairness only needs to hold
    // within a session.
    std::uint32_t boredEventsFired = 0;
    DWORD lastBoredEventTick = 0;
};

static std::vector<ActivatedNpc> g_activatedNpcs;
static std::mutex g_activatedNpcsMutex;

// Player's FormID is always 0x14 in Bethesda's Creation Engine (Skyrim and
// Starfield both). Hardcoded guard, same as CHIM's Plugin.cpp check, so we
// never accidentally activate the player as if they were an NPC.
static constexpr std::uint32_t kPlayerFormID = 0x14;

// FormIDs cross into Papyrus as decimal strings parsed with "as Int", and
// Papyrus Int is SIGNED 32-bit. Any FormID >= 0x80000000 - light (ESL)
// plugins (0xFE......), runtime-spawned references (0xFF......) - written as
// an unsigned decimal ("4278190080") overflows that parse and resolves to the
// wrong form or none. Writing the same 32 bits as a signed value
// ("-16777216") round-trips exactly through Game.GetForm(). This is also the
// form Papyrus itself writes (GetFormID() as String), which C++'s stoul
// already reads back correctly.
static std::string FormIdToPapyrusString(std::uint32_t formId)
{
    return std::to_string(static_cast<std::int32_t>(formId));
}

bool IsNpcActivated(std::uint32_t formId)
{
    std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
    for (const auto& npc : g_activatedNpcs) {
        if (npc.formId == formId) return true;
    }
    return false;
}

// Returns true if the actor is now ACTIVE, false if now INACTIVE (i.e. this
// call just deactivated them). Returns std::nullopt-equivalent (false with
// no roster change) if the actor fails an eligibility guard.
enum class ToggleActivationResult { NowActive, NowInactive, Rejected };

ToggleActivationResult ToggleNpcActivation(RE::Actor* actor, std::string& outReason)
{
    if (!actor) {
        outReason = "no target actor";
        return ToggleActivationResult::Rejected;
    }
    std::uint32_t formId = actor->GetFormID();
    if (formId == kPlayerFormID || actor->IsPlayer()) {
        outReason = "cannot activate the player";
        return ToggleActivationResult::Rejected;
    }
    if (actor->IsDead()) {
        outReason = "actor is dead";
        return ToggleActivationResult::Rejected;
    }
    if (actor->IsDisabled() || actor->IsDeleted()) {
        outReason = "actor is disabled/deleted";
        return ToggleActivationResult::Rejected;
    }

    std::string name = actor->GetDisplayFullName();
    if (name.empty()) {
        outReason = "actor has no display name";
        return ToggleActivationResult::Rejected;
    }

    std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
    for (auto it = g_activatedNpcs.begin(); it != g_activatedNpcs.end(); ++it) {
        if (it->formId == formId) {
            // Already active - toggling deactivates. Dedup guard: this is
            // also what prevents double-adding the same NPC if pressed twice.
            g_activatedNpcs.erase(it);
            outReason = name;
            return ToggleActivationResult::NowInactive;
        }
    }
    g_activatedNpcs.push_back({ formId, name });
    outReason = name;
    return ToggleActivationResult::NowActive;
}

// Keeps the full activated roster mirrored in Bridge.ini as indexed keys
// (formid_0, formid_1, ... plus name_0, name_1, ... and a count), written on
// every roster change (called from the toggle_activate handler in
// BridgeMonitorThread) so Papyrus's Delete-key handler can read it
// synchronously with zero round-trip latency. Deliberately the FULL roster,
// not a single precomputed "default" - initial version of this picked the
// most-recently-activated NPC on the C++ side, but that was an invented
// heuristic, not something ported from CHIM. Checked CHIM's actual
// HTTPManager::stream() listener resolution (SpatialSnapshotManager-based,
// audibility-ranked, its own full subsystem - too large to port tonight)
// and landed on proximity as the closest cheap approximation of the same
// underlying signal. Doing the distance computation in Papyrus itself
// (Actor.GetDistance()) rather than precomputing "nearest" here in C++
// keeps it live at the instant of keypress instead of trusting a snapshot
// that could be a few hundred ms stale - so this function's only job is
// exposing the roster, not deciding who's nearest.
//
// FormID, not name - the same lesson tonight's bored-event fix already
// established: names captured via C++'s GetDisplayFullName() are not
// guaranteed to match what Papyrus's GetFormsByTESFullName searches
// against, so any cross-boundary actor handoff goes through FormID
// (Game.GetForm() on the Papyrus side) instead.
//
// Stale indexed keys beyond the current count are harmless - Papyrus only
// ever reads formid_0..formid_(count-1), so a shrunk roster just leaves
// unread orphan keys rather than needing explicit cleanup.
void WriteFreeformRoster()
{
    std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
    WritePrivateProfileStringA("freeform", "count",
        std::to_string(g_activatedNpcs.size()).c_str(), BRIDGE_INI);
    for (std::size_t i = 0; i < g_activatedNpcs.size(); ++i) {
        const std::string formidKey = "formid_" + std::to_string(i);
        WritePrivateProfileStringA("freeform", formidKey.c_str(),
            FormIdToPapyrusString(g_activatedNpcs[i].formId).c_str(), BRIDGE_INI);
    }
}


namespace {
bool EqualsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](char c1, char c2) {
        return std::tolower(static_cast<unsigned char>(c1)) ==
               std::tolower(static_cast<unsigned char>(c2));
    });
}
}

// Declared in UnityChat.h so UnityChat.cpp (which builds each streamed
// line's work-file entry) can call it. Defined here since the roster and
// g_currentSpeakerFormID both live in this file. (That header declaration
// was missing until this session - the comment always claimed it existed,
// but nothing actually forward-declared this for cross-file use before now.)
std::uint32_t ResolveActorFormIdByName(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
        for (const auto& npc : g_activatedNpcs) {
            if (EqualsIgnoreCase(npc.name, name)) {
                return npc.formId;
            }
        }
    }
    // No roster match - fall back to whoever was originally targeted via G.
    // Correct by construction for ordinary single-NPC conversations: every
    // streamed line's actor name will just be that same NPC.
    return g_currentSpeakerFormID.load();
}

// Declared in UnityChat.h so UnityChat.cpp can call it. Unlike
// ResolveActorFormIdByName above, this deliberately does NOT fall back to
// g_currentSpeakerFormID - a caller trying to detect "does this word
// actually name a roster NPC" needs a genuine no-match signal, not a
// best-effort guess dressed up as a real answer.
std::string FindActivatedNpcNameExcluding(const std::string& candidateName,
                                          const std::string& excludeName)
{
    std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
    for (const auto& npc : g_activatedNpcs) {
        if (EqualsIgnoreCase(npc.name, excludeName)) continue;
        if (EqualsIgnoreCase(npc.name, candidateName)) {
            return npc.name;
        }
    }
    return "";
}

// Picks which activated NPC should answer a player message, so the player
// isn't locked to whoever the conversation started with:
//   1. an activated NPC the message ADDRESSES by name (full or first name,
//      case-insensitive): at the start ("Noel, ...", "hey Noel ...") or at
//      the end after a comma ("..., Noel?"). Merely mentioning someone
//      ("Is that Noel's data?") does not redirect the message,
//   2. otherwise the activated NPC under the crosshair when it was sent,
//   3. otherwise the current conversation partner.
// Done here rather than in Papyrus because Cassiopeia's FindStr is
// case-sensitive ("noel" would never match "Noel").
std::string ResolveInputTargetName(const std::string& text, std::uint32_t crosshairFormId,
                                   const std::string& currentSpeaker)
{
    auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    auto isWordChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '\'';
    };
    // Position of `needle` in `hay` as a whole word/phrase, or npos.
    auto findWord = [&](const std::string& hay, const std::string& needle) -> size_t {
        if (needle.empty()) return std::string::npos;
        for (size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + 1)) {
            bool startOk = pos == 0 || !isWordChar(hay[pos - 1]);
            size_t end = pos + needle.size();
            bool endOk = end >= hay.size() || !isWordChar(hay[end]) ||
                         (hay[end] == '\'' && end + 1 < hay.size() && hay[end + 1] == 's');  // "Noel's"
            if (startOk && endOk) return pos;
        }
        return std::string::npos;
    };

    // Trim to the words that can carry a vocative: the start of the message
    // (after a leading greeting/filler word) and the tail after the last comma.
    std::string msg = lower(text);
    while (!msg.empty() && !isWordChar(msg.back())) msg.pop_back();   // trailing ?!.
    size_t lead = msg.find_first_not_of(" \t");
    msg = (lead == std::string::npos) ? "" : msg.substr(lead);
    static const char* kFillers[] = { "hey", "hi", "hello", "yo", "ok", "okay", "so", "well", "and", "but", "oh" };
    std::string head = msg;
    for (const char* f : kFillers) {
        const std::string w = std::string(f) + " ";
        const std::string wc = std::string(f) + ", ";
        if (head.rfind(wc, 0) == 0) { head = head.substr(wc.size()); break; }
        if (head.rfind(w, 0) == 0) { head = head.substr(w.size()); break; }
    }
    const size_t lastComma = msg.rfind(',');
    std::string tail = (lastComma == std::string::npos) ? "" : msg.substr(lastComma + 1);
    size_t tl = tail.find_first_not_of(" \t");
    tail = (tl == std::string::npos) ? "" : tail.substr(tl);

    // Name must START `head`, or BE the whole of `tail`.
    auto addressedAs = [&](const std::string& name) -> int {   // 0 = no, 1 = start, 2 = end
        if (name.empty()) return 0;
        if (findWord(head, name) == 0 &&
            !(head.size() > name.size() && head[name.size()] == '\'')) return 1;   // not "Noel's ..."
        if (!tail.empty() && tail == name) return 2;
        return 0;
    };

    std::string bestName;
    int bestRank = 0;
    std::string crosshairName;
    {
        std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
        for (const auto& npc : g_activatedNpcs) {
            const std::string full = lower(npc.name);
            const std::string first = full.substr(0, full.find(' '));
            int rank = addressedAs(full);
            if (rank == 0 && first.size() >= 3) rank = addressedAs(first);
            if (rank != 0 && (bestRank == 0 || rank < bestRank)) {   // start beats end
                bestRank = rank;
                bestName = npc.name;
            }
            if (crosshairFormId != 0 && npc.formId == crosshairFormId) crosshairName = npc.name;
        }
    }
    if (!bestName.empty()) return bestName;
    if (!crosshairName.empty()) return crosshairName;
    return currentSpeaker;
}

// Forward declarations - defined later in this file, needed here since
// the bored-event functions below (PlayBoredEventQueue, MaybeTriggerBoredEvent)
// are inserted right after the roster helpers, ahead of their definitions.
int ReadQueueWriteIndex();
bool ReadQueueTurnDone();
void PlayQueuedLine(int idx);

// -----------------------------------------------------------------------
// Bored-event support - ported from CHIM's AIAgentManager pattern
// (CHIM Plugin/Globals.h: getLessBoredAgentNearby), NOT from STOBE.
// STOBE's TriggerBoredEvent does a live world-proximity scan using
// Kenshi/Ogre APIs (world->getCharacterUpdateList(), isIndoors(),
// IsConversationAreaCompatible()) with no CommonLibSF equivalent. CHIM's
// version instead distance-filters an already-known agent roster - the
// same shape as g_activatedNpcs above - and picks the least-recently-
// fired eligible NPC. That's the pattern followed here.
//
// Divergence from CHIM: CHIM's own trigger site sends only the selected
// actor over HTTP (HTTPManager::stream("bored|...", selectedActor) - no
// people list attached). UNITYServer's processor/bored.php, confirmed by
// reading it directly, explicitly expects a people= param
// (extractParticipantNames()), so SendUnityBoredEvent (UnityChat.cpp)
// attaches BuildActivatedRosterPeopleParam() even though CHIM's own
// trigger doesn't bother.
//
// API VERIFICATION STATUS (updated after first real build + in-game test):
// - RE::PlayerCharacter::GetSingleton(), ->GetPosition() compiled clean and
//   behave as expected.
// - ->IsSneaking() compiled clean AND is a legitimate implementation
//   (signature-scanned via REL::Relocation against ID::Actor::IsSneaking,
//   same pattern as the confirmed-working IsJumping() right next to it in
//   Actor.h) - trustworthy.
// - ->IsInCombat() compiled clean but was NOT semantically correct: it's a
//   bare `virtual bool IsInCombat() const;` vtable-slot declaration with no
//   body, sitting in a stretch of Actor.h's vtable that's mostly literal
//   "Unk_16A/16B/16D/16F/171-174" unknowns - i.e. an unverified/guessed
//   offset, not a confirmed one. Compiling clean only meant the vtable slot
//   existed at all, not that it pointed at the right function. Confirmed
//   broken in-game: reported "player in combat" continuously while the
//   player was demonstrably not in combat. Removed from the gate entirely
//   below rather than trusted or "fixed" - the offset itself would need to
//   be independently re-verified by the commonlib-starfield project before
//   it's safe to rely on again.
// - ->GetParentCell() did NOT exist (confirmed via the actual build error
//   and cross-checked against RE/T/TESObjectREFR.h): parentCell is a public
//   data member, not a getter. ->IsInteriorCell() was also wrong - the real
//   method is IsInterior() (RE/T/TESObjectCELL.h). Both fixed below using
//   player->parentCell / cell->IsInterior() - these have NOT yet been
//   in-game confirmed to produce correct interior/exterior radius behavior,
//   only confirmed to compile.
// - Distance/radius values themselves (kBoredEventRadiusInterior/Exterior)
//   are still unmeasured placeholders - a clean compile doesn't confirm
//   those numbers are right for Starfield, only that the calls resolve.
// -----------------------------------------------------------------------

static DWORD g_lastBoredEventTick = 0;

// Real-time gate on how often we even ASK the server about a bored event -
// NOT how often one actually fires. That distinction matters: confirmed by
// reading bored.php directly that it already has its own real probability
// gate (mt_rand(0,99) against the NPC profile's BORED_EVENT_CHANCE
// setting, falling back to a global default) - if the roll fails, it just
// returns "ok" with nothing generated, and BoredEventThread already
// handles that gracefully (AppendEndTurnMarker() fires unconditionally
// regardless of whether any content came back, so nothing gets stuck).
//
// Value matches CHIM's own GlobalBoredEventTimeOut (Plugin.cpp:285,
// checked against std::chrono::seconds - same real-time basis this value
// uses) - confirmed via direct source check, not guessed. An earlier
// version of this comment argued for shrinking this to 5s on the theory
// that "ask often, let probability decide" was the right split of
// responsibility - that reasoning wasn't wrong in principle, but was
// applied without first checking what either reference implementation
// actually does. Neither CHIM (60s real-time) nor STOBE (3h in-game
// default, configurable 1-720h) treats their own check-interval as a
// throwaway formality with probability doing all the real work - both
// use a deliberate, substantial spacing as the PRIMARY control, with
// their own server-side chance layered on top as an additional throttle,
// not a replacement for one. 5s didn't match either architecture; it was
// closer to "ask constantly" than anything either mature implementation
// actually does.
static DWORD kBoredEventIntervalMs = 60 * 1000;          // [Tuning] BoredIntervalSeconds

// Per-NPC cooldown - the same activated NPC can't fire back-to-back bored
// events even if the global interval above has elapsed and they're still
// "least bored" (e.g. only one NPC is currently activated). Independent of
// the probability-gate change above - this still serves its own purpose
// (variety across multiple activated NPCs), not touched by this change.
static DWORD kPerNpcBoredCooldownMs = 2 * 60 * 1000;     // [Tuning] BoredPerNpcCooldownSeconds

// Search radius in game units. Ported from CHIM's DISTANCE_ACTIVATING_NPC_IN/OUT
// at the same relative indoor/outdoor ratio, NOT measured against actual
// Starfield distances - these are placeholders, almost certainly wrong as
// absolute values. Tune once you can observe trigger behavior in-game.
static float kBoredEventRadiusInterior = 2000.0f;        // [Tuning] BoredRadiusInterior
static float kBoredEventRadiusExterior = 4000.0f;        // [Tuning] BoredRadiusExterior

static std::atomic<bool> g_boredEventsEnabled{ true };  // [Tuning] BoredEventsEnabled

// Extra wait after each line's audio before the next line starts.
static int g_playbackBufferMs = 700;                     // [Tuning] PlaybackBufferMs

// -----------------------------------------------------------------------
// Runtime config: [Keys] and [Tuning] in MantellaStarfield_Config.ini.
//
// Keys are resolved here and handed to Papyrus through Bridge.ini [keys],
// because Papyrus properties are baked into saves - changing a default in
// the .psc never reaches an existing save, and players shouldn't need the
// Creation Kit to rebind a key. Papyrus re-applies [keys] on every load.
// -----------------------------------------------------------------------

static std::string TrimAscii(std::string v)
{
    const char* ws = " \t\r\n\"";
    size_t a = v.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(ws);
    return v.substr(a, b - a + 1);
}

static std::string ReadConfigValue(const char* section, const char* key)
{
    char buffer[256] = {};
    GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), g_configPath.string().c_str());
    // Allow trailing ";" comments on the same line.
    std::string v(buffer);
    size_t semi = v.find(';');
    if (semi != std::string::npos) v = v.substr(0, semi);
    return TrimAscii(v);
}

// Accepts a key name ("G", "F6", "NumpadMinus", "Delete"), a hex code
// ("0x47") or a decimal code ("71"). Codes are Windows virtual-key codes,
// which is what CassiopeiaPapyrusExtender's BSInputEvent has reported for
// every key tested so far (letters, Delete, Escape, Numpad Minus) - with one
// known exception: Numpad Plus came through as 18 (Alt). So prefer letters
// and F-keys, and test anything exotic in game.
static int ParseKeyName(const std::string& raw, int fallback, const char* settingName)
{
    std::string v = TrimAscii(raw);
    if (v.empty()) return fallback;
    std::string u = v;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    u.erase(std::remove(u.begin(), u.end(), ' '), u.end());

    try {
        if (u.rfind("0X", 0) == 0) return std::stoi(u.substr(2), nullptr, 16);
        if (std::all_of(u.begin(), u.end(), [](unsigned char c) { return std::isdigit(c); }) && u.size() > 1)
            return std::stoi(u);
    } catch (...) {}

    if (u.size() == 1 && ((u[0] >= 'A' && u[0] <= 'Z') || (u[0] >= '0' && u[0] <= '9'))) return u[0];
    if (u.size() >= 2 && u.size() <= 3 && u[0] == 'F') {
        try { int n = std::stoi(u.substr(1)); if (n >= 1 && n <= 24) return 0x70 + n - 1; } catch (...) {}
    }
    if (u.rfind("NUMPAD", 0) == 0 && u.size() == 7 && std::isdigit(static_cast<unsigned char>(u[6]))) return 0x60 + (u[6] - '0');

    static const std::pair<const char*, int> kNamed[] = {
        {"NUMPADMINUS", 0x6D}, {"NUMPADSUBTRACT", 0x6D}, {"NUMPADPLUS", 0x6B}, {"NUMPADADD", 0x6B},
        {"NUMPADMULTIPLY", 0x6A}, {"NUMPADDIVIDE", 0x6F}, {"NUMPADDECIMAL", 0x6E}, {"NUMPADPERIOD", 0x6E},
        {"DELETE", 0x2E}, {"DEL", 0x2E}, {"INSERT", 0x2D}, {"INS", 0x2D}, {"HOME", 0x24}, {"END", 0x23},
        {"PAGEUP", 0x21}, {"PAGEDOWN", 0x22}, {"ESCAPE", 0x1B}, {"ESC", 0x1B}, {"TAB", 0x09},
        {"BACKSPACE", 0x08}, {"ENTER", 0x0D}, {"RETURN", 0x0D}, {"SPACE", 0x20}, {"CAPSLOCK", 0x14},
        {"UP", 0x26}, {"DOWN", 0x28}, {"LEFT", 0x25}, {"RIGHT", 0x27},
        {"GRAVE", 0xC0}, {"TILDE", 0xC0}, {"MINUS", 0xBD}, {"EQUALS", 0xBB}, {"LEFTBRACKET", 0xDB},
        {"RIGHTBRACKET", 0xDD}, {"SEMICOLON", 0xBA}, {"APOSTROPHE", 0xDE}, {"COMMA", 0xBC},
        {"PERIOD", 0xBE}, {"SLASH", 0xBF}, {"BACKSLASH", 0xDC},
    };
    for (const auto& [name, code] : kNamed) {
        if (u == name) return code;
    }
    REX::INFO("Mantella: WARNING: [Keys] {}='{}' not recognised - keeping default 0x{:X}", settingName, raw, fallback);
    return fallback;
}

struct KeySetting { const char* configKey; const char* bridgeKey; int defaultCode; };
static const KeySetting kKeySettings[] = {
    // configKey          bridgeKey    default (matches the .psc property defaults)
    { "Talk",             "talk",      0x47 },  // G
    { "AddMoreText",      "continue",  0x54 },  // T
    { "EndConversation",  "end",       0x1B },  // Escape
    { "RenameNpc",        "rename",    0x4E },  // N
    { "ToggleActivation", "activate",  0x6D },  // Numpad Minus
    { "TalkToNearest",    "nearest",   0x2E },  // Delete
    { "PushToTalk",       "voice",     0x56 },  // V
};

static void LoadRuntimeConfig()
{
    // [Tuning]
    auto readInt = [](const char* key, long long fallback) -> long long {
        std::string v = ReadConfigValue("Tuning", key);
        if (v.empty()) return fallback;
        try { return std::stoll(v); } catch (...) {
            REX::INFO("Mantella: WARNING: [Tuning] {}='{}' is not a number - using {}", key, v, fallback);
            return fallback;
        }
    };
    auto readFloat = [](const char* key, float fallback) -> float {
        std::string v = ReadConfigValue("Tuning", key);
        if (v.empty()) return fallback;
        try { return std::stof(v); } catch (...) {
            REX::INFO("Mantella: WARNING: [Tuning] {}='{}' is not a number - using {}", key, v, fallback);
            return fallback;
        }
    };
    kBoredEventIntervalMs = static_cast<DWORD>(std::max<long long>(5, readInt("BoredIntervalSeconds", 60)) * 1000);
    kPerNpcBoredCooldownMs = static_cast<DWORD>(std::max<long long>(0, readInt("BoredPerNpcCooldownSeconds", 120)) * 1000);
    kBoredEventRadiusInterior = readFloat("BoredRadiusInterior", 2000.0f);
    kBoredEventRadiusExterior = readFloat("BoredRadiusExterior", 4000.0f);
    g_boredEventsEnabled.store(readInt("BoredEventsEnabled", 1) != 0);
    g_playbackBufferMs = static_cast<int>(std::clamp<long long>(readInt("PlaybackBufferMs", 700), 0, 5000));
    REX::INFO("Mantella: tuning - bored events {} every {}s (per-NPC cooldown {}s, radius {}/{}), playback buffer {}ms",
              g_boredEventsEnabled.load() ? "on" : "off", kBoredEventIntervalMs / 1000, kPerNpcBoredCooldownMs / 1000,
              kBoredEventRadiusInterior, kBoredEventRadiusExterior, g_playbackBufferMs);

    // [Keys] -> Bridge.ini [keys]
    std::string summary;
    for (const auto& k : kKeySettings) {
        int code = ParseKeyName(ReadConfigValue("Keys", k.configKey), k.defaultCode, k.configKey);
        WritePrivateProfileStringA("keys", k.bridgeKey, std::to_string(code).c_str(), BRIDGE_INI);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s%s=0x%X", summary.empty() ? "" : ", ", k.configKey, code);
        summary += buf;
    }
    // [Experimental] DeliveryMode: "scene" (default) or "say". In "say" mode
    // Papyrus plays each line with ObjectReference.Say() instead of starting a
    // scene - see DeliverLineWithSay in the .psc. Applied with the keys below.
    std::string delivery = ReadConfigValue("Experimental", "DeliveryMode");
    std::transform(delivery.begin(), delivery.end(), delivery.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (delivery != "say") delivery = "scene";
    WritePrivateProfileStringA("settings", "delivery", delivery.c_str(), BRIDGE_INI);
    REX::INFO("Mantella: line delivery mode - {}", delivery);

    // Bridge.ini outlives the game session - drop last session's status so a
    // stale "UNITY ready" can't show before this session's first real check.
    WritePrivateProfileStringA("status", "message", "", BRIDGE_INI);

    // Papyrus re-applies keys whenever this changes (i.e. once per game launch).
    WritePrivateProfileStringA("keys", "version", std::to_string(GetTickCount64()).c_str(), BRIDGE_INI);
    REX::INFO("Mantella: keys - {}", summary);
}

// Guards against two PlayBoredEventQueue threads running concurrently.
// Confirmed as a real bug in-game: a zero-line bored event (no listener
// available) can leave its playback thread stuck polling indefinitely
// (never observing turn_done - root cause on the Python/unity_wem_gen.py
// side not yet fully diagnosed, but irrelevant to this fix). If a second
// bored event then fires before that thread exits, both threads read/write
// the same MantellaQueue.ini/Bridge.ini [speaking] state concurrently -
// reproduced directly: two distinct thread IDs both logged "Playing line 0"
// for the same turn and both logged "turn_done reached" at the identical
// timestamp. Nothing was audible in-game because the race corrupted the
// handoff to Papyrus's timer 4 before it could observe a stable state.
static std::atomic<bool> g_boredEventPlaybackActive{ false };

struct BoredCandidate {
    std::uint32_t formId;
    std::string name;
};

// Player-state gate, mirroring CHIM's checks before it calls
// getLessBoredAgentNearby at all (Plugin/Plugin.cpp ~line 2380). Only
// checks session state on the UNITY side now (g_conversationActive/
// g_mantellaSpeaking, already existing elsewhere and reused here) plus a
// player-singleton null check - see the crash/removal history below for
// why the combat/sneak checks CHIM does are absent.
//
// REMOVED, both confirmed broken rather than just unported:
// - IsInCombat(): compiled clean but was a bare, unverified vtable-slot
//   declaration (Actor.h, surrounded by literal "Unk_16A/16B/16D/..."
//   neighbors) that reported "in combat" continuously in-game while the
//   player demonstrably was not.
// - IsSneaking(): a legitimate signature-scanned implementation in
//   general (REL::Relocation against ID::Actor::IsSneaking, same pattern
//   as the working IsJumping()), but crashed the game outright the first
//   time it actually ran: "REL/IDDB.cpp(417): Failed to find offset for
//   Address Library ID! Invalid ID: 0, Game Version: 1.16.244.0" - i.e.
//   that specific ID isn't populated for this game build, regardless of
//   the pattern being sound in principle.
// Net effect: neither of Starfield CommonLibSF's two most obvious player-
// combat/stealth-state APIs can currently be trusted from this codebase.
// Revisit both if/when the commonlib-starfield project fixes the
// underlying offset/ID, rather than retrying either as currently written.
//
// ALSO NOT PORTED (unrelated to the above): CHIM also checks
// player->GetCurrentScene() and MenuTopicManager->unkB1 (player in a
// scene / vanilla dialogue menu). Skipped since neither has an obviously-
// equivalent Starfield API confirmed anywhere in this codebase yet -
// revisit if bored events end up firing while the player is in some
// other blocking UI/scene state.
bool IsBoredEventEligibleNow(std::string* outReason = nullptr)
{
    if (!g_boredEventsEnabled.load()) {
        if (outReason) *outReason = "bored events disabled";
        return false;
    }
    if (g_conversationActive.load()) {
        if (outReason) *outReason = "conversation active";
        return false;
    }
    if (g_mantellaSpeaking.load()) {
        if (outReason) *outReason = "currently speaking";
        return false;
    }

    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        if (outReason) *outReason = "no player singleton";
        return false;
    }

    return true;
}

// Distance-filters g_activatedNpcs against the player, sorts by
// boredEventsFired ascending (least-recently-featured NPC first - the
// same fairness rotation as getLessBoredAgentNearby's sort), and returns
// the first eligible candidate: not on its own per-NPC cooldown, resolves
// to a live actor, not dead/disabled/deleted, within radius.
std::optional<BoredCandidate> GetLeastBoredNearbyNpc()
{
    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return std::nullopt;

    float radius = kBoredEventRadiusExterior;
    // CONFIRMED via lib\commonlibsf\include\RE\T\TESObjectREFR.h and
    // TESObjectCELL.h (checked against a real build failure): parentCell is
    // a public data member, not a getter (there's only SetParentCell, no
    // Get-), and the interior check is IsInterior(), not IsInteriorCell().
    if (RE::TESObjectCELL* cell = player->parentCell) {
        if (cell->IsInterior()) radius = kBoredEventRadiusInterior;
    }

    RE::NiPoint3 playerPos = player->GetPosition();

    std::vector<ActivatedNpc> rosterCopy;
    {
        std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
        rosterCopy = g_activatedNpcs;
    }

    std::sort(rosterCopy.begin(), rosterCopy.end(),
              [](const ActivatedNpc& a, const ActivatedNpc& b) {
                  return a.boredEventsFired < b.boredEventsFired;
              });

    DWORD now = GetTickCount();
    for (const auto& npc : rosterCopy) {
        if (npc.lastBoredEventTick != 0 &&
            (now - npc.lastBoredEventTick) < kPerNpcBoredCooldownMs) {
            continue;
        }

        RE::Actor* actor = RE::TESForm::LookupByID<RE::Actor>(npc.formId);
        if (!actor) continue;
        if (actor->IsDead() || actor->IsDisabled() || actor->IsDeleted()) continue;

        RE::NiPoint3 actorPos = actor->GetPosition();
        float dx = actorPos.x - playerPos.x;
        float dy = actorPos.y - playerPos.y;
        float dz = actorPos.z - playerPos.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > radius) continue;

        return BoredCandidate{ npc.formId, npc.name };
    }
    return std::nullopt;
}

// Updates the real roster entry (not the sorted copy above) after a bored
// event actually fires for this NPC.
void MarkNpcBoredEventFired(std::uint32_t formId)
{
    std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
    for (auto& npc : g_activatedNpcs) {
        if (npc.formId == formId) {
            npc.boredEventsFired++;
            npc.lastBoredEventTick = GetTickCount();
            break;
        }
    }
}

// Standalone drain loop for a bored event's MantellaQueue.ini output.
// Deliberately independent of g_conversationActive/g_conversationGeneration:
// a bored event happens precisely when no player conversation is active,
// and must not be gated on (or able to interfere with) that state machine.
// Otherwise mirrors RunConversationLoop's STATE B playback exactly - same
// PlayQueuedLine/read_index/turn_done mechanics, just without the
// player_input half of that loop's state machine.
//
// Writes [bored] type=bored_end to Bridge.ini on both exit paths (normal
// completion and the conversation-interrupt abort) - this is the signal
// MantellaDialogueQuestScript.psc's aiTimerID==4 handler polls for to know
// when to clear BoredEventActive and stop re-arming itself. See
// MaybeTriggerBoredEvent below for the matching bored_start write and the
// full explanation of why this needed a dedicated [bored] section rather
// than reusing [request]/ConversationActive.
//
// Watchdog: exits after kBoredEventPlaybackTimeoutMs of NO PROGRESS - reset
// every time a line is actually played, not measured from thread start.
// The original version measured from thread start unconditionally, which
// was fine for the narrow case it was built for (a zero-line/no-listener
// bored event that never reaches turn_done at all) but was confirmed via a
// real in-game rechat test to be actively harmful for anything longer: a
// bored event that continues into a multi-hop rechat chain can legitimately
// run well past 20 seconds of real playback time, and the old absolute-
// timeout version killed the drain thread mid-chain regardless of whether
// new lines were still actively arriving and playing. Everything queued
// after the kill was silently dropped - the server kept generating a full
// ongoing exchange, but nothing was left watching the queue to play any of
// it, which looked exactly like "the conversation just stops" or "only the
// first line or two ever plays" depending on timing. Root cause of a
// TRULY zero-line/no-listener turn never reaching turn_done is still
// undiagnosed (Python/unity_wem_gen.py side), but g_boredEventPlaybackActive
// already blocks a new bored event from firing while a previous thread is
// still "active", so a thread that legitimately never finishes still can't
// block bored events permanently for the rest of the session - this
// watchdog is only the safety net for that one specific stuck case now,
// not a ceiling on how long a healthy, actively-progressing chain can run.
static constexpr DWORD kBoredEventPlaybackTimeoutMs = 20 * 1000;

void PlayBoredEventQueue()
{
    int localRead = 0;
    DWORD lastProgressTick = GetTickCount();
    while (true) {
        if (g_conversationActive.load()) {
            REX::INFO("UNITY: bored event playback aborted - player conversation started");
            WritePrivateProfileStringA("bored", "type", "bored_end", BRIDGE_INI);
            g_boredEventPlaybackActive.store(false);
            return;
        }

        if (GetTickCount() - lastProgressTick > kBoredEventPlaybackTimeoutMs) {
            REX::ERROR("UNITY: bored event playback watchdog timeout - no progress for {} ms (localRead={})",
                      kBoredEventPlaybackTimeoutMs, localRead);
            WritePrivateProfileStringA("bored", "type", "bored_end", BRIDGE_INI);
            g_boredEventPlaybackActive.store(false);
            return;
        }

        int writeIndex = ReadQueueWriteIndex();
        if (localRead < writeIndex) {
            g_mantellaSpeaking.store(true);
            PlayQueuedLine(localRead);
            localRead++;
            WritePrivateProfileStringA("queue", "read_index",
                std::to_string(localRead).c_str(), QUEUE_INI);
            g_mantellaSpeaking.store(false);
            lastProgressTick = GetTickCount();
            continue;
        }

        if (ReadQueueTurnDone()) {
            REX::INFO("UNITY: bored event turn_done reached and queue drained");
            WritePrivateProfileStringA("bored", "type", "bored_end", BRIDGE_INI);
            g_boredEventPlaybackActive.store(false);
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Called periodically from BridgeMonitorThread's existing poll loop (no
// new polling thread needed). Checks the global cooldown and eligibility
// gate, picks a candidate, and if one exists, resets the queue, fires
// SendUnityBoredEvent, and spawns PlayBoredEventQueue to play the
// response as it streams in.
void MaybeTriggerBoredEvent()
{
    DWORD now = GetTickCount();

    // Lightweight diagnostic heartbeat, throttled to once per ~10s so it
    // doesn't spam the 500ms poll loop this runs on. Added after the first
    // in-game test produced zero bored events with no way to tell why.
    static DWORD lastDiagTick = 0;
    bool shouldLogDiag = (now - lastDiagTick) >= 10000;
    if (shouldLogDiag) lastDiagTick = now;

    if (now - g_lastBoredEventTick < kBoredEventIntervalMs) {
        if (shouldLogDiag) {
            REX::INFO("UNITY: bored event check - global cooldown, {} ms remaining",
                      kBoredEventIntervalMs - (now - g_lastBoredEventTick));
        }
        return;
    }

    // Confirmed real bug: a zero-line bored event (no listener) can leave
    // its PlayBoredEventQueue thread stuck polling indefinitely, never
    // observing turn_done. If a new bored event fired anyway, its own
    // thread would run concurrently with the stuck one, both reading/
    // writing the same MantellaQueue.ini/Bridge.ini [speaking] state -
    // reproduced directly in-game as silent playback (two thread IDs both
    // logged "Playing line 0" for the same turn). This check is the fix:
    // refuse to fire a new bored event while a previous one might still be
    // playing back, regardless of why it hasn't finished.
    if (g_boredEventPlaybackActive.load()) {
        if (shouldLogDiag) {
            REX::INFO("UNITY: bored event check - previous bored playback still active");
        }
        return;
    }

    std::string ineligibleReason;
    if (!IsBoredEventEligibleNow(&ineligibleReason)) {
        if (shouldLogDiag) {
            REX::INFO("UNITY: bored event check - not eligible ({})", ineligibleReason);
        }
        return;
    }

    auto candidate = GetLeastBoredNearbyNpc();
    if (!candidate) {
        if (shouldLogDiag) {
            std::size_t rosterSize = 0;
            {
                std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
                rosterSize = g_activatedNpcs.size();
            }
            REX::INFO("UNITY: bored event check - no eligible nearby NPC (roster size={})",
                      rosterSize);
        }
        return;
    }

    g_lastBoredEventTick = now;
    MarkNpcBoredEventFired(candidate->formId);

    REX::INFO("UNITY: bored event firing for '{}' (0x{:X})",
              candidate->name, candidate->formId);

    // Signals MantellaDialogueQuestScript.psc's heartbeat (aiTimerID==3,
    // which already runs unconditionally every 0.5s) to resolve the actor,
    // set BoredEventActive, and start aiTimerID==4's speaking-poll loop.
    // Deliberately a SEPARATE [bored] section, not [request]: [request] is
    // exclusively Papyrus-to-C++ (BridgeMonitorThread's nonce-dedupe loop
    // treats any new nonce there as an incoming player/UI action), so C++
    // writing into it here would risk being misread as one on the very
    // next 500ms poll. A dedicated section keeps the two directions from
    // ever colliding.
    static int boredNonce = 0;
    boredNonce++;
    WritePrivateProfileStringA("bored", "type", "bored_start", BRIDGE_INI);
    WritePrivateProfileStringA("bored", "speaker", candidate->name.c_str(), BRIDGE_INI);
    // Decimal, not hex - Papyrus's "String as Int" cast expects plain
    // digits. Written specifically because GetFormsByTESFullName(candidate->name)
    // was confirmed in-game to return 0 forms for names captured via C++'s
    // GetDisplayFullName(), even for NPCs GetFormsByTESFullName resolves
    // correctly moments later during a normal conversation - that working
    // path's name originates from Papyrus's own GetReferenceName() instead,
    // round-tripped through the server. Different name source, evidently
    // not guaranteed to produce an identical string. FormID sidesteps the
    // whole problem: no string matching needed at all.
    WritePrivateProfileStringA("bored", "speaker_formid", FormIdToPapyrusString(candidate->formId).c_str(), BRIDGE_INI);
    WritePrivateProfileStringA("bored", "nonce", std::to_string(boredNonce).c_str(), BRIDGE_INI);

    {
        std::ofstream qf(QUEUE_INI, std::ios::trunc);
        qf << "[queue]\nwrite_index=0\nread_index=0\nturn_done=0\n";
    }

    g_boredEventPlaybackActive.store(true);
    SendUnityBoredEvent(candidate->name, 1);
    std::thread(PlayBoredEventQueue).detach();
}

static std::uintptr_t g_baseAddr = 0;

// Cookie counter — unique value per line busts Wwise external source cache
static std::atomic<std::uint32_t> g_cookieCounter{ 0x10000001 };

// Flag: currently processing a Mantella external source
static std::atomic<bool> g_inMantellaPlay{ false };

// Current wem path — updated before each line
static std::string g_currentWemPath;
static std::mutex g_wemPathMutex;

// Redirected path buffer
static char g_redirectedPathBuf[2048]{};


void PatchDialogueSubtitle(const std::string& text)
{
    if (text.empty()) return;

    // Cache INFO form lookup — only search once per session
    static RE::TESTopicInfo* s_infoForm = nullptr;
    if (!s_infoForm) {
        for (std::uint32_t pluginIdx = 0x01; pluginIdx < 0xFF; pluginIdx++) {
            std::uint32_t infoFormID = (pluginIdx << 24) | 0x000826;
            auto* candidate = RE::TESForm::LookupByID<RE::TESTopicInfo>(infoFormID);
            if (candidate) {
                s_infoForm = candidate;
                REX::INFO("Mantella: Found INFO form at plugin index 0x{:X}", pluginIdx);
                break;
            }
        }
    }
    RE::TESTopicInfo* infoForm = s_infoForm;

    if (!infoForm) {
        REX::INFO("Mantella: Could not find INFO form for subtitle patch");
        return;
    }

    auto* response = infoForm->responses;
    if (!response) {
        REX::INFO("Mantella: No responses in INFO form");
        return;
    }

    response->responseText = std::string_view{ text };
    REX::INFO("Mantella: Patched subtitle to: {}", text);
}

// -----------------------------------------------------------------------
// Path hook at 0xF1C8B1
// Intercepts FUN_1427dda80 call that builds the narrow wem path
// Detects Mantella wems and sets flag for cookie hook
// -----------------------------------------------------------------------
using PathBuildFunc_t = const char*(*)(void*);
static PathBuildFunc_t g_origPathBuild = nullptr;

const char* HookedPathBuild(void* buffer)
{
    const char* path = g_origPathBuild(buffer);
    if (path && strstr(path, "MantellaStarfield.esp")) {
        REX::INFO("Mantella: Path hook detected Mantella wem: {}", path);
        g_inMantellaPlay.store(true);

        // Redirect to current wem filename — unique filename = fresh Wwise cache entry
        std::lock_guard<std::mutex> lock(g_wemPathMutex);
        if (!g_currentWemPath.empty()) {
            // g_currentWemPath is absolute MO2 path, extract "Sound\Voice\..." portion
            auto pos = g_currentWemPath.find("Sound\\Voice\\");
            if (pos == std::string::npos) pos = g_currentWemPath.find("sound\\voice\\");
            if (pos == std::string::npos) pos = g_currentWemPath.find("SOUND\\Voice\\");
            if (pos != std::string::npos) {
                std::string rel = g_currentWemPath.substr(pos);
                strncpy_s(g_redirectedPathBuf, sizeof(g_redirectedPathBuf), rel.c_str(), _TRUNCATE);
                REX::INFO("Mantella: Path hook redirected to: {}", g_redirectedPathBuf);
                return g_redirectedPathBuf;
            }
        }
    } else {
        g_inMantellaPlay.store(false);
    }
    return path;
}

// -----------------------------------------------------------------------
// Cookie hook at 0xF1C9A6
// Intercepts FUN_142cd8640("External_Source") hash call
// Returns unique counter so Wwise treats each line as a new source (cache miss)
// -----------------------------------------------------------------------
using HashFunc_t = std::uint32_t(*)(const char*);
static HashFunc_t g_origHashFunc = nullptr;

std::uint32_t HookedHashFunc(const char* str)
{
    std::uint32_t result = g_origHashFunc(str);
    if (g_inMantellaPlay.load()) {
        std::uint32_t newCookie = g_cookieCounter.fetch_add(1);
        REX::INFO("Mantella: Cookie hook - replaced 0x{:X} with unique 0x{:X}", result, newCookie);
        g_inMantellaPlay.store(false);
        return newCookie;
    }
    return result;
}

std::string ReadIniValue(const char* section, const char* key, const char* fallback = "")
{
    char buffer[1024]{};
    GetPrivateProfileStringA(section, key, fallback, buffer, sizeof(buffer), BRIDGE_INI);
    return std::string(buffer);
}

// Read a value from the QUEUE file (Python-written line queue). Larger buffer
// because subtitle lines can be long.
std::string ReadQueueValue(const char* section, const char* key, const char* fallback = "")
{
    static thread_local char buffer[4096];
    buffer[0] = '\0';
    GetPrivateProfileStringA(section, key, fallback, buffer, sizeof(buffer), QUEUE_INI);
    return std::string(buffer);
}

int ReadQueueWriteIndex()
{
    std::string v = ReadQueueValue("queue", "write_index", "0");
    try { return std::stoi(v); } catch (...) { return 0; }
}

bool ReadQueueTurnDone()
{
    std::string v = ReadQueueValue("queue", "turn_done", "0");
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    return v == "1" || v == "true" || v == "yes";
}

std::string EscapeJson(const std::string& input)
{
    std::ostringstream out;
    for (char c : input) {
        switch (c) {
        case '"':  out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n";  break;
        case '\r': out << "\\r";  break;
        case '\t': out << "\\t";  break;
        default:   out << c;      break;
        }
    }
    return out.str();
}

// Serializes the activated-NPC roster into the ?people= JSON array format
// reverse-engineered from processor/bored.php's extractParticipantIdentityToken()
// (lib/data_functions.php): an array of "Name|StorageID" strings, split on
// the first '|', with StorageID optional (falls back to name-only matching
// server-side if omitted).
//
// UPDATE: StorageID is no longer omitted. STOBE's reference implementation
// (ChatBox.cpp: AppendUniquePerson(people, speaker.name + "|" + speaker.serial))
// confirms the mature CHIM/STOBE lineage always populates this token with
// the character's engine handle - Starfield's FormID is the direct
// equivalent, so it's included here now rather than left as a guess.
std::string BuildActivatedRosterPeopleParam()
{
    std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
    std::string json = "[";
    bool first = true;
    for (const auto& npc : g_activatedNpcs) {
        if (!first) json += ",";
        first = false;
        char formIdHex[16];
        snprintf(formIdHex, sizeof(formIdHex), "%X", npc.formId);
        // Hex, no "0x" prefix, matching this file's own logging convention
        // (see the roster-dump snprintf a few lines up in toggle_activate).
        // Whether extractParticipantIdentities() on the PHP side actually
        // expects hex vs. decimal here is still unconfirmed - StorageID
        // being optional means a mismatched format should just fail closed
        // to name-only matching rather than break anything, but worth
        // checking lib/data_functions.php if NPC resolution ever seems off.
        json += "\"" + EscapeJson(npc.name) + "|" + formIdHex + "\"";
    }
    json += "]";
    return json;
}

// (Removed: the original Mantella client - SendToMantella() posting to
// localhost:4999 plus its start/continue/player-input/end JSON builders.
// Nothing called any of it after the UNITYServer port; see git history.)

void WriteAwaitInput(const std::string& lastNonce)
{
    int nonce = 0;
    try { nonce = std::stoi(lastNonce); } catch (...) {}
    std::string newNonce = std::to_string(nonce + 100);
    WritePrivateProfileStringA("request", "type", "await_input", BRIDGE_INI);
    WritePrivateProfileStringA("request", "nonce", newNonce.c_str(), BRIDGE_INI);
    REX::INFO("Mantella: Wrote await_input to bridge, nonce={}", newNonce);
}

void WriteSpeakingState(float duration)
{
    std::string durationStr = std::to_string(duration);
    WritePrivateProfileStringA("speaking", "duration", durationStr.c_str(), BRIDGE_INI);
    // The Papyrus scene fires only when BOTH active==1 and wem_ready==1. In the
    // queue design the wem is always already generated+deployed by the time we
    // play a line, so we assert both here to trigger the scene. (wem_ready no
    // longer gates path capture — the queue/index does — it's purely the Papyrus
    // "a line is ready to speak" signal now.)
    WritePrivateProfileStringA("speaking", "wem_ready", "1", BRIDGE_INI);
    WritePrivateProfileStringA("speaking", "active", "1", BRIDGE_INI);
    REX::INFO("Mantella: Wrote speaking state, duration={}s", duration);
}

// Set the current wem path explicitly (used by the queue loop, which gets the
// path from a [line_N] section rather than the single bridge slot).
void SetCurrentWemPath(const std::string& wemPath)
{
    if (wemPath.empty()) return;
    std::lock_guard<std::mutex> lock(g_wemPathMutex);
    g_currentWemPath = wemPath;
    REX::INFO("Mantella: Current wem path set to: {}", wemPath);
}


// Activate the lips for the line that is about to play.
//
// Generation (Python) now deploys each line's ffxanim to that line's OWN slot
// (e.g. NPCFSarahMorgan\00666A7A.ffxanim) and does NOT touch the shared
// 00666A79.ffxanim read-slot. The scene always reads lips from 00666A79.ffxanim
// in whatever voice folder the INFO record points at, so right as a line starts
// we copy THIS line's slot ffxanim onto 00666A79.ffxanim in every read folder.
//
// Because this is keyed to the specific line starting (derived from
// g_currentWemPath), generating later lines ahead of time can never corrupt the
// lips of the line currently playing.
void ActivateLipsForCurrentLine()
{
    std::string wemPath;
    {
        std::lock_guard<std::mutex> lock(g_wemPathMutex);
        wemPath = g_currentWemPath;
    }
    if (wemPath.empty()) return;

    namespace fs = std::filesystem;
    // Derive the line's ffxanim from its wem: same folder, same basename, .ffxanim.
    fs::path wem(wemPath);
    std::string slot = wem.stem().string();               // e.g. "00666A7A"
    fs::path lineFfxanim = wem.parent_path() / (slot + ".ffxanim");

    if (!fs::exists(lineFfxanim)) {
        REX::INFO("Mantella: ActivateLips - line ffxanim missing: {}", lineFfxanim.string());
        return;
    }

    // Folders the scene may read 00666A79.ffxanim from. The line's own folder is
    // wem.parent_path(); the generics are where spawned/unnamed NPCs read.
    //
    // Deliberately does NOT include "NPCMBarrett"/"NPCFSarahMorgan" - they
    // used to be here, treated as just two more generic slots per the
    // comment above, but they're actually specific NAMED NPCs' own
    // folders. Every line (regardless of speaker) got copied into BOTH of
    // them unconditionally via the loop below, which was invisible as
    // long as only one NPC was ever genuinely "in scene" at a time (an
    // NPC's own turns are already correctly handled by the
    // wem.parent_path() copy just above this list, so this was a harmless
    // redundant write on their own lines) - but once two NPCs are
    // genuinely present together (rechat), every OTHER speaker's line
    // kept overwriting Sarah's own folder with THEIR lip data, and
    // whatever's currently sitting in that file is what her face animates
    // from regardless of which actor the Papyrus alias is bound to.
    // Confirmed via a real two-NPC rechat test: Noel's voice played while
    // Sarah's mouth moved, on every line, matching this exactly.
    std::vector<std::string> folders = {
        "GenericMaleRough", "GenericMaleSmooth",
        "GenericMale01", "GenericMale02", "GenericMale03", "GenericMale04",
        "GenericFemaleRough", "GenericFemaleSmooth",
        "GenericFemale01", "GenericFemale02", "GenericFemale03", "GenericFemale04"
    };

    // Snapshot the source's real size so we can confirm the destination
    // actually matches after the copy, not just assume copy_file worked.
    std::error_code sizeErr;
    auto srcSize = fs::file_size(lineFfxanim, sizeErr);

    int successCount = 0;
    int failCount = 0;

    auto copyAndVerify = [&](const fs::path& dst) {
        std::error_code ec;
        bool ok = fs::copy_file(lineFfxanim, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            REX::ERROR("Mantella: ActivateLips COPY FAILED for {} -> {} : {} ({})",
                       lineFfxanim.string(), dst.string(), ec.message(), ec.value());
            failCount++;
            return;
        }
        // copy_file can report success while the OS silently did nothing
        // useful (e.g. a locked/memory-mapped destination on Windows can
        // behave inconsistently) - verify the byte count actually landed.
        std::error_code verifyErr;
        auto dstSize = fs::file_size(dst, verifyErr);
        if (verifyErr || dstSize != srcSize) {
            REX::ERROR("Mantella: ActivateLips SIZE MISMATCH after copy: {} "
                       "(src={} dst={}, ok={})",
                       dst.string(), srcSize, dstSize, ok);
            failCount++;
            return;
        }
        successCount++;
    };

    // 1) The line's own folder (covers named NPCs reading their own folder).
    copyAndVerify(wem.parent_path() / "00666A79.ffxanim");

    // 2) All generic + registered folders under every configured voice base.
    for (const auto& base : g_voiceBases) {
        for (const auto& folder : folders) {
            fs::path dst = fs::path(base) / folder / "00666A79.ffxanim";
            std::error_code e2;
            fs::create_directories(dst.parent_path(), e2);
            copyAndVerify(dst);
        }
    }
    REX::INFO("Mantella: Activated lips for slot {} - {} succeeded, {} FAILED (src size={})",
              slot, successCount, failCount, srcSize);
}

// Play a single queued line: capture its path, activate its lips, patch its
// subtitle, start the scene, and sleep out its duration. Returns when the line
// has finished playing.
void PlayQueuedLine(int idx)
{
    std::string sec = "line_" + std::to_string(idx);
    std::string wemPath  = ReadQueueValue(sec.c_str(), "wem_path", "");
    std::string subtitle = ReadQueueValue(sec.c_str(), "subtitle", "");
    std::string durStr   = ReadQueueValue(sec.c_str(), "duration", "2.0");
    // Name of the NPC who actually speaks THIS line, straight from the
    // server's streamed actor field (see UnityChat.cpp). Empty if the queue
    // line predates this change - Papyrus treats an empty name as "keep
    // whichever actor the scene already has," matching old single-NPC
    // behavior exactly. No FormID resolution happens on the C++ side: there
    // is no Int FormID->Form reverse-lookup native function available in
    // CassiopeiaPapyrusExtender, so Papyrus resolves by name directly via
    // GetFormsByTESFullName instead.
    std::string speakerName = ReadQueueValue(sec.c_str(), "speaker_name", "");
    // Server-side id of this line (uid=utt_... from the stream).
    std::string utteranceId = ReadQueueValue(sec.c_str(), "utterance_id", "");

    float duration = 2.0f;
    try {
        duration = std::stof(durStr);
    }
    catch (...) {
        duration = 2.0f;
    }

    // Tell the path hook which WEM to redirect to.
    SetCurrentWemPath(wemPath);

    // Lip generation IS active (unity_wem_gen.py generates a real per-slot
    // .ffxanim for every line) - this call is required for lips to move at
    // all. Confirmed working via direct in-game test earlier this session.
    // (Stale "WEM-only mode" comment removed - it described an old state of
    // the Python pipeline that no longer applies.)
    ActivateLipsForCurrentLine();

    if (!subtitle.empty()) {
        PatchDialogueSubtitle(subtitle);
    }

    // Buffer added to the line's duration before advancing. Covers two timing
    // offsets that otherwise let the NEXT line's lip activation overwrite the
    // shared 00666A79.ffxanim slot before THIS line's audio finishes.
    const int PLAYBACK_BUFFER_MS = g_playbackBufferMs;  // [Tuning] PlaybackBufferMs

    int waitMs = static_cast<int>(duration * 1000.0f) + PLAYBACK_BUFFER_MS;

    REX::INFO("Mantella: Playing line {} (duration={}s) speaker_name={}", idx, duration, speakerName);

    WriteSpeakingState(duration);

    // Tell the server this line was actually delivered. UNITYServer stores
    // every NPC line as delivery_state='pending' and only lets 'spoken' lines
    // into ANY NPC's prompt history and long-term memory (DataEventLog's
    // delivery-visibility filter). CHIM/STOBE acknowledge each line through
    // speech_delivery.php; without this, no NPC ever remembered anything an
    // NPC said - including what it said itself. Fire-and-forget, so playback
    // never waits on the network.
    if (!utteranceId.empty()) {
        AsyncPostToUnity(L"/UNITYServer/speech_delivery.php",
            "{\"utterance_id\":\"" + EscapeJson(utteranceId) + "\",\"delivery_state\":\"spoken\"}");
    }
    // Written per-line, separately from WriteSpeakingState, so Papyrus can
    // retarget MantellaSpeakerAlias to THIS line's actual speaker before
    // starting the scene, instead of staying fixed to whoever the G-key
    // originally targeted for the whole conversation.
    WritePrivateProfileStringA("speaking", "speaker_name", speakerName.c_str(), BRIDGE_INI);
    // FormID alongside the name - reuses ResolveActorFormIdByName (already
    // built for exactly this purpose, declared in UnityChat.h for
    // cross-file use, but never actually called until now). Confirmed via
    // a real in-game rechat test that name-based resolution
    // (GetFormsByTESFullName, still what timer 1 uses on the Papyrus side)
    // silently fails for a second speaker joining an existing conversation
    // - same class of bug the bored-event fix already diagnosed and fixed
    // (names captured via C++'s GetDisplayFullName() aren't guaranteed to
    // match what GetFormsByTESFullName searches against), just never
    // exercised on THIS path before, since chat.php always had exactly one
    // speaker until rechat existed. Empty speaker_name (single-NPC turns,
    // the common case) resolves to 0 here and Papyrus's fallback chain
    // already handles that correctly - falls through to CurrentSpeakerActor
    // exactly as it always has.
    std::uint32_t speakerFormId = speakerName.empty() ? 0 : ResolveActorFormIdByName(speakerName);
    // TEMPORARY DIAGNOSTIC - shows exactly whether ResolveActorFormIdByName
    // matched speakerName against the activated roster, or silently fell
    // through to its own fallback (g_currentSpeakerFormID, i.e. whoever the
    // conversation originally started with via G-key). If speakerFormId ==
    // g_currentSpeakerFormID's current value here despite speakerName being
    // a DIFFERENT NPC, that confirms the roster string-match is failing -
    // worth comparing this logged speakerName character-for-character
    // against the roster dump from "activated roster now has N entries".
    REX::INFO("Mantella: Resolved speaker_formid=0x{:X} for speaker_name='{}' (current g_currentSpeakerFormID=0x{:X})",
              speakerFormId, speakerName, g_currentSpeakerFormID.load());
    WritePrivateProfileStringA("speaking", "speaker_formid",
        FormIdToPapyrusString(speakerFormId).c_str(), BRIDGE_INI);

    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));

    WritePrivateProfileStringA("speaking", "active", "0", BRIDGE_INI);
}


// UNITY conversation loop.
//
// Differs fundamentally from the Mantella pull model: UNITYServer STREAMS every
// line of a turn back at once in response to a single inputtext request. The
// plugin fires that request (SendUnityConversation, which streams and appends
// pending lines to UnityWork.ini), and the Python generator fetches each wav,
// builds the wem, deploys it, and writes the [line_N] into MantellaQueue.ini.
//
// So this loop does NOT request lines. It only:
//   - plays line_{localRead} as write_index grows (Python-driven),
//   - decides a turn is "done" when no new line has appeared for a grace period,
//   - then awaits the next player_input (fires a fresh stream) or end.
//
// The play mechanics (PlayQueuedLine, read_index persistence, speaking flags)
// and the per-turn queue reset are preserved exactly from the Mantella version.
void RunConversationLoop(const std::string& startNonce, std::uint64_t myGeneration)
{
    std::string lastNonce = startNonce;
    int localRead = 0;                 // next line index C++ will play
    bool awaitingPlayerInput = true;   // no auto greeting; first G opens input
    bool awaitPromptWritten = false;

    REX::INFO("UNITY: Conversation loop started (explicit state machine), generation={}", myGeneration);

    auto ResetQueueForNewTurn = []() {
        std::ofstream qf(QUEUE_INI, std::ios::trunc);
        qf << "[queue]\nwrite_index=0\nread_index=0\nturn_done=0\n";
    };

    // Fresh queue for this conversation. Python writes [line_N] and sets
    // [queue] turn_done=1 only after UnityChat appends __ENDTURN__.
    ResetQueueForNewTurn();

    while (g_conversationActive && g_conversationGeneration.load() == myGeneration) {
        // -----------------------------------------------------------------
        // STATE A: Await player input
        // -----------------------------------------------------------------
        // This state is used at conversation start and after each NPC turn is
        // fully played. Do not wait for turn_done here; there is no Unity turn
        // in flight yet. We only move to playback after a real player_input.
        if (awaitingPlayerInput) {
            if (!awaitPromptWritten) {
                g_mantellaSpeaking.store(false);
                WriteAwaitInput(lastNonce);
                awaitPromptWritten = true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));

            // Re-check after the sleep: a newer conversation may have started
            // (or this one ended) while this thread was asleep. Without this,
            // a stale loop would wake up and keep writing to Bridge.ini even
            // though it's no longer the current conversation.
            if (!g_conversationActive || g_conversationGeneration.load() != myGeneration) {
                REX::INFO("UNITY: loop generation {} superseded, exiting", myGeneration);
                return;
            }

            std::string currentNonce = ReadIniValue("request", "nonce", "");
            std::string currentType = ReadIniValue("request", "type", "");
            std::transform(currentType.begin(), currentType.end(), currentType.begin(), ::tolower);

            if (currentNonce.empty() || currentNonce == lastNonce) {
                continue;
            }

            lastNonce = currentNonce;

            if (currentType == "end") {
                REX::INFO("UNITY: Player ended conversation");
                CancelPendingChats();
                g_conversationActive = false;
                g_mantellaSpeaking.store(false);
                return;
            }

            if (currentType == "player_input") {
                std::string playerInput = ReadIniValue("request", "player_input", "");
                std::string speaker = ReadIniValue("request", "speaker", "Unknown");
                REX::INFO("UNITY: Player said: {}", playerInput);

                // Per-message responder (named NPC > crosshair NPC > current).
                std::uint32_t crosshairFormId = 0;
                try {
                    crosshairFormId = static_cast<std::uint32_t>(
                        std::stoll(ReadIniValue("request", "crosshair_refid", "0")));
                } catch (...) {}
                const std::string target = ResolveInputTargetName(playerInput, crosshairFormId, speaker);
                if (target != speaker) {
                    REX::INFO("UNITY: message addressed to '{}' instead of '{}'", target, speaker);
                    speaker = target;
                }

                ResetQueueForNewTurn();
                localRead = 0;

                std::string decoded = playerInput;
                std::replace(decoded.begin(), decoded.end(), '_', ' ');

                awaitingPlayerInput = false;
                awaitPromptWritten = false;

                SendUnityConversation(speaker, decoded, 1);
                continue;
            }

            // Ignore other bridge states while awaiting input.
            continue;
        }

        // -----------------------------------------------------------------
        // STATE B: Play / wait for the current Unity turn
        // -----------------------------------------------------------------
        // UnityChat is streaming lines to UnityWork.ini, Python is converting
        // them to WEMs and writing MantellaQueue.ini, and Python sets turn_done
        // after it sees __ENDTURN__. We play lines as soon as they appear and
        // only return to input once the stream is done AND the queue is drained.
        int writeIndex = ReadQueueWriteIndex();
        if (localRead < writeIndex) {
            g_mantellaSpeaking.store(true);
            PlayQueuedLine(localRead);
            localRead++;
            WritePrivateProfileStringA("queue", "read_index",
                std::to_string(localRead).c_str(), QUEUE_INI);
            g_mantellaSpeaking.store(false);

            // Re-check post-playback too: PlayQueuedLine can block for
            // several real seconds (line duration + buffer). A supersede
            // during that sleep must not let this thread keep going.
            if (!g_conversationActive || g_conversationGeneration.load() != myGeneration) {
                REX::INFO("UNITY: loop generation {} superseded during playback, exiting", myGeneration);
                return;
            }
            continue;
        }

        if (ReadQueueTurnDone()) {
            REX::INFO("UNITY: NPC turn_done reached and queue drained; returning to input state");
            awaitingPlayerInput = true;
            awaitPromptWritten = false;
            continue;
        }

        // No line ready yet, and stream not done. Wait briefly for Python.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REX::INFO("UNITY: loop generation {} exiting normally (active={}, current_gen={})",
              myGeneration, g_conversationActive.load(), g_conversationGeneration.load());
}

// --- Voice recording (press-and-hold STT) -----------------------------
// Ported from CHIM's Voicerec.cpp (VoiceRecordThread/makeSTT), adapted to
// this plugin's architecture:
//   - No Papyrus native-function registration (CHIM calls straight into
//     C++ from OnKeyDown with zero latency; this plugin only has Bridge.ini
//     polling available, so there's up to ~500ms of latency on the START
//     edge - acceptable for a first version, first syllable might
//     occasionally get slightly clipped, not worth the added complexity of
//     native function registration yet).
//   - The STOP edge does NOT have that latency problem: this thread polls
//     raw Windows key state directly via GetAsyncKeyState, exactly like
//     CHIM's own thread does, so release timing is immediate regardless of
//     Bridge.ini polling cadence. The Papyrus-side key-up write to
//     [voice] stop_recording (g_voiceStopRequested below) is a backup
//     signal only.
//   - Key-code assumption: VoiceRecordKey's raw value is passed straight
//     to GetAsyncKeyState with no translation table, unlike CHIM's
//     KeyMap.h. Inferred safe from every other keybind already confirmed
//     working in this plugin (ActivationKey=0x47='G', FreeformTalkKey=
//     0x2E=VK_DELETE, etc. all already match Windows VK codes exactly,
//     unlike Skyrim's DirectInput-based codes CHIM has to translate) - not
//     independently verified against CassiopeiaPapyrusExtender's own
//     source, just consistent with every example so far.
//   - Transcription uploads to UNITYServer's stt.php - real implementation
//     as of this session (ported from the confirmed-working
//     abeiro/HerikaServer reference), previously a not_implemented stub.
//   - Once transcribed, text is handed to Papyrus via [voice]
//     type=transcription_ready rather than C++ writing [request]
//     player_input directly - Papyrus owns all conversation state
//     (ConversationActive/WaitingForInput/IsAccumulating) and already has
//     the logic to decide what to do with a line of text; C++ has no
//     visibility into that state at all. Scoped for v1 to only work when a
//     conversation is already open and waiting for input, mirroring the
//     G-key "send accumulated text" step exactly - voice also auto-
//     starting a fresh conversation (reusing FreeformTalkKey's nearest-
//     activated-NPC logic) is a natural follow-up once this foundation is
//     proven working, deliberately not attempted in this same pass.
static std::atomic<bool> g_voiceRecordingActive{ false };
static std::atomic<bool> g_voiceStopRequested{ false };
static constexpr int kVoiceSilenceThreshold = 500;    // same threshold CHIM uses
static constexpr DWORD kVoiceMaxSilenceMs = 4000;     // stop after 4s continuous silence
static constexpr DWORD kVoiceMaxDurationMs = 60000;   // hard cap, matches CHIM
static constexpr float kVoiceMinDurationSec = 0.5f;   // reject anything shorter
static constexpr size_t kVoiceMinFileBytes = 32000;   // reject anything smaller

// Builds a minimal in-memory WAV file (RIFF/WAVE/fmt/data chunks) from
// captured 16-bit PCM samples - same format CHIM's own VoiceRecordThread
// writes (16kHz, mono, 16-bit), which is also what UNITYServer's
// stt-whisper.php expects to receive.
std::string BuildWavFile(const std::vector<short>& samples, const WAVEFORMATEX& wfx)
{
    std::string out;
    DWORD dataSize = static_cast<DWORD>(samples.size() * sizeof(short));
    DWORD fileSize = dataSize + 36;

    out.append("RIFF", 4);
    out.append(reinterpret_cast<const char*>(&fileSize), 4);
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    DWORD fmtSize = sizeof(wfx);
    out.append(reinterpret_cast<const char*>(&fmtSize), 4);
    out.append(reinterpret_cast<const char*>(&wfx), sizeof(wfx));
    out.append("data", 4);
    out.append(reinterpret_cast<const char*>(&dataSize), 4);
    if (!samples.empty()) {
        out.append(reinterpret_cast<const char*>(samples.data()), dataSize);
    }
    return out;
}

void VoiceRecordThread(int voiceKeyCode)
{
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 16000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.wBitsPerSample * wfx.nChannels / 8;
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;

    HWAVEIN wi = nullptr;
    if (waveInOpen(&wi, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL | WAVE_FORMAT_DIRECT) != MMSYSERR_NOERROR) {
        REX::ERROR("UNITY: voice recording failed to open capture device");
        g_voiceRecordingActive.store(false);
        g_voiceStopRequested.store(false);
        return;
    }

    const int kBufferCount = 8;
    const int kBufferSize = 16000 * 2 / 8; // 4000 bytes = 0.125s of 16-bit mono @ 16kHz (matches CHIM's buffer size)
    const long kBufferMs = static_cast<long>(kBufferSize) * 1000 / static_cast<long>(wfx.nAvgBytesPerSec); // 125
    std::vector<std::vector<char>> buffers(kBufferCount, std::vector<char>(kBufferSize));
    std::vector<WAVEHDR> headers(kBufferCount);
    for (int i = 0; i < kBufferCount; ++i) {
        headers[i] = {};
        headers[i].lpData = buffers[i].data();
        headers[i].dwBufferLength = kBufferSize;
        waveInPrepareHeader(wi, &headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(wi, &headers[i], sizeof(WAVEHDR));
    }

    waveInStart(wi);
    REX::INFO("UNITY: voice recording started (key=0x{:X})", voiceKeyCode);

    // Deliberately does NOT touch g_conversationGeneration or reset
    // QUEUE_INI here, unlike a fresh conversation start. An earlier version
    // did bump generation to "interrupt whatever was playing," copied
    // reflexively from CHIM's pattern without checking whether it actually
    // applied here - it doesn't. v1 voice input only ever fires while
    // WaitingForInput is true (see the [voice] transcription_ready
    // dispatch gate in the .psc), which is precisely the one state where
    // RunConversationLoop has nothing queued and isn't playing anything at
    // all - there is nothing to interrupt. Bumping generation anyway
    // doesn't just skip a no-op interrupt, it actively kills the
    // RunConversationLoop thread itself (its per-iteration generation
    // check sees the mismatch and exits), which is the ONLY thread
    // watching [request] for the player_input write this whole feature
    // depends on. Confirmed via a real in-game log: transcription
    // succeeded correctly, but "loop generation N superseded, exiting"
    // fired the instant recording started, and the transcribed text was
    // never heard because nothing was left running to receive it.

    std::vector<short> audioData;
    DWORD startTick = GetTickCount();
    long silenceMs = -500; // matches CHIM's initial negative bias so a brief
                            // false-positive silence read right at the start
                            // doesn't immediately trip the timeout

    while (true) {
        if (GetTickCount() - startTick > kVoiceMaxDurationMs) {
            REX::INFO("UNITY: voice recording stopped - hard 60s cap reached");
            break;
        }
        if (silenceMs >= (long)kVoiceMaxSilenceMs) {
            REX::INFO("UNITY: voice recording stopped - {} ms of silence", silenceMs);
            break;
        }
        // Primary stop mechanism: direct key-state poll, same as CHIM -
        // immediate, not dependent on Bridge.ini/Papyrus round-trip timing.
        if (!(GetAsyncKeyState(voiceKeyCode) & 0x8000)) {
            REX::INFO("UNITY: voice recording stopped - key released");
            break;
        }
        // Backup: Papyrus-side stop signal, in case the direct key poll
        // above is ever wrong for some environmental reason.
        if (g_voiceStopRequested.load()) {
            REX::INFO("UNITY: voice recording stopped - Papyrus stop signal");
            break;
        }
        if (!g_voiceRecordingActive.load()) {
            break;
        }

        for (auto& h : headers) {
            if (h.dwFlags & WHDR_DONE) {
                short* data = reinterpret_cast<short*>(h.lpData);
                size_t numSamples = h.dwBytesRecorded / sizeof(short);
                bool silentChunk = true;
                for (size_t i = 0; i < numSamples; ++i) {
                    audioData.push_back(data[i]);
                    if (std::abs((int)data[i]) > kVoiceSilenceThreshold) {
                        silentChunk = false;
                    }
                }
                if (silentChunk) {
                    // Was a flat +250 per buffer, but each buffer is 125ms,
                    // so recording cut off after ~2s of silence, not 4s.
                    silenceMs += kBufferMs;
                } else {
                    silenceMs = 0;
                }
                h.dwBytesRecorded = 0;  // consumed - don't re-collect after waveInReset below
                waveInPrepareHeader(wi, &h, sizeof(WAVEHDR));
                waveInAddBuffer(wi, &h, sizeof(WAVEHDR));
            }
        }
        Sleep(10);
    }

    waveInStop(wi);
    // waveInStop leaves unfilled buffers queued; unprepare/close both FAIL
    // while buffers are still queued, which leaked the capture device on
    // every recording. waveInReset returns every buffer (marked done) first.
    // The partially-filled buffer at the moment of release is kept too, so
    // the last word isn't clipped.
    waveInReset(wi);
    for (auto& h : headers) {
        if ((h.dwFlags & WHDR_DONE) && h.dwBytesRecorded > 0) {
            short* data = reinterpret_cast<short*>(h.lpData);
            size_t numSamples = h.dwBytesRecorded / sizeof(short);
            audioData.insert(audioData.end(), data, data + numSamples);
            h.dwBytesRecorded = 0;
        }
        waveInUnprepareHeader(wi, &h, sizeof(WAVEHDR));
    }
    waveInClose(wi);

    g_voiceRecordingActive.store(false);
    g_voiceStopRequested.store(false);

    float durationSec = audioData.empty() ? 0.0f :
        static_cast<float>(audioData.size()) / static_cast<float>(wfx.nSamplesPerSec);
    std::string wavBytes = BuildWavFile(audioData, wfx);

    if (durationSec < kVoiceMinDurationSec || wavBytes.size() < kVoiceMinFileBytes) {
        REX::INFO("UNITY: voice recording discarded - too short (duration={:.2f}s size={} bytes)",
                  durationSec, wavBytes.size());
        return;
    }

    REX::INFO("UNITY: voice recording captured {:.2f}s ({} bytes), uploading for transcription",
              durationSec, wavBytes.size());

    std::string transcribedText = PostAudioForTranscription(wavBytes);
    if (transcribedText.empty()) {
        REX::INFO("UNITY: voice transcription returned empty text - nothing to send");
        return;
    }

    REX::INFO("UNITY: voice transcription: '{}'", transcribedText);

    // Hand off to Papyrus via [voice] - Papyrus owns conversation state and
    // decides what to do with this text; C++ has no visibility into it.
    static std::atomic<int> voiceTranscriptionNonce{ 0 };
    int nonce = voiceTranscriptionNonce.fetch_add(1) + 1;
    WritePrivateProfileStringA("voice", "type", "transcription_ready", BRIDGE_INI);
    WritePrivateProfileStringA("voice", "text", transcribedText.c_str(), BRIDGE_INI);
    WritePrivateProfileStringA("voice", "nonce", std::to_string(nonce).c_str(), BRIDGE_INI);
}

// -----------------------------------------------------------------------
// Game version check + in-game status line.
//
// Most "nothing happens" reports come down to setup, not code: the Python
// watcher isn't running, the server isn't up, or the game version doesn't
// match the hook offsets. The plugin sums that up in Bridge.ini [status]
// "message" and rewrites it only when it changes; Papyrus shows each new
// message once as a notification (and again after every load).
// -----------------------------------------------------------------------
static std::string g_gameVersion = "unknown";
static bool g_gameVersionSupported = true;
static std::string g_hookStatus = "not installed";   // "ok" | "skipped" | "failed"

static std::string ReadGameVersion()
{
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeA(exePath, &ignored);
    if (size == 0) return "unknown";
    std::vector<char> data(size);
    if (!GetFileVersionInfoA(exePath, 0, size, data.data())) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueA(data.data(), "\\", reinterpret_cast<void**>(&info), &len) || !info) return "unknown";
    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
             HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return buf;
}

static bool IsSupportedGameVersion(const std::string& version)
{
    const std::string supported = kSupportedGameVersion;
    return version == supported || version.rfind(supported + ".", 0) == 0;
}

// 0 = not checked yet, 1 = reachable, 2 = unreachable
static std::atomic<int> g_serverState{ 0 };
static std::atomic<bool> g_serverCheckRunning{ false };
static DWORD g_lastServerCheckTick = 0;

static void StartServerCheckIfDue()
{
    DWORD now = GetTickCount();
    bool due = g_serverState.load() == 0 || (now - g_lastServerCheckTick) > 30000;
    if (!due || g_serverCheckRunning.exchange(true)) return;
    g_lastServerCheckTick = now;
    // Own thread: an unreachable host can block for the full HTTP timeout,
    // and the bridge monitor loop must keep polling meanwhile.
    std::thread([] {
        PostToUnityWithResponse(L"/UNITYServer/", "");  // any HTTP response counts as reachable
        g_serverState.store(IsDwemerDistroConnected() ? 1 : 2);
        g_serverCheckRunning.store(false);
    }).detach();
}

// Reads the heartbeat file unity_wem_gen.py rewrites every couple of seconds.
// Returns 0 = running, 1 = not running, 2 = running but reports problems.
static int ReadWatcherState(std::string& problems)
{
    const std::string path = g_bridgeDir + "UnityWatcher.ini";
    char buf[512] = {};
    GetPrivateProfileStringA("watcher", "heartbeat", "", buf, sizeof(buf), path.c_str());
    long long beat = 0;
    try { beat = std::stoll(buf); } catch (...) { beat = 0; }
    long long now = static_cast<long long>(time(nullptr));
    if (beat == 0 || now - beat > 15) return 1;
    char pbuf[512] = {};
    GetPrivateProfileStringA("watcher", "problems", "", pbuf, sizeof(pbuf), path.c_str());
    problems = TrimAscii(pbuf);
    return problems.empty() ? 0 : 2;
}

static std::string g_lastStatusMessage;
static DWORD g_lastStatusTick = 0;

static void UpdateStatus()
{
    DWORD now = GetTickCount();
    if (g_lastStatusTick != 0 && now - g_lastStatusTick < 2000) return;
    g_lastStatusTick = now;

    StartServerCheckIfDue();

    std::vector<std::string> problems;
    if (!g_gameVersionSupported) {
        problems.push_back("Starfield " + g_gameVersion + " is not supported (needs " +
                           kSupportedGameVersion + ") - voices disabled");
    } else if (g_hookStatus != "ok") {
        problems.push_back("voice hooks failed to install - voices disabled");
    }
    const int server = g_serverState.load();
    if (server == 2) {
        problems.push_back("server not reachable at " + GetUnityServerHomeUrl());
    }
    std::string watcherProblems;
    const int watcher = ReadWatcherState(watcherProblems);
    if (watcher == 1) {
        problems.push_back("voice watcher not running (start unity_wem_gen.py)");
    } else if (watcher == 2) {
        problems.push_back("voice watcher: " + watcherProblems);
    }

    std::string message;
    if (!problems.empty()) {
        message = "UNITY: ";
        for (size_t i = 0; i < problems.size(); ++i) {
            if (i) message += "; ";
            message += problems[i];
        }
    } else if (server == 1) {
        message = "UNITY ready";   // only once the first server check has actually passed
    }
    if (message.empty() || message == g_lastStatusMessage) return;
    g_lastStatusMessage = message;

    WritePrivateProfileStringA("status", "plugin_version", kPluginVersion, BRIDGE_INI);
    WritePrivateProfileStringA("status", "game_version", g_gameVersion.c_str(), BRIDGE_INI);
    WritePrivateProfileStringA("status", "hooks", g_hookStatus.c_str(), BRIDGE_INI);
    WritePrivateProfileStringA("status", "server", server == 1 ? "ok" : server == 2 ? "unreachable" : "checking", BRIDGE_INI);
    WritePrivateProfileStringA("status", "watcher", watcher == 0 ? "ok" : watcher == 1 ? "not running" : "problems", BRIDGE_INI);
    WritePrivateProfileStringA("status", "message", message.c_str(), BRIDGE_INI);
    REX::INFO("Mantella: status - {}", message);
}

void BridgeMonitorThread()
{
    // Non-throwing overloads: an unreachable path (e.g. a drive that no
    // longer exists) must log, not crash the game. The Aug 2026 build used
    // the throwing form with a hardcoded E:\ path and crashed on launch once
    // the E: drive was gone.
    std::error_code dirErr;
    std::filesystem::create_directories(BRIDGE_DIR, dirErr);
    if (dirErr) REX::ERROR("Mantella: cannot create bridge folder {} ({})", BRIDGE_DIR, dirErr.message());
    REX::INFO("Mantella: Bridge monitor started");
    REX::INFO("Mantella: Watching {}", BRIDGE_INI);
    std::string lastNonce;
    std::string lastVoiceNonce;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        UpdateStatus();
        MaybeTriggerBoredEvent();

        std::error_code iniErr;
        if (!std::filesystem::exists(BRIDGE_INI, iniErr)) continue;

        // Voice recording start/stop signals from Papyrus's BSInputEvent.
        // Deliberately checked BEFORE the [request] nonce-dedup below (and
        // not nested inside it) - that block's own "continue" on no-new-
        // request would otherwise skip this check on every iteration where
        // nothing else happened to change, which is most of them. Own
        // section, own nonce counter - same reasoning as [bored]/
        // [freeform] having their own sections: keeps this channel from
        // ever colliding with the request/response dedupe loop.
        std::string voiceNonce = ReadIniValue("voice", "nonce", "");
        if (!voiceNonce.empty() && voiceNonce != lastVoiceNonce) {
            std::string voiceType = ReadIniValue("voice", "type", "");
            if (voiceType == "start_recording") {
                lastVoiceNonce = voiceNonce;
                if (!g_voiceRecordingActive.load()) {
                    // Read the actual configured key from Papyrus rather
                    // than hardcoding VoiceRecordKey's default here - C++
                    // has no other visibility into that property's value,
                    // and hardcoding it would silently poll the wrong key
                    // if it's ever reconfigured.
                    std::string keyStr = ReadIniValue("voice", "keycode", "86");
                    int voiceKeyCode = 0x56; // VK_V fallback if parse fails - matches
                                              // Papyrus's VoiceRecordKey default (see
                                              // that property's comment: switched from
                                              // Numpad Plus after an in-game diagnostic
                                              // revealed a scan-code collision with ALT)
                    try { voiceKeyCode = std::stoi(keyStr); } catch (...) {}
                    g_voiceRecordingActive.store(true);
                    g_voiceStopRequested.store(false);
                    std::thread(VoiceRecordThread, voiceKeyCode).detach();
                    REX::INFO("UNITY: voice recording thread spawned (key=0x{:X})", voiceKeyCode);
                }
            } else if (voiceType == "stop_recording") {
                lastVoiceNonce = voiceNonce;
                g_voiceStopRequested.store(true);
            }
        }

        std::string nonce = ReadIniValue("request", "nonce", "");
        if (nonce.empty() || nonce == lastNonce) continue;
        lastNonce = nonce;
        std::string type = ReadIniValue("request", "type", "");
        std::string speaker = ReadIniValue("request", "speaker", "Unknown");
        std::transform(type.begin(), type.end(), type.begin(), ::tolower);
        REX::INFO("Mantella: Bridge request detected. type={}, speaker={}, nonce={}", type, speaker, nonce);
        std::string refidStr = ReadIniValue("request", "speaker_refid", "0");
        try {
            g_currentSpeakerFormID.store(static_cast<std::uint32_t>(std::stoul(refidStr)));
            REX::INFO("Mantella: Speaker form ID set to 0x{:X}", g_currentSpeakerFormID.load());
        } catch (...) { g_currentSpeakerFormID.store(0); }

        if (type == "start" && !g_conversationActive) {
            g_conversationActive = true;
            // Bump generation BEFORE spawning the new loop. Any previous
            // loop thread still alive (e.g. mid-sleep, not yet noticed
            // g_conversationActive changing) will see a mismatched
            // generation on its very next check and self-terminate,
            // instead of potentially racing the new loop for writes to
            // Bridge.ini.
            std::uint64_t myGeneration = g_conversationGeneration.fetch_add(1) + 1;
            // Stop any bored-event/rechat chain still generating lines, so
            // they can't land in this conversation's queue.
            CancelPendingChats();
            REX::INFO("UNITY: Conversation start, speaker={}, generation={}", speaker, myGeneration);
            // No Mantella start handshake in UNITY. Spawn the play-and-await loop
            // first (so it's ready to drain whatever the greeting stream enqueues),
            // then fire an initial stream to get the NPC's greeting. A greeting is
            // just an inputtext turn with no real player line; the server generates
            // an opener from the NPC profile/context.
            std::thread(RunConversationLoop, nonce, myGeneration).detach();
        }
        else if (type == "end") {
            CancelPendingChats();
            g_conversationActive = false;
            g_mantellaSpeaking.store(false);
            // Also bump generation on end: if a loop thread is currently
            // asleep and hasn't yet noticed g_conversationActive flip to
            // false, this ensures it still gets invalidated even if a new
            // "start" hasn't happened yet.
            g_conversationGeneration.fetch_add(1);
        }
        else if (type == "reinit") {
            // Sent by TESLoadGameEvent on every save load. This is the
            // recovery path for a conversation state that got stuck true
            // without a clean "end" ever arriving (e.g. Escape opening the
            // vanilla PauseMenu before BSInputEvent's EndKey branch runs -
            // confirmed via a real stuck-session test). Same reset as
            // "end", since from C++'s perspective a reinit and a
            // conversation-end are equivalent: whatever conversation state
            // existed before this point is no longer valid.
            bool wasStuck = g_conversationActive.load();
            CancelPendingChats();
            g_conversationActive = false;
            g_mantellaSpeaking.store(false);
            g_conversationGeneration.fetch_add(1);
            REX::INFO("UNITY: reinit received (was_stuck_active={})", wasStuck);
        }
        else if (type == "toggle_activate") {
            // Synchronous - this is a local roster mutation, no network/LLM
            // call involved, so it can resolve and write its result back to
            // Bridge.ini before this loop iteration even finishes. Papyrus
            // reads the result on its next poll tick and shows an accurate
            // notification, rather than a blind "toggling..." message.
            RE::Actor* actor = nullptr;
            std::uint32_t formId = g_currentSpeakerFormID.load();
            if (formId != 0) {
                actor = RE::TESForm::LookupByID<RE::Actor>(formId);
            }
            std::string reason;
            ToggleActivationResult result = ToggleNpcActivation(actor, reason);
            std::string statusStr;
            switch (result) {
                case ToggleActivationResult::NowActive:   statusStr = "active";   break;
                case ToggleActivationResult::NowInactive: statusStr = "inactive"; break;
                case ToggleActivationResult::Rejected:    statusStr = "rejected"; break;
            }
            WritePrivateProfileStringA("activation", "status", statusStr.c_str(), BRIDGE_INI);
            WritePrivateProfileStringA("activation", "name", reason.c_str(), BRIDGE_INI);
            REX::INFO("UNITY: toggle_activate result={} detail={}", statusStr, reason);

            // Dump the full roster after every toggle - this is what actually
            // proves multiple NPCs coexist correctly (vs. one silently
            // overwriting another), which a single-toggle result= line alone
            // does not demonstrate.
            {
                std::lock_guard<std::mutex> lock(g_activatedNpcsMutex);
                std::string rosterDump;
                for (const auto& npc : g_activatedNpcs) {
                    if (!rosterDump.empty()) rosterDump += ", ";
                    rosterDump += npc.name + " (0x" +
                        [](std::uint32_t id) {
                            char buf[16];
                            snprintf(buf, sizeof(buf), "%X", id);
                            return std::string(buf);
                        }(npc.formId) + ")";
                }
                REX::INFO("UNITY: activated roster now has {} entries: [{}]",
                          g_activatedNpcs.size(), rosterDump.empty() ? "none" : rosterDump);
            }

            WriteFreeformRoster();
        }
    }
}

void InstallVoiceHook()
{
    REX::INFO("Mantella: InstallVoiceHook called");
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("Starfield.exe"));
    REX::INFO("Mantella: Base address: 0x{:X}", base);
    if (!base) { REX::ERROR("Mantella: Failed to get base address"); g_hookStatus = "failed"; return; }
    int hooksInstalled = 0;
    g_baseAddr = base;

    SFSE::AllocTrampoline(512);
    auto& trampoline = SFSE::GetTrampoline();

    // -----------------------------------------------------------------------
    // Path hook at 0xF1C8B1 — detects when Mantella wem path is being resolved
    // -----------------------------------------------------------------------
    {
        static const std::uintptr_t PATH_CALLSITE = 0xF1BFB1;
        auto addr = base + PATH_CALLSITE;
        auto firstByte = *reinterpret_cast<std::uint8_t*>(addr);
        REX::INFO("Mantella: Path callsite 0x{:X} first byte: 0x{:02X}", addr, firstByte);
        if (firstByte == 0xE8) {
            g_origPathBuild = reinterpret_cast<PathBuildFunc_t>(
                trampoline.write_call<5>(addr, &HookedPathBuild));
            REX::INFO("Mantella: Path hook installed at 0x{:X}", addr);
            hooksInstalled++;
        } else {
            REX::ERROR("Mantella: Path hook wrong byte 0x{:02X}", firstByte);
        }
    }

    // -----------------------------------------------------------------------
    // Cookie hook at 0xF1C9A6 — intercepts External_Source hash computation
    // Returns unique counter to bust Wwise external source cache
    // -----------------------------------------------------------------------
    {
        static const std::uintptr_t COOKIE_CALLSITE = 0xF1C0A6;
        auto addr = base + COOKIE_CALLSITE;
        auto firstByte = *reinterpret_cast<std::uint8_t*>(addr);
        REX::INFO("Mantella: Cookie callsite 0x{:X} first byte: 0x{:02X}", addr, firstByte);
        if (firstByte == 0xE8) {
            g_origHashFunc = reinterpret_cast<HashFunc_t>(
                trampoline.write_call<5>(addr, &HookedHashFunc));
            REX::INFO("Mantella: Cookie hook installed at 0x{:X}", addr);
            hooksInstalled++;
        } else {
            REX::ERROR("Mantella: Cookie hook wrong byte 0x{:02X}", firstByte);
        }
    }
    g_hookStatus = (hooksInstalled == 2) ? "ok" : "failed";
}

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    LoadPathConfig();
    LoadRuntimeConfig();
    g_taskInterface = SFSE::GetTaskInterface();
    if (g_taskInterface) REX::INFO("Mantella: Task interface acquired");
    else REX::ERROR("Mantella: Failed to get task interface");
    REX::INFO("Mantella Starfield bridge plugin loaded (UNITY {})", kPluginVersion);

    // The hook offsets are only valid for one game build. On any other build,
    // skip them rather than patching random code, and say so in game.
    g_gameVersion = ReadGameVersion();
    const bool skipVersionCheck = ReadConfigValue("Compat", "SkipVersionCheck") == "1";
    g_gameVersionSupported = skipVersionCheck || g_gameVersion == "unknown" || IsSupportedGameVersion(g_gameVersion);
    REX::INFO("Mantella: game version {} (supported: {}{}{})", g_gameVersion, kSupportedGameVersion,
              skipVersionCheck ? ", check skipped by config" : "",
              g_gameVersion == "unknown" ? ", could not read version - assuming supported" : "");
    if (g_gameVersionSupported) {
        InstallVoiceHook();
    } else {
        g_hookStatus = "skipped";
        REX::ERROR("Mantella: game version {} is not {} - voice hooks NOT installed", g_gameVersion, kSupportedGameVersion);
    }
    std::thread(BridgeMonitorThread).detach();

    return true;
}