#pragma once
#include <string>

// -----------------------------------------------------------------------------
// UnityChat - Stage 2 conversation transport for the UNITY Starfield plugin.
//
// Bridges the UnityComm streaming layer to the existing MantellaQueue.ini
// playback queue. A single inputtext request to stream.php returns multiple
// NPC lines (actor|action|subtitle|uid=...); each streamed line is written as
// the next [line_N] section so the existing PlayQueuedLine loop drains it.
//
// STAGE 2 IS SUBTITLE-ONLY: TTS is not configured yet, so streamed lines carry
// no tts= hash and no wem_path. PlayQueuedLine's new empty-wem branch shows the
// subtitle for a computed duration and skips audio/lips. When TTS is wired
// later, the parser fills wem_path and the audio path re-engages unchanged.
// -----------------------------------------------------------------------------

// Fire a conversation turn: reset the queue, stream inputtext to the server, and
// let the stream callback fill MantellaQueue.ini with [line_N] sections as lines
// arrive. Non-blocking: spawns a detached thread. playerText is the raw player
// line; npcProfile is the target NPC name (the ?profile= param).
void SendUnityConversation(const std::string& npcProfile,
                           const std::string& playerText,
                           long long gamets);

// Estimate a subtitle display duration (seconds) from text length, used when a
// line has no TTS audio to derive real duration from. Exposed for reuse/tuning.
float EstimateSubtitleDurationSeconds(const std::string& subtitle);
