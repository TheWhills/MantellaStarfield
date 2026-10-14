// -----------------------------------------------------------------------------
// UnityChat.cpp - conversation streaming (audio via Python generator).
//
// Streams an inputtext turn from UNITYServer. For each returned line that has a
// tts= hash, appends "hash|voice_folder|speaker_name|subtitle" to
// UnityWork.ini. A separate Python watcher (unity_wem_gen.py) fetches
// soundcache/<hash>.wav, builds the wem via WwiseConsole, deploys it into the
// MantellaStarfield.esp voice folder, and writes the [line_N] section into
// MantellaQueue.ini (including speaker_name, so Papyrus can retarget the
// scene alias to whichever NPC actually speaks each line, via
// CassiopeiaPapyrusExtender.GetFormsByTESFullName - there is no Int
// FormID->Form reverse-lookup native function available, so name-based
// resolution is used instead of a FormID round-trip). The existing C++
// PlayQueuedLine then drains MantellaQueue.ini unchanged.
//
// Division of labor:
//   plugin  -> writes UnityWork.ini      (pending TTS lines)
//   python  -> writes MantellaQueue.ini  (built + deployed wem per line)
//   plugin  -> reads  MantellaQueue.ini  (PlayQueuedLine, existing)
//
// Line format from the server (confirmed live):
//   actor|action|subtitle|uid=utt_xxxx|tts=<hash>
//   e.g. TestNPC|ScriptQueue|Yeah, I hear you.|uid=utt_fce4...|tts=4bbd6111...
// -----------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "UnityChat.h"
#include "UnityComm.h"

#include <windows.h>
#undef ERROR
#include <RE/Starfield.h>   // REX::INFO / REX::ERROR

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>

// Work file the Python generator watches. Plugin APPENDS pending TTS lines here;
// Python consumes them, builds wems, and writes the playback queue below.
// Built from GetBridgeDir() (resolved once in main.cpp's LoadPathConfig) so
// this lands in the same folder as Bridge.ini/MantellaQueue.ini rather than
// carrying its own separately-hardcoded copy of the path that could drift
// out of sync with the other two.
static std::string UnityWorkIniPath()
{
    return GetBridgeDir() + "UnityWork.ini";
}

// Per-NPC voice folder mapping. Mirrors Starfield.py's NPC_VOICE_TYPE_MAP so
// the two sides of the pipeline agree on which folder each named NPC's
// wem/ffxanim belong in. Previously every line was hardcoded to
// "NPCFSarahMorgan" regardless of speaker - harmless for audio (the Wwise
// path hook redirects unconditionally to whatever wem is current) but wrong
// for lip sync, since is_male_voice_folder() on the Python side keys off
// this exact folder name to pick the male/female FaceFX actor and analysis
// file. Every male NPC was silently being compiled through
// StarfieldHumanFemale.facefx as a result. Confirmed fixed and working
// in-game.
namespace {

// Chain epoch. Every chain (a player turn, a bored event, and all rechat hops
// that follow from either) carries the epoch that was current when it began.
// CancelPendingChats() bumps it, so any chain from before that point stops
// queuing lines and stops firing follow-up hops. Without this, a rechat chain
// kept running after Esc, or after the player sent a new line - appending the
// OLD chain's lines (and its __ENDTURN__ marker) into the NEW turn's work file.
std::atomic<std::uint64_t> g_chatEpoch{ 1 };

bool IsEpochCurrent(std::uint64_t epoch) { return epoch == g_chatEpoch.load(); }

const std::map<std::string, std::string>& NpcVoiceTypeMap() {
    static const std::map<std::string, std::string> m = {
        {"Sarah Morgan",     "NPCFSarahMorgan"},
        {"Noel",             "NPCFNoel"},
        {"Walter Stroud",    "NPCMWalterStroud"},
        {"Sam Coe",          "NPCMSamCoe"},
        {"Andreja",          "NPCFAndreja"},
        {"Barrett",          "NPCMBarrett"},
        {"Vasco",            "RobotModelAVasco"},
        {"Matteo Khatri",    "NPCMMatteoKhatri"},
        {"Cora Coe",         "NPCFCoraCoe"},
        {"Vladimir Sall",    "NPCMVladimirSall"},
        {"Heller",           "NPCMHeller"},
        {"Lin",              "NPCFLin"},
        {"Grand Inquisitor", "NPCMGrandInquisitor"},
        {"Mara Jade",        "NPCFMuria"},
        {"Muria Siarkiewicz","NPCFMuria"},
        {"Luke Skywalker",   "NPCMLukeSkywalker"},
        {"Shanni Adasca",    "NPCFAndreja"},
        {"Starkiller",       "NPCMGalenMarek"}, // Galen Marek - key must match speaker_name exactly (confirmed "Starkiller" from the actual server response, not "Galen Marek"). Value is a UNIQUE dedicated folder, same pattern as Grand Inquisitor's own entry - deliberately NOT "GenericMaleRough" itself, even though that's his real in-game voice type, since that folder is already written to unconditionally by ActivateLipsForCurrentLine's blanket generic-folder deployment for every line regardless of speaker - using it as his dedicated audio-routing slot too would collide with that. His real voice type (GenericMaleRough) doesn't need to appear in this map at all - the blanket ffxanim deployment already covers it separately, for lipsync specifically, same as it does for Grand Inquisitor.
    };
    return m;
}

// Fallback for any NPC not in the map above (e.g. generic/unnamed NPCs).
// Sarah's folder is used as the default since it was the original hardcode
// and is guaranteed to have preloaded placeholder wem/ffxanim slots.
static const char* DEFAULT_VOICE_FOLDER = "NPCFSarahMorgan";

std::string ResolveVoiceFolder(const std::string& actorName) {
    const auto& m = NpcVoiceTypeMap();
    auto it = m.find(actorName);
    if (it != m.end()) return it->second;
    REX::INFO("UnityChat: no voice folder mapping for actor '{}', using default '{}'",
              actorName, DEFAULT_VOICE_FOLDER);
    return DEFAULT_VOICE_FOLDER;
}

std::string TrimCopy(const std::string& v) {
  size_t s = v.find_first_not_of(" \t\r\n");
  if (s == std::string::npos) return "";
  size_t e = v.find_last_not_of(" \t\r\n");
  return v.substr(s, e - s + 1);
}

// Parsed representation of one streamed server line.
struct ParsedLine {
  std::string actor;
  std::string action;    // e.g. "ScriptQueue"
  std::string subtitle;  // the spoken text
  std::string uid;       // utterance id (uid=...)
  std::string ttsHash;   // Cartesia audio hash (tts=...)
  int ttsDurationMs = 0; // ttsd= if present
};

// Split "actor|action|subtitle|uid=..|tts=..|[ttsd=..]" into fields.
// Fields 0/1/2 are positional; anything after is a keyed meta token.
bool ParseServerLine(const std::string& raw, ParsedLine& out) {
  std::string line = TrimCopy(raw);
  if (line.empty()) return false;

  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t bar = line.find('|', start);
    if (bar == std::string::npos) {
      parts.push_back(line.substr(start));
      break;
    }
    parts.push_back(line.substr(start, bar - start));
    start = bar + 1;
  }

  if (parts.size() < 3) return false;

  out.actor = TrimCopy(parts[0]);
  out.action = TrimCopy(parts[1]);
  out.subtitle = TrimCopy(parts[2]);

  for (size_t i = 3; i < parts.size(); ++i) {
    std::string tok = TrimCopy(parts[i]);
    if (tok.rfind("uid=", 0) == 0) {
      out.uid = tok.substr(4);
    } else if (tok.rfind("tts=", 0) == 0) {
      out.ttsHash = tok.substr(4);
    } else if (tok.rfind("ttsd=", 0) == 0) {
      out.ttsDurationMs = atoi(tok.substr(5).c_str());
    }
  }

  if (out.subtitle.empty()) return false;
  return true;
}

// Truncate the work file at the start of a turn so the Python generator only
// sees this conversation's lines.
void ResetWorkFile() {
  FILE* f = nullptr;
  std::string workIniPath = UnityWorkIniPath();
  fopen_s(&f, workIniPath.c_str(), "w");
  if (f) {
    fclose(f);
  } else {
    REX::ERROR("UnityChat: failed to reset work file");
  }
}

// Append one pending line for the Python generator:
// hash|voice_folder|speaker_name|subtitle
//
// speaker_name is just pl.actor - no resolution needed on the C++ side at
// all, since Papyrus resolves by name directly via
// CassiopeiaPapyrusExtender.GetFormsByTESFullName (there is no Int
// FormID->Form reverse lookup available). Placed BEFORE subtitle (not
// after) so subtitle stays the final, catch-all field - unity_wem_gen.py's
// parser uses a bounded split that treats the last field as "everything
// remaining," which is what already made it tolerant of a subtitle that
// happens to contain a literal '|' character. Appending speaker_name
// after subtitle instead would have broken that tolerance.
void AppendWorkLine(const ParsedLine& pl) {
  FILE* f = nullptr;
  std::string workIniPath = UnityWorkIniPath();
  fopen_s(&f, workIniPath.c_str(), "a");
  if (!f) {
    REX::ERROR("UnityChat: could not open work file for append");
    return;
  }
  // Newlines in the subtitle would corrupt the line-based work file; strip them.
  std::string safeSub = pl.subtitle;
  std::replace(safeSub.begin(), safeSub.end(), '\n', ' ');
  std::replace(safeSub.begin(), safeSub.end(), '\r', ' ');
  std::string voiceFolder = ResolveVoiceFolder(pl.actor);
  // Speaker name may itself contain '|' in a pathological case (unlikely
  // for real NPC names) - strip it defensively since it's a field delimiter
  // here, same reasoning as the subtitle newline stripping above.
  std::string safeSpeakerName = pl.actor;
  std::replace(safeSpeakerName.begin(), safeSpeakerName.end(), '|', ' ');
  fprintf(f, "%s|%s|%s|%s\n", pl.ttsHash.c_str(), voiceFolder.c_str(),
          safeSpeakerName.c_str(), safeSub.c_str());
  fclose(f);
}

// Append a sentinel that tells Python the current stream is complete.
// Python converts this into [queue] turn_done=1 after all prior lines are queued.
void AppendEndTurnMarker() {
  FILE* f = nullptr;
  std::string workIniPath = UnityWorkIniPath();
  fopen_s(&f, workIniPath.c_str(), "a");
  if (!f) {
    REX::ERROR("UnityChat: could not open work file for end-turn append");
    return;
  }
  fprintf(f, "__ENDTURN__\n");
  fclose(f);
  REX::INFO("UnityChat: end-turn marker queued");
}

// Tracks the last real (audio-bearing) line of a completed stream, plus how
// many there were. Mirrors STOBE's parseState.lastSpeaker/lastSubtitle/
// lineCount - exactly the information MaybeFireRechatFollowup needs: who
// just spoke, what they said, and whether anything was actually said at
// all (rechat.php itself can legitimately produce zero lines - depth
// budget exhausted, no eligible responder, etc. - and that's the signal
// to stop the chain, not an error).
//
// firstSubtitle is additional to STOBE's own tracked fields - needed for
// addressee detection specifically. An opening address ("Sarah, have you
// heard...") naturally lives in the FIRST line of a multi-line turn, but
// lastSubtitle only ever holds the trailing line by the time the whole
// turn completes (it gets overwritten on every line). Confirmed as a
// real gap via a live test: a two-line turn's addressee-naming opener
// was completely invisible to the addressee-detection heuristic, which
// only ever checked lastSubtitle.
struct StreamLineTracker {
  std::uint64_t epoch = 0;   // chain this stream belongs to (see g_chatEpoch)
  int lineCount = 0;
  std::string lastSpeaker;
  std::string lastSubtitle;
  std::string firstSubtitle;
};

// --- Stream callback -------------------------------------------------------
// Called by PostToUnityWithResponseStream for each streamed line. Return true
// to keep consuming.
bool OnUnityStreamLine(const std::string& line, void* userData) {
  if (userData && !IsEpochCurrent(static_cast<StreamLineTracker*>(userData)->epoch)) {
    REX::INFO("UnityChat: chain superseded mid-stream, dropping remaining lines");
    return false;  // stop reading this stream
  }
  ParsedLine pl;
  if (!ParseServerLine(line, pl)) {
    return true;  // skip non-line output, keep streaming
  }
  // Never render a line attributed to the player - the player should
  // never have dialogue fabricated and rendered as if they said it,
  // regardless of voice/scene mismatches downstream. rechat.php can
  // apparently select "Player" as a simulated responder in some modes;
  // this is the single choke point every streamed line (chat/bored/
  // rechat alike) passes through, so filtering here catches all of them
  // uniformly. Deliberately NOT counted toward the tracker either - the
  // next rechat hop's eventData should be built from the last REAL NPC
  // line, not from discarded fabricated content as if it had actually
  // been said.
  if (pl.actor == "Player") {
    REX::INFO("UnityChat: discarding fabricated Player line, not queuing: '{}'",
              pl.subtitle);
    return true;
  }
  if (pl.ttsHash.empty()) {
    // No audio hash (TTS not applied to this line) - nothing to synthesize.
    REX::INFO("UnityChat: line has no tts hash, skipping audio: '{}'",
              pl.subtitle);
    return true;
  }
  AppendWorkLine(pl);
  REX::INFO("UnityChat: work line queued hash={} actor='{}' '{}'",
            pl.ttsHash, pl.actor, pl.subtitle);
  if (userData) {
    auto* tracker = static_cast<StreamLineTracker*>(userData);
    if (tracker->lineCount == 0) {
      tracker->firstSubtitle = pl.subtitle;
    }
    tracker->lineCount++;
    tracker->lastSpeaker = pl.actor;
    tracker->lastSubtitle = pl.subtitle;
  }
  return true;
}

struct ConvTask {
  std::string profile;
  std::string playerText;
  long long gamets;

  // --- Rechat chain threading -----------------------------------------
  // All default to values correct for a fresh, non-chained turn -
  // ConversationThread/BoredEventThread don't need to set these
  // explicitly unless deviating from the default.
  //
  // eventType: which processor this task's own request routes to.
  std::string eventType = "inputtext"; // "inputtext" | "bored" | "rechat"
  // Chain epoch this task belongs to - see g_chatEpoch.
  std::uint64_t epoch = 0;
  // addressee: who THIS task's speaker is addressing. Empty by default -
  // was "Player" (matching STOBE's own unconditional default), but
  // confirmed via a full trace through rechat.php's candidate-selection
  // code that sending the literal string "Player" is actively harmful,
  // not just imprecise: rechat.php's candidate exclusion only checks
  // against the real player character's actual name, never against this
  // reserved word, so our own hardcoded default was smuggling a fake
  // candidate straight through the exclusion guard and winning selection
  // outright in tight/conversational mode. Empty means "no signal" -
  // rechat.php's own pushCandidate() already short-circuits on an empty
  // candidate name, so this is a true no-op hint rather than a wrong
  // guess dressed up as a real one. Embedded directly as "(talking to:
  // X)" in the NEXT rechat hop's eventData once this task's stream
  // completes, and used as the rechat_target hint - see
  // MaybeFireRechatFollowup/RechatThread.
  std::string addressee = "";
  // initiator: who started the whole exchange. Threaded unchanged down
  // every hop of a chain (STOBE: nextTask->initiatorSpeaker = currentTask.
  // initiatorSpeaker, copied as-is, never recomputed mid-chain). Empty by
  // default for the same reason as addressee above - lower-confidence
  // fix than that one specifically, since rechat.php's code only ever
  // uses this for EXCLUSION (never pushes it as a candidate itself), so
  // it doesn't have the same confirmed win-selection bug. Still changed
  // for consistency: rechat.php already has its own separate, correctly-
  // checked $playerName parameter for identifying the real player
  // character, so this field doesn't need to also carry that meaning via
  // a magic string, and empty avoids any chance of the literal word
  // "Player" being compared against something unexpected elsewhere.
  std::string initiator = "";
  // rechatDepth: 0 for a fresh turn. Each rechat hop increments by 1.
  int rechatDepth = 0;
  // rechatEventData: precomputed "speaker: message (talking to: X)" string
  // for a rechat request specifically. Only set (non-empty) when
  // eventType=="rechat" - ConversationThread/BoredEventThread build their
  // own eventData inline instead, per their existing established format.
  std::string rechatEventData;
};

// Maximum rechat chain depth, matching STOBE's own MAX_RECHAT_DEPTH
// (HTTPManager.cpp in CHIM's lineage uses the same constant name/value).
// A hard client-side ceiling independent of rechat.php's own
// RECHAT_RESPONSES/RECHAT_PROBABILITY budget - belt-and-suspenders in case
// a server-side misconfiguration or bug ever produced an unbounded chain.
static constexpr int kMaxRechatDepth = 10;

DWORD WINAPI RechatThread(LPVOID param);

// Called at the end of every stream. The __ENDTURN__ marker (which Python
// turns into [queue] turn_done=1) is written ONLY when the whole chain is
// finished - i.e. when no rechat hop follows. Previously every hop wrote its
// own marker; turn_done then went to 1 after the FIRST hop and stayed 1
// (Python never clears it), so if playback caught up with generation during
// the gap before the next hop's lines arrived, the conversation loop decided
// the turn was over and the rest of the chain was silently never played.
// A superseded chain writes no marker - whatever replaced it writes its own.
void FinishStream(const ConvTask& task, const StreamLineTracker& tracker);

// Fires a rechat follow-up if the turn that just completed actually
// produced real spoken lines. Called unconditionally at the end of
// ConversationThread/BoredEventThread/RechatThread, right after their
// lines are queued - NOT after they finish playing. Confirmed from
// STOBE's own source (ChatBox.cpp) that this is deliberate: rechat is
// dispatched the instant the current turn's lines are queued, with
// StobeTiming.cpp's ShouldWaitForPlaybackBeforeRechatDispatch() hardcoded
// to false, so the next line prefetches while the current one is still
// speaking rather than waiting for a silence gap - this is what makes a
// rechat-driven exchange feel continuous instead of stilted.
//
// Deliberately does NOT attempt local responder selection (STOBE's own
// TrySelectRechatResponder). Confirmed from STOBE's source that a failed
// local guess falls back to "server_authoritative_fallback" anyway
// (clearing the target hint entirely), so profile=/rechat_target= are
// simply left unset here and rechat.php's own candidate-selection logic
// (already fully read this session: tight/conversational/group/random
// mode selection, environment/room compatibility, chain-depth budget via
// RECHAT_RESPONSES/RECHAT_PROBABILITY) does the actual picking
// server-side. This is the "server is authoritative, client only sends
// hints" principle STOBE's own code states outright in comments.
//
// tracker.lineCount == 0 is the natural chain-termination signal, not an
// error case - rechat.php can legitimately produce zero lines (depth
// budget exhausted, no eligible responder, private mode, etc.), and that
// comes back exactly the same way a hop that was never going to continue
// does, so no separate "did the server say stop" parsing is needed.
bool MaybeFireRechatFollowup(const ConvTask& completedTask, const StreamLineTracker& tracker) {
  if (!IsEpochCurrent(completedTask.epoch)) {
    REX::INFO("UnityChat: chain superseded, not firing rechat follow-up");
    return false;
  }
  if (tracker.lineCount == 0 || tracker.lastSpeaker.empty()) {
    return false;
  }

  int nextDepth = completedTask.rechatDepth + 1;
  if (nextDepth > kMaxRechatDepth) {
    REX::INFO("UnityChat: rechat chain depth cap ({}) reached, not continuing", kMaxRechatDepth);
    return false;
  }

  // Detect who the speaker is actually addressing. Checks TWO patterns,
  // both confirmed as real and common from actual in-game dialogue, not
  // hypothetical: an OPENING address ("Hey Sarah, I've been...") - the
  // LAST WORD before the first comma, not the whole prefix, since a
  // greeting routinely precedes the name - and a CLOSING vocative ("The
  // Starborn do unsettle me, Noel.") - the FIRST WORD after the last
  // comma. A single-comma sentence naturally falls through from the
  // opening check (which won't match ordinary prose) to the closing
  // check without any special-casing needed. Checks firstSubtitle before
  // lastSubtitle - an address naturally lives in the FIRST line of a
  // multi-line turn, and lastSubtitle alone missed it entirely in a live
  // test. Falls back to completedTask.addressee (empty by default now -
  // see ConvTask's own addressee field comment for why the old "Player"
  // default was actively harmful, not just imprecise) when nothing
  // matches - this is a heuristic, not a guaranteed-correct parse, but
  // it catches the common cases. Confirmed as a real, not just cosmetic,
  // issue: the OLD hardcoded "Player" default was being sent to
  // rechat.php as if the speaker were
  // addressing the player even when the dialogue text clearly named
  // another NPC - which appears to directly bias the server toward
  // fabricating Player-attributed response lines that then have to be
  // discarded client-side anyway.
  auto trimWord = [](std::string w) -> std::string {
    size_t start = w.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = w.find_last_not_of(" \t.!?");
    if (end == std::string::npos || end < start) return "";
    return w.substr(start, end - start + 1);
  };

  auto detectAddresseeFromLine = [&](const std::string& subtitle) -> std::string {
    if (subtitle.empty()) return "";

    size_t firstComma = subtitle.find(',');
    if (firstComma != std::string::npos && firstComma > 0) {
      std::string prefix = subtitle.substr(0, firstComma);
      size_t lastSpace = prefix.find_last_of(' ');
      std::string lastWord = (lastSpace == std::string::npos) ? prefix : prefix.substr(lastSpace + 1);
      lastWord = trimWord(lastWord);
      if (!lastWord.empty()) {
        std::string match = FindActivatedNpcNameExcluding(lastWord, tracker.lastSpeaker);
        if (!match.empty()) return match;
      }
    }

    size_t lastComma = subtitle.find_last_of(',');
    if (lastComma != std::string::npos && lastComma + 1 < subtitle.size()) {
      std::string suffix = subtitle.substr(lastComma + 1);
      size_t spacePos = suffix.find(' ');
      std::string firstWord = (spacePos == std::string::npos) ? suffix : suffix.substr(0, spacePos);
      firstWord = trimWord(firstWord);
      if (!firstWord.empty()) {
        std::string match = FindActivatedNpcNameExcluding(firstWord, tracker.lastSpeaker);
        if (!match.empty()) return match;
      }
    }

    return "";
  };

  std::string detectedAddressee = detectAddresseeFromLine(tracker.firstSubtitle);
  if (detectedAddressee.empty()) {
    detectedAddressee = detectAddresseeFromLine(tracker.lastSubtitle);
  }
  if (detectedAddressee.empty()) {
    detectedAddressee = completedTask.addressee;
  }

  ConvTask* t = new ConvTask();
  t->profile = "";              // server-authoritative selection
  t->playerText = "";
  t->gamets = static_cast<long long>(time(nullptr));
  t->eventType = "rechat";
  t->addressee = tracker.lastSpeaker;      // for THIS new hop's own eventual follow-up, if any
  t->initiator = completedTask.initiator;  // threaded unchanged down the chain
  t->rechatDepth = nextDepth;
  t->epoch = completedTask.epoch;          // same chain
  t->rechatEventData = tracker.lastSpeaker + ": " + tracker.lastSubtitle;
  if (!detectedAddressee.empty()) {
    t->rechatEventData += " (talking to: " + detectedAddressee + ")";
  }

  REX::INFO("UnityChat: rechat follow-up queued speaker='{}' addressee='{}' (detected={}) depth={}",
            tracker.lastSpeaker, detectedAddressee,
            detectedAddressee != completedTask.addressee, nextDepth);

  CreateThread(NULL, 0, RechatThread, t, 0, NULL);
  return true;
}

void FinishStream(const ConvTask& task, const StreamLineTracker& tracker) {
  if (!IsEpochCurrent(task.epoch)) {
    REX::INFO("UnityChat: chain superseded, no end-turn marker");
    return;
  }
  if (!MaybeFireRechatFollowup(task, tracker)) {
    AppendEndTurnMarker();  // chain complete
  }
}

DWORD WINAPI ConversationThread(LPVOID param) {
  ConvTask* t = static_cast<ConvTask*>(param);

  // Start this turn with a clean work file. Python resets the playback queue
  // itself on its side, so we do NOT reset MantellaQueue.ini here.
  ResetWorkFile();

  // eventData = "Speaker: text" - the server splits on ": " for speaker/message.
  std::string eventData = "Player: " + t->playerText;
  std::string dataParam = BuildStreamQueryData("inputtext", eventData, t->gamets);

  // people= tells the server which activated NPCs (beyond the profile-
  // targeted one) are available to speak this turn - the prerequisite for
  // any line in the response actually naming a second speaker. Same
  // envelope/param shape as the bored.php contract, generalized to the
  // regular inputtext/chat request per the handoff's "immediate next step".
  std::string peopleParam = BuildActivatedRosterPeopleParam();

  std::wstring endpoint = L"/UNITYServer/stream.php?DATA=" + ToWide(dataParam) +
                          L"&profile=" + ToWide(UrlEncode(t->profile)) +
                          L"&people=" + ToWide(UrlEncode(peopleParam)) +
                          L"&mode=chat";

  REX::INFO("UnityChat: dispatching inputtext profile='{}' text='{}' people={}",
            t->profile, t->playerText, peopleParam);

  StreamLineTracker tracker;
  tracker.epoch = t->epoch;
  bool ok = PostToUnityWithResponseStream(endpoint, "", OnUnityStreamLine, &tracker);
  if (!ok) {
    REX::ERROR("UnityChat: stream request failed");
  } else {
    REX::INFO("UnityChat: stream complete");
  }

  FinishStream(*t, tracker);

  delete t;
  return 0;
}

// Bored-event counterpart to ConversationThread. Reuses ConvTask (playerText
// left empty - bored events have no player message) and the same
// UnityWork.ini -> MantellaQueue.ini pipeline; PlayQueuedLine and
// unity_wem_gen.py don't need to know the difference between a bored event
// and an ordinary conversation turn.
DWORD WINAPI BoredEventThread(LPVOID param) {
  ConvTask* t = static_cast<ConvTask*>(param);

  ResetWorkFile();

  // No player message for a bored event - eventData is empty. (CHIM's own
  // trigger site sends the player's current location string as the 4th
  // field; omitted here since there's no location-string helper in this
  // codebase yet. bored.php doesn't appear to require it for anything
  // beyond its own storeEvent() logging call, so this is a simplification,
  // not a missing requirement - revisit if server-side logs end up wanting
  // it.)
  std::string dataParam = BuildStreamQueryData("bored", "", t->gamets);

  std::string peopleParam = BuildActivatedRosterPeopleParam();

  // No &mode= param here - that's a chat.php-specific dialogue-delivery
  // setting (talk/whisper/shout/autochat/cheat/narrator) that bored.php
  // doesn't read at all.
  std::wstring endpoint = L"/UNITYServer/stream.php?DATA=" + ToWide(dataParam) +
                          L"&profile=" + ToWide(UrlEncode(t->profile)) +
                          L"&people=" + ToWide(UrlEncode(peopleParam));

  REX::INFO("UnityChat: dispatching bored event profile='{}' people={}",
            t->profile, peopleParam);

  StreamLineTracker tracker;
  tracker.epoch = t->epoch;
  bool ok = PostToUnityWithResponseStream(endpoint, "", OnUnityStreamLine, &tracker);
  if (!ok) {
    REX::ERROR("UnityChat: bored event stream request failed");
  } else {
    REX::INFO("UnityChat: bored event stream complete");
  }

  FinishStream(*t, tracker);

  delete t;
  return 0;
}

// Rechat continuation - fired automatically by MaybeFireRechatFollowup at
// the end of ConversationThread/BoredEventThread/RechatThread itself,
// never called directly from outside UnityChat.cpp (no public entry point
// needed - this is purely an internal continuation of whatever chain
// started with a normal player turn or bored event). Same
// UnityWork.ini -> MantellaQueue.ini pipeline as every other turn type;
// PlayQueuedLine and unity_wem_gen.py don't need to know a line came from
// a rechat hop rather than an ordinary turn.
DWORD WINAPI RechatThread(LPVOID param) {
  ConvTask* t = static_cast<ConvTask*>(param);

  // Deliberately does NOT call ResetWorkFile() here, unlike
  // ConversationThread/BoredEventThread. Those represent a genuinely
  // fresh, non-overlapping start (a new G-key press or a new bored-event
  // timer fire), where truncating the work file is correct. A rechat hop
  // is different by design - it fires immediately after the PREVIOUS
  // turn's stream completes, deliberately overlapping with that turn
  // potentially still being read/processed/played, not after it. Calling
  // ResetWorkFile() here truncated the file out from under Python's
  // watcher mid-read, silently destroying whichever of the previous
  // turn's lines it hadn't gotten to yet - confirmed directly via a live
  // test: a line C++ definitely queued (visible in UnityChat's own log)
  // never once appeared in Python's own "processing X" log, sandwiched
  // exactly between this reset and the moment it should have been read.
  // Confirmed via unity_wem_gen.py's own source that this fix is safe:
  // its watcher is already built to handle a continuously-growing file
  // correctly (tracks a "consumed" line count, not a reset assumption),
  // including multiple __ENDTURN__ markers across multiple hops each
  // correctly triggering write_turn_done() in sequence - the "work file
  // was truncated; resetting consumed counter" log line was Python
  // faithfully reporting damage this call was causing, not a Python-side
  // bug of its own.

  std::string dataParam = BuildStreamQueryData("rechat", t->rechatEventData, t->gamets);
  std::string peopleParam = BuildActivatedRosterPeopleParam();

  // profile=/rechat_target=/rechat_target_sid= deliberately omitted - see
  // MaybeFireRechatFollowup's comment on server-authoritative selection.
  // rechat.php reads $_GET['profile'] with an empty-string default
  // (confirmed from its source), so omitting the param entirely behaves
  // identically to STOBE sending it present-but-empty.
  std::wstring endpoint = L"/UNITYServer/stream.php?DATA=" + ToWide(dataParam) +
                          L"&rechat_depth=" + ToWide(std::to_string(t->rechatDepth)) +
                          L"&initiator=" + ToWide(UrlEncode(t->initiator)) +
                          L"&people=" + ToWide(UrlEncode(peopleParam));

  REX::INFO("UnityChat: dispatching rechat depth={} initiator='{}' data='{}'",
            t->rechatDepth, t->initiator, t->rechatEventData);

  StreamLineTracker tracker;
  tracker.epoch = t->epoch;
  bool ok = PostToUnityWithResponseStream(endpoint, "", OnUnityStreamLine, &tracker);
  if (!ok) {
    REX::ERROR("UnityChat: rechat stream request failed");
  } else {
    REX::INFO("UnityChat: rechat stream complete ({} lines)", tracker.lineCount);
  }

  // Continue the chain if this hop produced real lines - mirrors STOBE's
  // "if (parseState.lineCount > 0) DispatchRechatFollowup(...)" exactly.
  // Self-terminates when rechat.php returns zero lines (depth budget
  // reached, no eligible responder, private mode, etc.).
  FinishStream(*t, tracker);

  delete t;
  return 0;
}

} // namespace

float EstimateSubtitleDurationSeconds(const std::string& subtitle) {
  float secs = static_cast<float>(subtitle.length()) / 15.0f;
  if (secs < 1.8f) secs = 1.8f;
  if (secs > 12.0f) secs = 12.0f;
  return secs;
}

void CancelPendingChats() {
  std::uint64_t e = g_chatEpoch.fetch_add(1) + 1;
  REX::INFO("UnityChat: pending chats cancelled (epoch now {})", e);
}

void SendUnityConversation(const std::string& npcProfile,
                           const std::string& playerText,
                           long long gamets) {
  CancelPendingChats();  // a new player turn supersedes any running chain
  ConvTask* t = new ConvTask();
  t->epoch = g_chatEpoch.load();
  t->profile = npcProfile;
  t->playerText = playerText;
  t->gamets = (gamets > 0) ? gamets : 1;
  // eventType/addressee/initiator all match ConvTask's own defaults
  // ("inputtext"/""/"") - set explicitly anyway for consistency/
  // readability with SendUnityBoredEvent's own explicit initiator
  // override right below.
  t->eventType = "inputtext";
  t->addressee = "";
  t->initiator = "";
  CreateThread(NULL, 0, ConversationThread, t, 0, NULL);
}

void SendUnityBoredEvent(const std::string& npcProfile, long long gamets) {
  CancelPendingChats();  // a new bored event supersedes any running chain
  ConvTask* t = new ConvTask();
  t->epoch = g_chatEpoch.load();
  t->profile = npcProfile;
  t->playerText = "";
  t->gamets = (gamets > 0) ? gamets : 1;
  t->eventType = "bored";
  // Unlike SendUnityConversation, initiator is NOT left at the ConvTask
  // default ("Player") - a bored event's chain wasn't started by the
  // player at all, it was started by npcProfile deciding to speak
  // unprompted. addressee stays at the default ("Player") though, same
  // as the reasoning documented on ConvTask itself: we don't parse a
  // discrete listener out of generated bored-event text, and STOBE
  // doesn't special-case this differently either.
  t->initiator = npcProfile;
  CreateThread(NULL, 0, BoredEventThread, t, 0, NULL);
}