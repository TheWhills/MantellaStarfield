#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#undef ERROR
#include <string>
#include <sstream>
#include <thread>
#include <chrono>

#pragma comment(lib, "winhttp.lib")

void SendToMantella(const std::string& jsonBody)
{
    HINTERNET hSession = WinHttpOpen(
        L"MantellaStarfield/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!hSession) {
        REX::ERROR("WinHttp: Failed to open session");
        return;
    }

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        L"localhost",
        4999,
        0);

    if (!hConnect) {
        REX::ERROR("WinHttp: Failed to connect");
        WinHttpCloseHandle(hSession);
        return;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        L"/mantella",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);

    if (!hRequest) {
        REX::ERROR("WinHttp: Failed to open request");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    std::wstring headers = L"Content-Type: application/json";
    std::string body = jsonBody;

    BOOL result = WinHttpSendRequest(
        hRequest,
        headers.c_str(),
        (DWORD)-1,
        (LPVOID)body.c_str(),
        (DWORD)body.size(),
        (DWORD)body.size(),
        0);

    if (result) {
        WinHttpReceiveResponse(hRequest, nullptr);
        REX::INFO("Mantella: Request sent successfully");
    } else {
        REX::ERROR("Mantella: Failed to send request");
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
}

std::string BuildStartConversationJSON(RE::TESNPC* npc, RE::TESObjectREFR* refr)
{
    if (!npc || !refr) return "";

    const char* name = npc->GetFullName();
    if (!name || strlen(name) == 0) name = "Unknown";

    std::uint32_t baseID = npc->GetFormID();
    std::uint32_t refID  = refr->GetFormID();

    std::string race = "HumanRace";
    if (auto* raceForm = npc->GetRace(); raceForm) {
        if (const char* raceID = raceForm->GetFormEditorID(); raceID && strlen(raceID) > 0) {
            race = raceID;
        }
    }

    int gender = (npc->GetSex() == RE::SEX::kFemale) ? 1 : 0;
    std::string safeName = name;

    std::ostringstream json;
    json << "{"
         << "\"mantella_request_type\":\"mantella_start_conversation\","
         << "\"mantella_worldid\":\"starfield\","
         << "\"mantella_actors\":["
         << "{"
         << "\"mantella_actor_baseid\":" << baseID << ","
         << "\"mantella_actor_refid\":" << refID << ","
         << "\"mantella_actor_name\":\"" << safeName << "\","
         << "\"mantella_actor_gender\":" << gender << ","
         << "\"mantella_actor_race\":\"" << race << "\","
         << "\"mantella_actor_voicetype\":\"Unknown\","
         << "\"mantella_actor_is_in_combat\":false,"
         << "\"mantella_actor_is_enemy\":false,"
         << "\"mantella_actor_relationshiprank\":0,"
         << "\"mantella_actor_is_player\":false"
         << "}"
         << "],"
         << "\"mantella_context\":{"
         << "\"mantella_location\":\"Starfield\","
         << "\"mantella_time\":12,"
         << "\"mantella_gamedays\":1.0,"
         << "\"mantella_weather\":{}"
         << "}"
         << "}";

    return json.str();
}

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    REX::INFO("Mantella Starfield plugin loaded");

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        REX::INFO("Mantella: Starting dialogue monitor thread");

        bool wasDialogueOpen = false;

        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            auto* ui = RE::UI::GetSingleton();
            if (!ui) continue;

            bool isDialogueOpen = ui->IsMenuOpen("DialogueMenu");

            if (isDialogueOpen && !wasDialogueOpen) {
                REX::INFO("Mantella: DialogueMenu opened (polled)");

                auto* topicManager = RE::MenuTopicManager::GetSingleton();
                if (topicManager) {
                    auto* speakerRef = topicManager->speaker.get().get();
                    if (speakerRef) {
                        auto* npc = speakerRef->As<RE::TESNPC>();
                        if (!npc) {
                            auto basePtr = speakerRef->GetBaseObject();
                            auto* base = basePtr.get();
                            if (base) npc = base->As<RE::TESNPC>();
                        }
                        if (npc) {
                            REX::INFO("Mantella: Talking to {}", npc->GetFullName());
                            std::string json = BuildStartConversationJSON(npc, speakerRef);
                            SendToMantella(json);
                        } else {
                            REX::ERROR("Mantella: Could not get TESNPC from speaker");
                        }
                    } else {
                        REX::ERROR("Mantella: Speaker reference is null");
                    }
                } else {
                    REX::ERROR("Mantella: MenuTopicManager not found");
                }

            } else if (!isDialogueOpen && wasDialogueOpen) {
                REX::INFO("Mantella: DialogueMenu closed (polled)");
                SendToMantella("{\"mantella_request_type\":\"mantella_end_conversation\"}");
            }

            wasDialogueOpen = isDialogueOpen;
        }
    }).detach();

    return true;
}