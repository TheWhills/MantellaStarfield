#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winhttp.h>

#undef ERROR

#include <RE/Starfield.h>
#include <RE/T/TESTopicInfo.h>
#include <RE/T/TESResponse.h>
#include <RE/T/TESForm.h>
#include <SFSE/SFSE.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

#pragma comment(lib, "winhttp.lib")

static const char* BRIDGE_DIR = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\";
static const char* BRIDGE_INI = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\Bridge.ini";
// Queue file — Python is the SOLE writer (write_index + [line_N] sections);
// C++ is the sole reader. Separate file from Bridge.ini so the two processes
// never write the same file (no read-modify-write race).
static const char* QUEUE_INI = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\MantellaQueue.ini";

static std::atomic<bool> g_conversationActive{ false };
static std::atomic<bool> g_mantellaSpeaking{ false };
static std::atomic<std::uint32_t> g_currentSpeakerFormID{ 0 };
static const SFSE::TaskInterface* g_taskInterface = nullptr;
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
    // wem.parent_path(); the generics are where spawned NPCs read.
    std::vector<std::string> folders = {
        "GenericMaleRough", "GenericMaleSmooth",
        "GenericMale01", "GenericMale02", "GenericMale03", "GenericMale04",
        "GenericFemaleRough", "GenericFemaleSmooth",
        "GenericFemale01", "GenericFemale02", "GenericFemale03", "GenericFemale04",
        "NPCMBarrett", "NPCFSarahMorgan"
    };

    std::error_code ec;
    // 1) The line's own folder (covers named NPCs reading their own folder).
    fs::copy_file(lineFfxanim, wem.parent_path() / "00666A79.ffxanim",
                  fs::copy_options::overwrite_existing, ec);

    // 2) All generic + registered folders under both MO2 and Data bases.
    for (const auto& base : { MO2_VOICE_BASE_CPP, DATA_VOICE_BASE_CPP }) {
        for (const auto& folder : folders) {
            fs::path dst = fs::path(base) / folder / "00666A79.ffxanim";
            std::error_code e2;
            fs::create_directories(dst.parent_path(), e2);
            fs::copy_file(lineFfxanim, dst, fs::copy_options::overwrite_existing, e2);
        }
    }
    REX::INFO("Mantella: Activated lips for slot {} (00666A79.ffxanim updated)", slot);
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
    float duration = 2.0f;
    try { duration = std::stof(durStr); } catch (...) {}

    SetCurrentWemPath(wemPath);     // hook redirect target for this line
    ActivateLipsForCurrentLine();   // copy this line's ffxanim onto 00666A79
    if (!subtitle.empty()) PatchDialogueSubtitle(subtitle);

    // Buffer added to the line's duration before advancing. Covers two timing
    // offsets that otherwise let the NEXT line's lip activation overwrite the
    // shared 00666A79.ffxanim slot before THIS line's audio finishes (which
    // shows as the lips drifting off at a long line's tail):
    //   1) Wwise starts the wem ~150-500ms AFTER we begin the line.
    //   2) Mantella's wait_time_buffer=-1.0 makes durations read slightly short.
    static const int PLAYBACK_BUFFER_MS = 700;
    int waitMs = static_cast<int>(duration * 1000.0f) + PLAYBACK_BUFFER_MS;
    REX::INFO("Mantella: Playing line {} (duration={}s)", idx, duration);
    WriteSpeakingState(duration);
    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
    WritePrivateProfileStringA("speaking", "active", "0", BRIDGE_INI);
}

void RunConversationLoop(const std::string& startNonce)
{
    std::string lastNonce = startNonce;
    // How many lines to keep requested ahead of what we've played. The queue
    // buffers them, so by the time we finish line N, line N+1 is already there.
    // Lower = first line plays sooner (less waiting before playback starts);
    // higher = more buffer for later lines. 2 balances first-line latency
    // against keeping the tail seamless.
    const int PREFETCH_AHEAD = 2;

    int localRead = 0;          // next line index WE will play
    bool turnDone = false;      // Mantella signalled player_talk/end for this turn
    std::string pendingStop;    // "player_talk" / "end" once turnDone

    REX::INFO("Mantella: Getting NPC greeting");
    // Fresh queue for this conversation (clear any stale lines from a prior run).
    {
        std::ofstream qf(QUEUE_INI, std::ios::trunc);
        qf << "[queue]\nwrite_index=0\nread_index=0\n";
    }

    while (g_conversationActive) {
        // ---- 1) REQUEST AHEAD --------------------------------------------
        // Pull lines from Mantella until the queue is PREFETCH_AHEAD past
        // playback, or until Mantella says the turn is over. Each request makes
        // Python generate + enqueue one [line_N]; the reply tells us what kind.
        // We pace off the actual on-disk writeIndex (source of truth) so a line
        // that failed to enqueue can't make us over-request and stall.
        while (!turnDone && (ReadQueueWriteIndex() - localRead) < PREFETCH_AHEAD) {
            std::string resp = SendToMantella(BuildContinueJson());
            std::string rt = GetReplyType(resp);
            if (rt == "mantella_npc_talk") {
                REX::INFO("Mantella: Requested line (write_index now {})", ReadQueueWriteIndex());
            }
            else if (rt == "mantella_player_talk") {
                turnDone = true; pendingStop = "player_talk";
                REX::INFO("Mantella: Turn done -> player_talk");
            }
            else if (rt == "mantella_end_conversation") {
                turnDone = true; pendingStop = "end";
                REX::INFO("Mantella: Turn done -> end");
            }
            else {
                // Unknown/empty — brief wait, don't spin.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                break;
            }
        }

        // ---- 2) PLAY NEXT QUEUED LINE ------------------------------------
        // Play strictly line_{localRead} when it exists in the queue. The index
        // IS the identity, so we can never grab the wrong or a stale line.
        int writeIndex = ReadQueueWriteIndex();
        if (localRead < writeIndex) {
            g_mantellaSpeaking.store(true);
            PlayQueuedLine(localRead);
            localRead++;
            // Persist read_index for visibility/debug (C++ is sole writer here).
            WritePrivateProfileStringA("queue", "read_index",
                std::to_string(localRead).c_str(), QUEUE_INI);
            g_mantellaSpeaking.store(false);
            continue;  // loop back: request more, play more
        }

        // ---- 3) QUEUE DRAINED --------------------------------------------
        if (!turnDone) {
            // We've played everything requested but Mantella isn't done — wait
            // briefly for the next line to finish generating (covers first-line
            // TTS latency without a fixed timeout cascade).
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Turn is done AND queue fully played -> handle the stop.
        if (pendingStop == "end") {
            REX::INFO("Mantella: Conversation ended by Mantella");
            g_conversationActive = false;
            g_mantellaSpeaking.store(false);
            return;
        }

        // player_talk: await player input, then start a fresh turn (reset queue).
        REX::INFO("Mantella: Awaiting player input");
        g_mantellaSpeaking.store(false);
        WriteAwaitInput(lastNonce);
        bool gotInput = false;
        while (g_conversationActive && !gotInput) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::string currentNonce = ReadIniValue("request", "nonce", "");
            std::string currentType = ReadIniValue("request", "type", "");
            std::transform(currentType.begin(), currentType.end(), currentType.begin(), ::tolower);
            if (currentNonce == lastNonce) continue;
            lastNonce = currentNonce;
            if (currentType == "end") {
                REX::INFO("Mantella: Player ended conversation");
                g_conversationActive = false;
                g_mantellaSpeaking.store(false);
                SendToMantella(BuildEndJson());
                return;
            }
            else if (currentType == "player_input") {
                std::string playerInput = ReadIniValue("request", "player_input", "");
                REX::INFO("Mantella: Player said: {}", playerInput);
                // Reset the queue file BEFORE telling Mantella the player spoke,
                // so the new turn's first line lands in line_0. C++ owns this
                // reset (it's the authoritative turn boundary); Python only ever
                // appends from the current write_index. Truncating the file clears
                // both write_index and all old [line_*] sections at once.
                {
                    std::ofstream qf(QUEUE_INI, std::ios::trunc);
                    qf << "[queue]\nwrite_index=0\nread_index=0\n";
                }
                SendToMantella(BuildPlayerInputJson(playerInput));
                localRead = 0;
                turnDone = false;
                pendingStop.clear();
                gotInput = true;
            }
        }
    }
}

void BridgeMonitorThread()
{
    std::filesystem::create_directories(BRIDGE_DIR);
    REX::INFO("Mantella: Bridge monitor started");
    REX::INFO("Mantella: Watching {}", BRIDGE_INI);
    std::string lastNonce;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!std::filesystem::exists(BRIDGE_INI)) continue;
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
            std::string startResponse = SendToMantella(BuildStartJson(speaker));
            std::string startReplyType = GetReplyType(startResponse);
            REX::INFO("Mantella: Start reply type: {}", startReplyType);
            int retries = 0;
            while (startReplyType != "mantella_start_conversation_completed" && retries < 20) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                startResponse = SendToMantella(BuildContinueJson());
                startReplyType = GetReplyType(startResponse);
                REX::INFO("Mantella: Waiting for start completed, reply: {}", startReplyType);
                retries++;
            }
            if (startReplyType == "mantella_start_conversation_completed") {
                REX::INFO("Mantella: Conversation started successfully");
                std::thread(RunConversationLoop, nonce).detach();
            } else {
                REX::ERROR("Mantella: Failed to start conversation after {} retries", retries);
                g_conversationActive = false;
            }
        }
        else if (type == "end") {
            g_conversationActive = false;
            g_mantellaSpeaking.store(false);
            SendToMantella(BuildEndJson());
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
