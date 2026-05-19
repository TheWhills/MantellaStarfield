#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winhttp.h>

#undef ERROR

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

static const char* BRIDGE_DIR = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\";
static const char* BRIDGE_INI = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\Bridge.ini";

static const char* MANTELLA_VOICE_PATH_NARROW =
    "Sound\\Voice\\MantellaStarfield.esp\\GenericMaleEvenToned\\002B2475";

static std::atomic<bool> g_conversationActive{ false };
static std::atomic<bool> g_mantellaSpeaking{ false };

// Hook 1: BSFixedString setter inside DialogueResponse ctor
using BSFixedString_Set_t = void(*)(void* dest, const wchar_t* src, std::uint32_t unk);
static BSFixedString_Set_t g_originalBSFixedStringSet = nullptr;

// Hook 2: FUN_140d41330 near top of FUN_140d41ac0
using Func140d41330_t = void*(*)(void*, void*, void*, int, void*, void*, int);
static Func140d41330_t g_orig140d41330 = nullptr;

// Hook 3: FUN_14190f7e0 - facial animation loader
using FaceAnimFunc_t = char(*)(void*, long long, char*);
static FaceAnimFunc_t g_origFaceAnimFunc = nullptr;

// Hook 4: FUN_1419c2290 - patches DialogueResponse voiceFilePath before face anim loads
using FUN_1419c2290_t = void(*)(void*, void*, void*, long long);
static FUN_1419c2290_t g_orig1419c2290 = nullptr;

void HookedBSFixedStringSet(void* dest, const wchar_t* src, std::uint32_t unk)
{
    REX::INFO("Mantella: HookedBSFixedStringSet called");
    g_originalBSFixedStringSet(dest, src, unk);
}

void* Hooked140d41330(void* a, void* b, void* c, int d, void* e, void* f, int g)
{
    REX::INFO("Mantella: FUN_140d41330 called - dialogue response being created!");
    return g_orig140d41330(a, b, c, d, e, f, g);
}

char HookedFaceAnimFunc(void* param_1, long long param_2, char* param_3)
{
    if (param_3 != nullptr && strlen(param_3) > 0) {
        REX::INFO("Mantella: FaceAnim called with path: {}", param_3);
        if (g_conversationActive.load()) {
            static const char* SARAH_ANIM_PATH =
                "Data\\Sound\\Voice\\Starfield.esm\\NPCFSarahMorgan\\0093F2BC.wem";
            char result = g_origFaceAnimFunc(param_1, param_2, const_cast<char*>(SARAH_ANIM_PATH));
            REX::INFO("Mantella: FaceAnim redirect returned: {}", (int)result);
            return result;
        }
    }
    return g_origFaceAnimFunc(param_1, param_2, param_3);
}

void Hooked1419c2290(void* param_1, void* param_2, void* param_3, long long param_4)
{
    if (g_conversationActive.load() && param_2 != nullptr) {
        REX::INFO("Mantella: Hooked1419c2290 - patching DialogueResponse voiceFilePath");
        // param_2 is DialogueResponse*, voiceFilePath is BSFixedString at offset 0x38
        // BSFixedString layout: first 8 bytes = pointer to string data
        // Patch it to point to a known Sarah Morgan voice line that has face animations
        if (g_originalBSFixedStringSet != nullptr) {
            void* voiceFilePathPtr = reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(param_2) + 0x38);
            g_originalBSFixedStringSet(voiceFilePathPtr,
                L"Sound\\Voice\\Starfield.esm\\NPCFSarahMorgan\\0093F2BC",
                0);
            REX::INFO("Mantella: Patched voiceFilePath to Sarah Morgan 0093F2BC");
        }
    }
    g_orig1419c2290(param_1, param_2, param_3, param_4);
}

void InstallVoiceHook()
{
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("Starfield.exe"));
    if (!base) {
        REX::ERROR("Mantella: Failed to get Starfield.exe base");
        return;
    }

    SFSE::AllocTrampoline(512);
    auto& trampoline = SFSE::GetTrampoline();

    // Hook 1: FUN_1428cfb80 call at 0xd422b2
    {
        static const std::uintptr_t HOOK_OFFSET = 0xd422b2;
        auto hookAddr = base + HOOK_OFFSET;
        auto firstByte = *reinterpret_cast<std::uint8_t*>(hookAddr);
        REX::INFO("Mantella: Hook1 byte at 0x{:X}: 0x{:02X} (need 0xE8)", hookAddr, firstByte);
        if (firstByte == 0xE8) {
            auto callTarget = *reinterpret_cast<std::int32_t*>(hookAddr + 1);
            auto originalFunc = hookAddr + 5 + callTarget;
            g_originalBSFixedStringSet = reinterpret_cast<BSFixedString_Set_t>(originalFunc);
            trampoline.write_call<5>(hookAddr, &HookedBSFixedStringSet);
            REX::INFO("Mantella: Hook1 installed at 0x{:X}", hookAddr);
        } else {
            REX::ERROR("Mantella: Hook1 wrong byte 0x{:02X}, skipping", firstByte);
        }
    }

    // Hook 2: FUN_140d41330 call at 0xd4221b
    {
        static const std::uintptr_t HOOK2_OFFSET = 0xd4221b;
        auto hook2Addr = base + HOOK2_OFFSET;
        auto firstByte2 = *reinterpret_cast<std::uint8_t*>(hook2Addr);
        REX::INFO("Mantella: Hook2 byte at 0x{:X}: 0x{:02X} (need 0xE8)", hook2Addr, firstByte2);
        if (firstByte2 == 0xE8) {
            auto call2Target = *reinterpret_cast<std::int32_t*>(hook2Addr + 1);
            auto orig2Func = hook2Addr + 5 + call2Target;
            g_orig140d41330 = reinterpret_cast<Func140d41330_t>(orig2Func);
            trampoline.write_call<5>(hook2Addr, &Hooked140d41330);
            REX::INFO("Mantella: Hook2 installed at 0x{:X}", hook2Addr);
        } else {
            REX::ERROR("Mantella: Hook2 wrong byte 0x{:02X}, skipping", firstByte2);
        }
    }

    // Hook 3: Call to FUN_14190f7e0 at 0x40f336 (facial animation loader)
    {
        static const std::uintptr_t HOOK3_OFFSET = 0x40f336;
        auto hook3Addr = base + HOOK3_OFFSET;
        auto firstByte3 = *reinterpret_cast<std::uint8_t*>(hook3Addr);
        REX::INFO("Mantella: Hook3 byte at 0x{:X}: 0x{:02X} (need 0xE8)", hook3Addr, firstByte3);
        if (firstByte3 == 0xE8) {
            auto call3Target = *reinterpret_cast<std::int32_t*>(hook3Addr + 1);
            auto orig3Func = hook3Addr + 5 + call3Target;
            g_origFaceAnimFunc = reinterpret_cast<FaceAnimFunc_t>(orig3Func);
            trampoline.write_call<5>(hook3Addr, &HookedFaceAnimFunc);
            REX::INFO("Mantella: Hook3 installed at 0x{:X}", hook3Addr);
        } else {
            REX::ERROR("Mantella: Hook3 wrong byte 0x{:02X}, skipping", firstByte3);
        }
    }

    // Hooks 3b-3f: All other call sites of FUN_14190f7e0
    static const std::uintptr_t HOOK3_OFFSETS[] = {
        0x1910777, 0x19142a2, 0x19416fc, 0x19c2345, 0x1aea3ff
    };
    for (auto offset : HOOK3_OFFSETS) {
        auto addr = base + offset;
        auto firstByte = *reinterpret_cast<std::uint8_t*>(addr);
        if (firstByte == 0xE8) {
            trampoline.write_call<5>(addr, &HookedFaceAnimFunc);
            REX::INFO("Mantella: FaceAnim hook installed at offset 0x{:X}", offset);
        } else {
            REX::ERROR("Mantella: FaceAnim hook wrong byte at offset 0x{:X}: 0x{:02X}", offset, firstByte);
        }
    }

    // Hook 4: Call to FUN_1419c2290 at 0x19dd540
    // Patches DialogueResponse voiceFilePath before face anim loads
    {
        static const std::uintptr_t HOOK4_OFFSET = 0x19dd540;
        auto hook4Addr = base + HOOK4_OFFSET;
        auto firstByte4 = *reinterpret_cast<std::uint8_t*>(hook4Addr);
        REX::INFO("Mantella: Hook4 byte at 0x{:X}: 0x{:02X} (need 0xE8)", hook4Addr, firstByte4);
        if (firstByte4 == 0xE8) {
            auto call4Target = *reinterpret_cast<std::int32_t*>(hook4Addr + 1);
            auto orig4Func = hook4Addr + 5 + call4Target;
            g_orig1419c2290 = reinterpret_cast<FUN_1419c2290_t>(orig4Func);
            trampoline.write_call<5>(hook4Addr, &Hooked1419c2290);
            REX::INFO("Mantella: Hook4 installed at 0x{:X}", hook4Addr);
        } else {
            REX::ERROR("Mantella: Hook4 wrong byte 0x{:02X}, skipping", firstByte4);
        }
    }
}

std::string ReadIniValue(const char* section, const char* key, const char* fallback = "")
{
    char buffer[1024]{};
    GetPrivateProfileStringA(section, key, fallback, buffer, sizeof(buffer), BRIDGE_INI);
    return std::string(buffer);
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

float GetLineDuration(const std::string& jsonResponse)
{
    std::string key = "\"mantella_actor_duration\":";
    size_t pos = jsonResponse.find(key);
    if (pos == std::string::npos) return 2.0f;
    pos += key.length();
    size_t end = jsonResponse.find_first_of(",}", pos);
    if (end == std::string::npos) return 2.0f;
    try {
        return std::stof(jsonResponse.substr(pos, end - pos));
    }
    catch (...) {
        return 2.0f;
    }
}

std::string SendToMantella(const std::string& jsonBody)
{
    HINTERNET hSession = WinHttpOpen(
        L"MantellaStarfield/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) { REX::ERROR("WinHttp: Failed to open session"); return ""; }

    HINTERNET hConnect = WinHttpConnect(hSession, L"localhost", 4999, 0);
    if (!hConnect) { REX::ERROR("WinHttp: Failed to connect"); WinHttpCloseHandle(hSession); return ""; }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", L"/mantella", nullptr,
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
    int hour = 12;
    try { hour = std::stoi(hourStr); } catch (...) {}
    float gamedays = 1.0f;
    try { gamedays = std::stof(gamedaysStr); } catch (...) {}
    std::string safeLocation = EscapeJson(location);
    std::ostringstream json;
    json << "{"
        << "\"mantella_request_type\":\"mantella_start_conversation\","
        << "\"mantella_worldid\":\"starfield\","
        << "\"mantella_actors\":["
        << "{"
        << "\"mantella_actor_baseid\":0,"
        << "\"mantella_actor_refid\":0,"
        << "\"mantella_actor_name\":\"" << safeSpeaker << "\","
        << "\"mantella_actor_gender\":0,"
        << "\"mantella_actor_race\":\"HumanRace\","
        << "\"mantella_actor_voicetype\":\"Unknown\","
        << "\"mantella_actor_is_in_combat\":false,"
        << "\"mantella_actor_is_enemy\":false,"
        << "\"mantella_actor_relationshiprank\":0,"
        << "\"mantella_actor_is_player\":false"
        << "},"
        << "{"
        << "\"mantella_actor_baseid\":1,"
        << "\"mantella_actor_refid\":1,"
        << "\"mantella_actor_name\":\"Player\","
        << "\"mantella_actor_gender\":0,"
        << "\"mantella_actor_race\":\"HumanRace\","
        << "\"mantella_actor_voicetype\":\"Unknown\","
        << "\"mantella_actor_is_in_combat\":false,"
        << "\"mantella_actor_is_enemy\":false,"
        << "\"mantella_actor_relationshiprank\":4,"
        << "\"mantella_actor_is_player\":true"
        << "}"
        << "],"
        << "\"mantella_context\":{"
        << "\"mantella_location\":\"" << safeLocation << "\","
        << "\"mantella_time\":" << hour << ","
        << "\"mantella_gamedays\":" << gamedays << ","
        << "\"mantella_weather\":{}"
        << "}"
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
    json << "{"
        << "\"mantella_request_type\":\"mantella_player_input\","
        << "\"mantella_player_input\":\"" << safeInput << "\","
        << "\"mantella_context\":{}"
        << "}";
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
    WritePrivateProfileStringA("speaking", "active", "1", BRIDGE_INI);
    REX::INFO("Mantella: Wrote speaking state, duration={}s", duration);
}

void RunConversationLoop(const std::string& startNonce)
{
    std::string lastNonce = startNonce;
    REX::INFO("Mantella: Getting NPC greeting");
    std::string response = SendToMantella(BuildContinueJson());
    std::string replyType = GetReplyType(response);
    REX::INFO("Mantella: Initial reply type: {}", replyType);

    while (g_conversationActive) {
        if (replyType == "mantella_npc_talk") {
            float duration = GetLineDuration(response);
            int waitMs = static_cast<int>(duration * 1000.0f) + 500;
            REX::INFO("Mantella: NPC talking, duration={}s, waiting {}ms", duration, waitMs);
            g_mantellaSpeaking.store(true);
            WriteSpeakingState(duration);
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
            g_mantellaSpeaking.store(false);
            response = SendToMantella(BuildContinueJson());
            replyType = GetReplyType(response);
            REX::INFO("Mantella: Reply type: {}", replyType);
        }
        else if (replyType == "mantella_player_talk") {
            REX::INFO("Mantella: Awaiting player input");
            g_mantellaSpeaking.store(false);
            WriteAwaitInput(lastNonce);
            while (g_conversationActive) {
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
                    response = SendToMantella(BuildPlayerInputJson(playerInput));
                    replyType = GetReplyType(response);
                    REX::INFO("Mantella: Reply type after player input: {}", replyType);
                    break;
                }
            }
        }
        else if (replyType == "mantella_end_conversation") {
            REX::INFO("Mantella: Conversation ended by Mantella");
            g_conversationActive = false;
            g_mantellaSpeaking.store(false);
            return;
        }
        else {
            REX::INFO("Mantella: Unknown reply type '{}', waiting and retrying", replyType);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            response = SendToMantella(BuildContinueJson());
            replyType = GetReplyType(response);
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
            }
            else {
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

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    REX::INFO("Mantella Starfield bridge plugin loaded");
    InstallVoiceHook();
    std::thread(BridgeMonitorThread).detach();
    return true;
}