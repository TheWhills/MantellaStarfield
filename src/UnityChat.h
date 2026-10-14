#pragma once
#include <cstdint>
#include <string>

// -----------------------------------------------------------------------------
// UnityChat - conversation transport for the UNITY Starfield plugin.
//
// Bridges the UnityComm streaming layer to the existing MantellaQueue.ini
// playback queue. A single inputtext request to stream.php returns multiple
// NPC lines (actor|action|subtitle|uid=...|tts=...); each streamed line is
// written as the next [line_N] section so the existing PlayQueuedLine loop
// drains it.
// -----------------------------------------------------------------------------

// Fire a conversation turn: reset the queue, stream inputtext to the server, and
// let the stream callback fill MantellaQueue.ini with [line_N] sections as lines
// arrive. Non-blocking: spawns a detached thread. playerText is the raw player
// line; npcProfile is the target NPC name (the ?profile= param).
void SendUnityConversation(const std::string& npcProfile,
                           const std::string& playerText,
                           long long gamets);

// Fire a bored event for npcProfile: same transport as SendUnityConversation
// but eventType="bored" and no player text. Called from main.cpp's
// MaybeTriggerBoredEvent once it has picked an eligible NPC via
// GetLeastBoredNearbyNpc. Non-blocking: spawns a detached thread.
void SendUnityBoredEvent(const std::string& npcProfile, long long gamets);

// Invalidates every in-flight conversation/bored/rechat chain. Any stream
// still running stops queuing lines, and no further rechat hops fire from
// it. Called from main.cpp on conversation end / reinit. SendUnityConversation
// and SendUnityBoredEvent also call it themselves, so a new player turn or
// bored event always supersedes an older chain instead of interleaving with it.
void CancelPendingChats();

// Estimate a subtitle display duration (seconds) from text length, used when a
// line has no TTS audio to derive real duration from. Exposed for reuse/tuning.
float EstimateSubtitleDurationSeconds(const std::string& subtitle);

// Defined in main.cpp, where g_activatedNpcs lives. Serializes the activated-
// NPC roster into the ?people= JSON array SendUnityConversation attaches to
// its request, per the reverse-engineered extractParticipantIdentityToken()
// contract (see main.cpp for format notes / open StorageID question).
std::string BuildActivatedRosterPeopleParam();

// Defined in main.cpp, where g_activatedNpcs lives. Resolves a streamed
// line's actor name to a FormID via roster lookup, falling back to
// g_currentSpeakerFormID if no roster match - correct-by-construction for
// ordinary single-NPC turns. NOTE: this function already existed with a
// comment claiming it was "declared in UnityChat.h", but that declaration
// was never actually added - this is that fix.
std::uint32_t ResolveActorFormIdByName(const std::string& name);

// Defined in main.cpp, where g_activatedNpcs lives. Unlike
// ResolveActorFormIdByName, this does NOT fall back to
// g_currentSpeakerFormID - callers need a genuine "no match" signal (empty
// string), not a best-effort guess. Returns the roster's own stored name
// (exact casing) if candidateName case-insensitively matches an activated
// NPC OTHER than excludeName, or "" otherwise. Used to detect who a line
// is actually addressing by parsing its opening "<Name>, " against the
// roster, rather than trusting a hardcoded default.
std::string FindActivatedNpcNameExcluding(const std::string& candidateName,
                                          const std::string& excludeName);

// Defined in main.cpp, where LoadPathConfig() resolves it once at plugin
// load. Returns the resolved bridge directory (trailing backslash included)
// so every file in this pipeline - Bridge.ini, MantellaQueue.ini,
// UnityWork.ini - lands in the same user-configured or default location
// instead of each having its own separately hardcoded copy of the path.
std::string GetBridgeDir();