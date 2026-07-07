// -----------------------------------------------------------------------------
// UnityChat.cpp - Stage 3 conversation streaming (audio via Python generator).
//
// Streams an inputtext turn from UNITYServer. For each returned line that has a
// tts= hash, appends "hash|voice_folder|subtitle" to UnityWork.ini. A separate
// Python watcher (unity_wem_gen.py) fetches soundcache/<hash>.wav, builds the
// wem via WwiseConsole, deploys it into the MantellaStarfield.esp voice folder,
// and writes the [line_N] section into MantellaQueue.ini. The existing C++
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
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

// Work file the Python generator watches. Plugin APPENDS pending TTS lines here;
// Python consumes them, builds wems, and writes the playback queue below.
static const char* UNITY_WORK_INI =
    "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\UnityWork.ini";

// Voice folder the test wem deploys into. Must be under a path containing
// "MantellaStarfield.esp" so the C++ Wwise path hook fires. Hardcoded for the
// first-audio test; per-NPC voice mapping (core_voiceid) comes later.
static const char* TEST_VOICE_FOLDER = "NPCFSarahMorgan";

namespace {

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
  fopen_s(&f, UNITY_WORK_INI, "w");
  if (f) {
    fclose(f);
  } else {
    REX::ERROR("UnityChat: failed to reset work file");
  }
}

// Append one pending line for the Python generator: hash|voice|subtitle
void AppendWorkLine(const ParsedLine& pl) {
  FILE* f = nullptr;
  fopen_s(&f, UNITY_WORK_INI, "a");
  if (!f) {
    REX::ERROR("UnityChat: could not open work file for append");
    return;
  }
  // Newlines in the subtitle would corrupt the line-based work file; strip them.
  std::string safeSub = pl.subtitle;
  std::replace(safeSub.begin(), safeSub.end(), '\n', ' ');
  std::replace(safeSub.begin(), safeSub.end(), '\r', ' ');
  fprintf(f, "%s|%s|%s\n", pl.ttsHash.c_str(), TEST_VOICE_FOLDER, safeSub.c_str());
  fclose(f);
}

// Append a sentinel that tells Python the current stream is complete.
// Python converts this into [queue] turn_done=1 after all prior lines are queued.
void AppendEndTurnMarker() {
  FILE* f = nullptr;
  fopen_s(&f, UNITY_WORK_INI, "a");
  if (!f) {
    REX::ERROR("UnityChat: could not open work file for end-turn append");
    return;
  }
  fprintf(f, "__ENDTURN__\n");
  fclose(f);
  REX::INFO("UnityChat: end-turn marker queued");
}

// --- Stream callback -------------------------------------------------------
// Called by PostToUnityWithResponseStream for each streamed line. Return true
// to keep consuming.
bool OnUnityStreamLine(const std::string& line, void* /*userData*/) {
  ParsedLine pl;
  if (!ParseServerLine(line, pl)) {
    return true;  // skip non-line output, keep streaming
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
  return true;
}

struct ConvTask {
  std::string profile;
  std::string playerText;
  long long gamets;
};

DWORD WINAPI ConversationThread(LPVOID param) {
  ConvTask* t = static_cast<ConvTask*>(param);

  // Start this turn with a clean work file. Python resets the playback queue
  // itself on its side, so we do NOT reset MantellaQueue.ini here.
  ResetWorkFile();

  // eventData = "Speaker: text" - the server splits on ": " for speaker/message.
  std::string eventData = "Player: " + t->playerText;
  std::string dataParam = BuildStreamQueryData("inputtext", eventData, t->gamets);

  std::wstring endpoint = L"/UNITYServer/stream.php?DATA=" + ToWide(dataParam) +
                          L"&profile=" + ToWide(UrlEncode(t->profile)) +
                          L"&mode=chat";

  REX::INFO("UnityChat: dispatching inputtext profile='{}' text='{}'",
            t->profile, t->playerText);

  bool ok = PostToUnityWithResponseStream(endpoint, "", OnUnityStreamLine, nullptr);
  if (!ok) {
    REX::ERROR("UnityChat: stream request failed");
  } else {
    REX::INFO("UnityChat: stream complete");
  }

  // Signal the Python watcher that no more TTS lines are coming for this turn.
  AppendEndTurnMarker();

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

void SendUnityConversation(const std::string& npcProfile,
                           const std::string& playerText,
                           long long gamets) {
  ConvTask* t = new ConvTask();
  t->profile = npcProfile;
  t->playerText = playerText;
  t->gamets = (gamets > 0) ? gamets : 1;
  CreateThread(NULL, 0, ConversationThread, t, 0, NULL);
}
