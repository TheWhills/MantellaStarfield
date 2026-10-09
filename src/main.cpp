#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winhttp.h>
#include <mmsystem.h>

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

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "winmm.lib")

static const char* BRIDGE_DIR = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\";
static const char* BRIDGE_INI = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\Bridge.ini";
// Queue file — Python is the SOLE writer (write_index + [line_N] sections);
// C++ is the sole reader. Separate file from Bridge.ini so the two processes
// never write the same file (no read-modify-write race).
static const char* QUEUE_INI = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\MantellaQueue.ini";

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
            std::to_string(g_activatedNpcs[i].formId).c_str(), BRIDGE_INI);
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
static constexpr DWORD kBoredEventIntervalMs = 60 * 1000;

// Per-NPC cooldown - the same activated NPC can't fire back-to-back bored
// events even if the global interval above has elapsed and they're still
// "least bored" (e.g. only one NPC is currently activated). Independent of
// the probability-gate change above - this still serves its own purpose
// (variety across multiple activated NPCs), not touched by this change.
static constexpr DWORD kPerNpcBoredCooldownMs = 2 * 60 * 1000; // 2 minutes

// Search radius in game units. Ported from CHIM's DISTANCE_ACTIVATING_NPC_IN/OUT
// at the same relative indoor/outdoor ratio, NOT measured against actual
// Starfield distances - these are placeholders, almost certainly wrong as
// absolute values. Tune once you can observe trigger behavior in-game.
static constexpr float kBoredEventRadiusInterior = 2000.0f;
static constexpr float kBoredEventRadiusExterior = 4000.0f;

static std::atomic<bool> g_boredEventsEnabled{ true };

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
    WritePrivateProfileStringA("bored", "speaker_formid", std::to_string(candidate->formId).c_str(), BRIDGE_INI);
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

// Current subtitle text — updated from bridge INI before each line
static std::string g_currentSubtitleText;
static std::mutex g_subtitleMutex;

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

std::string GetReplyType(const std::string& jsonResponse)
{
    std::string key = "\"mantella_reply_type\":\"";
    size_t pos = jsonResponse.find(key);
    if (pos == std::string::npos) return "";
    pos += key.length();
    size_t end = jsonResponse.find("\"", pos);
    if (end == std::string::npos) return "";
    return jsonResponse.substr(pos, end - pos);
}

std::string SendToMantella(const std::string& jsonBody)
{
    HINTERNET hSession = WinHttpOpen(L"MantellaStarfield/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { REX::ERROR("WinHttp: Failed to open session"); return ""; }
    HINTERNET hConnect = WinHttpConnect(hSession, L"localhost", 4999, 0);
    if (!hConnect) { REX::ERROR("WinHttp: Failed to connect"); WinHttpCloseHandle(hSession); return ""; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/mantella", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        REX::ERROR("WinHttp: Failed to open request");
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return "";
    }
    std::wstring headers = L"Content-Type: application/json";
    std::string body = jsonBody;
    BOOL result = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1),
        (LPVOID)body.c_str(), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    std::string response;
    if (result) {
        WinHttpReceiveResponse(hRequest, nullptr);
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buffer(bytesAvailable + 1, 0);
            DWORD bytesRead = 0;
            WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead);
            response.append(buffer.data(), bytesRead);
        }
        REX::INFO("Mantella: Request sent, reply type: {}", GetReplyType(response));
    } else {
        REX::ERROR("Mantella: Failed to send request");
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

std::string BuildStartJson(const std::string& speaker)
{
    std::string safeSpeaker = EscapeJson(speaker.empty() ? "Unknown" : speaker);
    std::string location = ReadIniValue("request", "location", "Unknown Location");
    std::string hourStr = ReadIniValue("request", "hour", "12");
    std::string gamedaysStr = ReadIniValue("request", "gamedays", "1.0");
    if (location.empty()) location = "Unknown Location";
    int hour = 12; float gamedays = 1.0f;
    try { hour = std::stoi(hourStr); } catch (...) {}
    try { gamedays = std::stof(gamedaysStr); } catch (...) {}
    std::string safeLocation = EscapeJson(location);
    std::ostringstream json;
    json << "{"
        << "\"mantella_request_type\":\"mantella_start_conversation\","
        << "\"mantella_worldid\":\"starfield\","
        << "\"mantella_actors\":["
        << "{\"mantella_actor_baseid\":0,\"mantella_actor_refid\":0,\"mantella_actor_name\":\"" << safeSpeaker << "\","
        << "\"mantella_actor_gender\":0,\"mantella_actor_race\":\"HumanRace\",\"mantella_actor_voicetype\":\"Unknown\","
        << "\"mantella_actor_is_in_combat\":false,\"mantella_actor_is_enemy\":false,\"mantella_actor_relationshiprank\":0,\"mantella_actor_is_player\":false},"
        << "{\"mantella_actor_baseid\":1,\"mantella_actor_refid\":1,\"mantella_actor_name\":\"Player\","
        << "\"mantella_actor_gender\":0,\"mantella_actor_race\":\"HumanRace\",\"mantella_actor_voicetype\":\"Unknown\","
        << "\"mantella_actor_is_in_combat\":false,\"mantella_actor_is_enemy\":false,\"mantella_actor_relationshiprank\":4,\"mantella_actor_is_player\":true}"
        << "],"
        << "\"mantella_context\":{\"mantella_location\":\"" << safeLocation << "\","
        << "\"mantella_time\":" << hour << ",\"mantella_gamedays\":" << gamedays << ",\"mantella_weather\":{}}"
        << "}";
    return json.str();
}

std::string BuildContinueJson()
{
    return "{\"mantella_request_type\":\"mantella_continue_conversation\",\"mantella_topicinfofile\":1,\"mantella_context\":{}}";
}

std::string BuildPlayerInputJson(const std::string& playerInput)
{
    std::string processed = playerInput;
    std::replace(processed.begin(), processed.end(), '_', ' ');
    std::string safeInput = EscapeJson(processed);
    std::ostringstream json;
    json << "{\"mantella_request_type\":\"mantella_player_input\","
        << "\"mantella_player_input\":\"" << safeInput << "\","
        << "\"mantella_context\":{}}";
    return json.str();
}

std::string BuildEndJson()
{
    return "{\"mantella_request_type\":\"mantella_end_conversation\"}";
}

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


static const char* MO2_VOICE_BASE_CPP =
    "E:\\Star Wars Genesis\\Game\\mods\\MantellaStarfield\\Sound\\Voice\\MantellaStarfield.esp";
static const char* DATA_VOICE_BASE_CPP =
    "E:\\SteamLibrary\\steamapps\\common\\Starfield\\Data\\Sound\\Voice\\MantellaStarfield.esp";

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

    // 2) All generic + registered folders under both MO2 and Data bases.
    for (const auto& base : { MO2_VOICE_BASE_CPP, DATA_VOICE_BASE_CPP }) {
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
    static const int PLAYBACK_BUFFER_MS = 700;

    int waitMs = static_cast<int>(duration * 1000.0f) + PLAYBACK_BUFFER_MS;

    REX::INFO("Mantella: Playing line {} (duration={}s) speaker_name={}", idx, duration, speakerName);

    WriteSpeakingState(duration);
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
        std::to_string(speakerFormId).c_str(), BRIDGE_INI);

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
                g_conversationActive = false;
                g_mantellaSpeaking.store(false);
                return;
            }

            if (currentType == "player_input") {
                std::string playerInput = ReadIniValue("request", "player_input", "");
                std::string speaker = ReadIniValue("request", "speaker", "Unknown");
                REX::INFO("UNITY: Player said: {}", playerInput);

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
    if (waveInOpen(&wi, WAVE_MAPPER, &wfx, NULL, NULL, CALLBACK_NULL | WAVE_FORMAT_DIRECT) != MMSYSERR_NOERROR) {
        REX::ERROR("UNITY: voice recording failed to open capture device");
        g_voiceRecordingActive.store(false);
        g_voiceStopRequested.store(false);
        return;
    }

    const int kBufferCount = 8;
    const int kBufferSize = 16000 * 2 / 8; // 0.5s of 16-bit mono @ 16kHz, matches CHIM
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
                    silenceMs += 250;
                } else {
                    silenceMs = 0;
                }
                waveInPrepareHeader(wi, &h, sizeof(WAVEHDR));
                waveInAddBuffer(wi, &h, sizeof(WAVEHDR));
            }
        }
        Sleep(10);
    }

    waveInStop(wi);
    for (auto& h : headers) {
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

void BridgeMonitorThread()
{
    std::filesystem::create_directories(BRIDGE_DIR);
    REX::INFO("Mantella: Bridge monitor started");
    REX::INFO("Mantella: Watching {}", BRIDGE_INI);
    std::string lastNonce;
    std::string lastVoiceNonce;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        MaybeTriggerBoredEvent();

        if (!std::filesystem::exists(BRIDGE_INI)) continue;

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
            REX::INFO("UNITY: Conversation start, speaker={}, generation={}", speaker, myGeneration);
            // No Mantella start handshake in UNITY. Spawn the play-and-await loop
            // first (so it's ready to drain whatever the greeting stream enqueues),
            // then fire an initial stream to get the NPC's greeting. A greeting is
            // just an inputtext turn with no real player line; the server generates
            // an opener from the NPC profile/context.
            std::thread(RunConversationLoop, nonce, myGeneration).detach();
        }
        else if (type == "end") {
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
    if (!base) { REX::ERROR("Mantella: Failed to get base address"); return; }
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
        } else {
            REX::ERROR("Mantella: Cookie hook wrong byte 0x{:02X}", firstByte);
        }
    }
}

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    g_taskInterface = SFSE::GetTaskInterface();
    if (g_taskInterface) REX::INFO("Mantella: Task interface acquired");
    else REX::ERROR("Mantella: Failed to get task interface");
    REX::INFO("Mantella Starfield bridge plugin loaded");
    InstallVoiceHook();
    std::thread(BridgeMonitorThread).detach();

    return true;
}