#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winhttp.h>

#undef ERROR

#include <RE/Starfield.h>
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

#pragma comment(lib, "winhttp.lib")

static const char* BRIDGE_DIR = "C:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\";
static const char* BRIDGE_INI = "C:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\Bridge.ini";

static std::atomic<bool> g_conversationActive{ false };
static std::atomic<bool> g_mantellaSpeaking{ false };
static std::atomic<std::uint32_t> g_currentSpeakerFormID{ 0 };
static const SFSE::TaskInterface* g_taskInterface = nullptr;

using BSFixedString_Set_t = void(*)(void* dest, const wchar_t* src, std::uint32_t unk);
static BSFixedString_Set_t g_originalBSFixedStringSet = nullptr;

using FaceAnimFunc_t = char(*)(void*, void*, const char*);
static FaceAnimFunc_t g_origFaceAnimFunc = nullptr;

// DialogueManager::Say
// RCX = DialogueManager*
// RDX = 0
// R8  = 0
// R9  = Actor*
// [rsp+0x20] = TESTopic*
// [rsp+0x28] = 0
// [rsp+0x30] = 0
// [rsp+0x38] = Actor* target / nullptr
// [rsp+0x40] = 0
using DialogueMgrSay_t = void*(*)(void* mgr,
                                  std::uint64_t rdx_zero,
                                  std::uint64_t r8_zero,
                                  void* actor,
                                  void* topic,
                                  std::uint64_t unk28,
                                  std::uint64_t unk30,
                                  void* target,
                                  std::uint64_t unk40);

static DialogueMgrSay_t g_dialogueMgrSay = nullptr;
static void** g_dialogueMgrPtr = nullptr;

void HookedBSFixedStringSet(void* dest, const wchar_t* src, std::uint32_t unk)
{
    g_originalBSFixedStringSet(dest, src, unk);
}

char HookedFaceAnimFunc(void* actor, void* param_2, const char* voicePath)
{
    if (voicePath != nullptr && strlen(voicePath) > 0) {
        char result = g_origFaceAnimFunc(actor, param_2, voicePath);
        REX::INFO("Mantella: FaceAnim {} returned: {}", voicePath, static_cast<int>(result));
        return result;
    }

    return g_origFaceAnimFunc(actor, param_2, voicePath);
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

    if (pos == std::string::npos) {
        return "";
    }

    pos += key.length();

    size_t end = jsonResponse.find("\"", pos);

    if (end == std::string::npos) {
        return "";
    }

    return jsonResponse.substr(pos, end - pos);
}

float GetLineDuration(const std::string& jsonResponse)
{
    std::string key = "\"mantella_actor_duration\":";
    size_t pos = jsonResponse.find(key);

    if (pos == std::string::npos) {
        return 2.0f;
    }

    pos += key.length();

    size_t end = jsonResponse.find_first_of(",}", pos);

    if (end == std::string::npos) {
        return 2.0f;
    }

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
        0
    );

    if (!hSession) {
        REX::ERROR("WinHttp: Failed to open session");
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(hSession, L"localhost", 4999, 0);

    if (!hConnect) {
        REX::ERROR("WinHttp: Failed to connect");
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        L"/mantella",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0
    );

    if (!hRequest) {
        REX::ERROR("WinHttp: Failed to open request");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    std::wstring headers = L"Content-Type: application/json";
    std::string body = jsonBody;

    BOOL result = WinHttpSendRequest(
        hRequest,
        headers.c_str(),
        static_cast<DWORD>(-1),
        (LPVOID)body.c_str(),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0
    );

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
    }
    else {
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

    if (location.empty()) {
        location = "Unknown Location";
    }

    int hour = 12;
    float gamedays = 1.0f;

    try { hour = std::stoi(hourStr); } catch (...) {}
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

    try {
        nonce = std::stoi(lastNonce);
    }
    catch (...) {}

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

void CallActorSay()
{
    auto formID = g_currentSpeakerFormID.load();

    if (formID == 0) {
        REX::ERROR("Mantella: No speaker form ID set for Say");
        return;
    }

    if (g_dialogueMgrSay == nullptr || g_dialogueMgrPtr == nullptr) {
        REX::ERROR("Mantella: DialogueManager::Say not initialized");
        return;
    }

    if (g_taskInterface == nullptr) {
        REX::ERROR("Mantella: No task interface available");
        return;
    }

    REX::INFO("Mantella: Queuing Say task for actor 0x{:X}", formID);

    g_taskInterface->AddTask([formID]() {
        // Double-check pointer validation inside the game's main thread execution loop
        if (!g_dialogueMgrPtr || !g_dialogueMgrSay) {
            return;
        }

        void* mgr = *g_dialogueMgrPtr;
        if (!mgr) {
            REX::ERROR("Mantella: DialogueManager singleton is null (Player likely loading or in main menu)");
            return;
        }

        auto* actorForm = RE::TESForm::LookupByID(formID);
        if (!actorForm) {
            REX::ERROR("Mantella: Could not find actor form 0x{:X}", formID);
            return;
        }

        RE::TESForm* topicForm = nullptr;
        for (std::uint32_t modIdx = 1; modIdx < 0xFF; modIdx++) {
            std::uint32_t questID = (modIdx << 24) | 0x00000807;
            auto* qForm = RE::TESForm::LookupByID(questID);

            if (qForm) {
                std::uint32_t topicID = (modIdx << 24) | 0x00000826;
                topicForm = RE::TESForm::LookupByID(topicID);

                REX::INFO(
                    "Mantella: Found mod at index 0x{:X}, topic runtime ID 0x{:X}",
                    modIdx,
                    topicID
                );
                break;
            }
        }

        if (!topicForm) {
            REX::ERROR("Mantella: Could not find MantellaDialogueTopic - is MantellaStarfield.esp loaded?");
            return;
        }

        REX::INFO("Mantella: Calling DialogueManager::Say on actor 0x{:X}", formID);

        // Safely invoke the native engine Say layout
        void* result = g_dialogueMgrSay(
            mgr,
            0,
            0,
            actorForm,
            topicForm,
            0,
            0,
            nullptr,
            0
        );

        REX::INFO(
            "Mantella: DialogueManager::Say returned: 0x{:X}",
            reinterpret_cast<std::uintptr_t>(result)
        );
    });
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
            CallActorSay();

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

                if (currentNonce == lastNonce) {
                    continue;
                }

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

        if (!std::filesystem::exists(BRIDGE_INI)) {
            continue;
        }

        std::string nonce = ReadIniValue("request", "nonce", "");

        if (nonce.empty() || nonce == lastNonce) {
            continue;
        }

        lastNonce = nonce;

        std::string type = ReadIniValue("request", "type", "");
        std::string speaker = ReadIniValue("request", "speaker", "Unknown");

        std::transform(type.begin(), type.end(), type.begin(), ::tolower);

        REX::INFO(
            "Mantella: Bridge request detected. type={}, speaker={}, nonce={}",
            type,
            speaker,
            nonce
        );

        std::string refidStr = ReadIniValue("request", "speaker_refid", "0");

        try {
            g_currentSpeakerFormID.store(static_cast<std::uint32_t>(std::stoul(refidStr)));
            REX::INFO("Mantella: Speaker form ID set to 0x{:X}", g_currentSpeakerFormID.load());
        }
        catch (...) {
            g_currentSpeakerFormID.store(0);
        }

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

void InstallVoiceHook()
{
    REX::INFO("Mantella: InstallVoiceHook called");

    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("Starfield.exe"));

    REX::INFO("Mantella: Base address: 0x{:X}", base);

    if (!base) {
        REX::ERROR("Mantella: Failed to get Starfield.exe base address");
        return;
    }

    g_dialogueMgrSay = reinterpret_cast<DialogueMgrSay_t>(base + 0xEEEC60);
    g_dialogueMgrPtr = reinterpret_cast<void**>(base + 0x61E76E0);

    REX::INFO("Mantella: DialogueManager::Say at 0x{:X}", base + 0xEEEC60);
    REX::INFO("Mantella: DialogueManager singleton ptr at 0x{:X}", base + 0x61E76E0);

    SFSE::AllocTrampoline(128);
    auto& trampoline = SFSE::GetTrampoline();

    {
        static const std::uintptr_t HOOK_OFFSET = 0xD42472;
        auto hookAddr = base + HOOK_OFFSET;
        auto firstByte = *reinterpret_cast<std::uint8_t*>(hookAddr);

        if (firstByte == 0xE8) {
            auto callTarget = *reinterpret_cast<std::int32_t*>(hookAddr + 1);
            auto originalFunc = hookAddr + 5 + callTarget;

            g_originalBSFixedStringSet = reinterpret_cast<BSFixedString_Set_t>(originalFunc);

            trampoline.write_call<5>(hookAddr, &HookedBSFixedStringSet);

            REX::INFO("Mantella: Hook1 installed at 0x{:X}", hookAddr);
        }
        else {
            REX::ERROR("Mantella: Hook1 wrong byte 0x{:02X}", firstByte);
        }
    }

    {
        static const std::uintptr_t HOOK3_OFFSET = 0x40F216;
        auto hook3Addr = base + HOOK3_OFFSET;
        auto firstByte3 = *reinterpret_cast<std::uint8_t*>(hook3Addr);

        if (firstByte3 == 0xE8) {
            auto call3Target = *reinterpret_cast<std::int32_t*>(hook3Addr + 1);
            auto orig3Func = hook3Addr + 5 + call3Target;

            g_origFaceAnimFunc = reinterpret_cast<FaceAnimFunc_t>(orig3Func);

            trampoline.write_call<5>(hook3Addr, &HookedFaceAnimFunc);

            REX::INFO("Mantella: Hook3 installed at 0x{:X}", hook3Addr);
        }
        else {
            REX::ERROR("Mantella: Hook3 wrong byte 0x{:02X}", firstByte3);
        }
    }

    static const std::uintptr_t HOOK3_OFFSETS[] = {
        0x1910A07,
        0x1914532,
        0x194198C,
        0x19C25D5,
        0x1AEA68F
    };

    for (auto offset : HOOK3_OFFSETS) {
        auto addr = base + offset;
        auto firstByte = *reinterpret_cast<std::uint8_t*>(addr);

        if (firstByte == 0xE8) {
            trampoline.write_call<5>(addr, &HookedFaceAnimFunc);
            REX::INFO("Mantella: FaceAnim hook installed at offset 0x{:X}", offset);
        }
        else {
            REX::ERROR(
                "Mantella: FaceAnim hook wrong byte at offset 0x{:X}: 0x{:02X}",
                offset,
                firstByte
            );
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

    if (g_taskInterface) {
        REX::INFO("Mantella: Task interface acquired");
    }
    else {
        REX::ERROR("Mantella: Failed to get task interface");
    }

    REX::INFO("Mantella Starfield bridge plugin loaded");

    InstallVoiceHook();

    std::thread(BridgeMonitorThread).detach();

    return true;
}