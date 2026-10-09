// -----------------------------------------------------------------------------
// UnityComm.cpp - UNITY Starfield plugin transport.
//
// Adapted from STOBE's Comm.cpp (Dwemer Dynamics). Renames applied:
//   stobe -> unity, kenshi -> starfield, 8083 -> 8087, /StobeServer/ -> /UNITYServer/
// Kenshi-coupled bits removed (BoredEventPollThread, message-queue globals,
// speech-delivery/CSV helpers that the milestone doesn't need). Logging routed
// to CommonLibSF's REX. Depends only on winhttp + <RE/.. > for REX macros.
//
// If your project's precompiled header already pulls in windows.h / REX, you can
// trim the includes below accordingly.
// -----------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "UnityComm.h"

#include <windows.h>
#include <winhttp.h>

#undef ERROR  // REX::ERROR vs the windows.h ERROR macro

#include <RE/Starfield.h>   // for REX::INFO / REX::ERROR (CommonLibSF)
#include <SFSE/SFSE.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------------------
// Small logging shims so the ported STOBE code's Log("...") style still reads
// naturally, mapping onto REX. STOBE used a single Log(std::string); we split
// into info/error at call sites where it matters.
// ---------------------------------------------------------------------------
static void ULog(const std::string& msg)  { REX::INFO("UnityComm: {}", msg); }
static void UErr(const std::string& msg)  { REX::ERROR("UnityComm: {}", msg); }

static std::string IntToStr(long long v) { return std::to_string(v); }

namespace {

static const INTERNET_PORT kDefaultServerPort = 8087;   // was 8083
static const INTERNET_PORT kDiscoveryPort     = 7135;
const wchar_t* kDefaultServerHost = L"127.0.0.1";

std::wstring    g_unityHost = kDefaultServerHost;
INTERNET_PORT   g_unityPort = kDefaultServerPort;
LONG            g_discoveryDone = 0;            // interlocked 0/1
LONG            g_dwemerDistroConnected = 0;    // interlocked 0/1
DWORD           g_dwemerDistroLastSuccessTick = 0;

struct RequestPlan {
  std::wstring method;
  std::wstring path;
  std::string  body;
  bool         isStub;
  std::string  stubResponse;
};

std::string Trim(const std::string& v) {
  if (v.empty()) return "";
  size_t s = v.find_first_not_of(" \t\r\n");
  if (s == std::string::npos) return "";
  size_t e = v.find_last_not_of(" \t\r\n");
  return v.substr(s, e - s + 1);
}

std::string ToUtf8(const std::wstring& v) {
  if (v.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, v.c_str(), (int)v.size(), NULL, 0, NULL, NULL);
  if (n <= 0) return "";
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, v.c_str(), (int)v.size(), &out[0], n, NULL, NULL);
  return out;
}

bool StartsWith(const std::wstring& v, const std::wstring& p) {
  if (v.size() < p.size()) return false;
  return v.compare(0, p.size(), p) == 0;
}

// --- HTTP response readers -------------------------------------------------
std::string ReadHttpResponse(HINTERNET hRequest) {
  std::string body;
  DWORD dwSize = 0;
  do {
    if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
    if (dwSize == 0) break;
    std::vector<char> buf(dwSize + 1, 0);
    DWORD got = 0;
    if (!WinHttpReadData(hRequest, buf.data(), dwSize, &got)) break;
    body.append(buf.data(), got);
  } while (dwSize > 0);
  return body;
}

bool ReadHttpResponseWithLineCallback(HINTERNET hRequest, std::string* out,
                                      UnityStreamLineCallback cb, void* user) {
  std::string body, lineBuf;
  bool readOk = true, cont = true;
  DWORD dwSize = 0;
  do {
    if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) { readOk = false; break; }
    if (dwSize == 0) break;
    std::vector<char> buf(dwSize + 1, 0);
    DWORD got = 0;
    if (!WinHttpReadData(hRequest, buf.data(), dwSize, &got)) { readOk = false; break; }
    if (got == 0) continue;
    std::string chunk(buf.data(), got);
    body.append(chunk);
    if (cb) {
      lineBuf.append(chunk);
      while (cont) {
        size_t nl = lineBuf.find('\n');
        if (nl == std::string::npos) break;
        std::string line = lineBuf.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lineBuf.erase(0, nl + 1);
        cont = cb(line, user);
      }
      if (!cont) break;
    }
  } while (dwSize > 0);
  if (cb && cont && !lineBuf.empty()) {
    if (lineBuf.back() == '\r') lineBuf.pop_back();
    cont = cb(lineBuf, user);
  }
  if (out) *out = body;
  return readOk && cont;
}

bool TryParseHostPort(const std::string& raw, std::wstring& host, INTERNET_PORT& port) {
  std::string v = Trim(raw);
  if (v.empty()) return false;
  size_t colon = v.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon == v.size() - 1) return false;
  std::string h = Trim(v.substr(0, colon));
  std::string p = Trim(v.substr(colon + 1));
  if (h.empty() || p.empty()) return false;
  int parsed = atoi(p.c_str());
  if (parsed <= 0 || parsed > 65535) return false;
  host = ToWide(h);
  port = (INTERNET_PORT)parsed;
  return true;
}

// --- Raw HTTP send ---------------------------------------------------------
bool SendRawHttp(const RequestPlan& req, bool expectResponse, std::string* out,
                 UnityStreamLineCallback lineCb, void* lineUser) {
  HINTERNET hSession = WinHttpOpen(L"Unity/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    UErr("Failed to create WinHTTP session.");
    return false;
  }
  int timeoutMs = expectResponse ? (lineCb ? 120000 : 60000) : 8000;
  WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

  HINTERNET hConnect = WinHttpConnect(hSession, g_unityHost.c_str(), g_unityPort, 0);
  if (!hConnect) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    UErr("Failed to connect to resolved UNITY server.");
    WinHttpCloseHandle(hSession);
    return false;
  }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, req.method.c_str(), req.path.c_str(),
                                          NULL, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!hRequest) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    UErr("Failed to create HTTP request handle.");
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  BOOL sendOk;
  if (req.method == L"POST") {
    sendOk = WinHttpSendRequest(hRequest, L"Content-Type: application/json\r\n",
                                (DWORD)-1L, (LPVOID)req.body.c_str(),
                                (DWORD)req.body.length(), (DWORD)req.body.length(), 0);
  } else {
    sendOk = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  }

  bool success = false;
  if (sendOk && WinHttpReceiveResponse(hRequest, NULL)) {
    if (expectResponse) {
      if (lineCb) {
        success = ReadHttpResponseWithLineCallback(hRequest, out, lineCb, lineUser);
      } else if (out) {
        *out = ReadHttpResponse(hRequest);
        success = true;
      } else {
        (void)ReadHttpResponse(hRequest);
        success = true;
      }
    } else {
      success = true;
    }
  }

  if (!success) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    UErr("HTTP request failed with error " + IntToStr((long long)GetLastError()));
  } else {
    g_dwemerDistroLastSuccessTick = GetTickCount();
    InterlockedExchange(&g_dwemerDistroConnected, 1);
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return success;
}

// --- Discovery -------------------------------------------------------------
void DiscoverUnityServer() {
  HINTERNET hSession = WinHttpOpen(L"Unity/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    ULog("DISCOVERY: WinHTTP session failed, using fallback 127.0.0.1:8087.");
    return;
  }
  WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);

  HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", kDiscoveryPort, 0);
  if (!hConnect) {
    ULog("DISCOVERY: Could not connect to launcher discovery, using fallback.");
    WinHttpCloseHandle(hSession);
    return;
  }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
                                          L"/discover?game=starfield", NULL,
                                          WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!hRequest) {
    ULog("DISCOVERY: Could not create discovery request, using fallback.");
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return;
  }

  bool ok = false;
  std::string response;
  if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(hRequest, NULL)) {
    response = ReadHttpResponse(hRequest);
    ok = true;
  }

  if (ok) {
    std::wstring host;
    INTERNET_PORT port = kDefaultServerPort;
    if (TryParseHostPort(response, host, port)) {
      g_unityHost = host;
      g_unityPort = port;
      ULog("DISCOVERY: Resolved UNITY server to " + ToUtf8(g_unityHost) + ":" +
           IntToStr((long long)g_unityPort));
    } else {
      ULog("DISCOVERY: Invalid response '" + Trim(response) +
           "', using fallback 127.0.0.1:8087.");
    }
  } else {
    ULog("DISCOVERY: Request failed, using fallback 127.0.0.1:8087.");
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
}

// --- Endpoint routing ------------------------------------------------------
// Maps friendly endpoints to UNITYServer PHP paths. For the milestone you can
// just pass a raw L"/UNITYServer/stream.php?DATA=..." path and it flows through
// the StartsWith(L"/UNITYServer/") branch untouched.
RequestPlan ResolveRequest(const std::wstring& endpoint, const std::string& json) {
  RequestPlan r;
  r.method = L"POST";
  r.path = endpoint;
  r.body = json;
  r.isStub = false;
  r.stubResponse = "{\"ok\":false,\"error\":\"unsupported_endpoint\"}";

  if (endpoint == L"/chat") { r.path = L"/UNITYServer/chat.php"; return r; }
  if (endpoint == L"/context") {
    r.path = L"/UNITYServer/npc_snapshot.php"; return r;
  }
  if (endpoint == L"/gamedata")   { r.path = L"/UNITYServer/gamedata.php";   return r; }
  if (endpoint == L"/world_state"){ r.path = L"/UNITYServer/world_state.php";return r; }

  // Raw path passthrough (GET if no body). This is the milestone path:
  //   L"/UNITYServer/stream.php?DATA=<...>"
  if (StartsWith(endpoint, L"/UNITYServer/")) {
    if (json.empty()) { r.method = L"GET"; r.body.clear(); }
    return r;
  }

  r.isStub = true;
  return r;
}

// --- Async plumbing --------------------------------------------------------
struct HttpTask { std::wstring endpoint; std::string data; };

DWORD WINAPI AsyncHttpThread(LPVOID p) {
  HttpTask* t = (HttpTask*)p;
  PostToUnity(t->endpoint, t->data);
  delete t;
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::wstring ToWide(const std::string& v) {
  return std::wstring(v.begin(), v.end());
}

std::string UrlEncode(const std::string& input) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(input.size() * 3);
  for (size_t i = 0; i < input.size(); ++i) {
    unsigned char c = (unsigned char)input[i];
    bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) { out.push_back((char)c); continue; }
    out.push_back('%');
    out.push_back(hex[(c >> 4) & 0x0F]);
    out.push_back(hex[c & 0x0F]);
  }
  return out;
}

std::string Base64Encode(const std::string& input) {
  static const char t[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < input.size()) {
    unsigned int c = ((unsigned char)input[i] << 16) |
                     ((unsigned char)input[i + 1] << 8) |
                     ((unsigned char)input[i + 2]);
    out.push_back(t[(c >> 18) & 0x3F]);
    out.push_back(t[(c >> 12) & 0x3F]);
    out.push_back(t[(c >> 6) & 0x3F]);
    out.push_back(t[c & 0x3F]);
    i += 3;
  }
  if (i < input.size()) {
    unsigned int c = (unsigned char)input[i] << 16;
    out.push_back(t[(c >> 18) & 0x3F]);
    if (i + 1 < input.size()) {
      c |= (unsigned char)input[i + 1] << 8;
      out.push_back(t[(c >> 12) & 0x3F]);
      out.push_back(t[(c >> 6) & 0x3F]);
      out.push_back('=');
    } else {
      out.push_back(t[(c >> 12) & 0x3F]);
      out.push_back('=');
      out.push_back('=');
    }
  }
  return out;
}

std::string BuildStreamQueryData(const std::string& eventType,
                                 const std::string& eventData,
                                 long long gamets) {
  if (gamets < 0) gamets = 0;
  std::string packet = eventType + "|" + IntToStr((long long)time(NULL)) + "|" +
                       IntToStr(gamets) + "|" + eventData;
  return UrlEncode(Base64Encode(packet));
}

void EnsureDiscovered() {
  if (InterlockedCompareExchange(&g_discoveryDone, 1, 0) == 0) {
    DiscoverUnityServer();
  }
}

void PostToUnity(const std::wstring& endpoint, const std::string& json) {
  EnsureDiscovered();
  RequestPlan req = ResolveRequest(endpoint, json);
  if (req.isStub) {
    ULog("Skipping unsupported async endpoint " + ToUtf8(endpoint));
    return;
  }
  std::string ignored;
  SendRawHttp(req, false, &ignored, NULL, NULL);
}

std::string PostToUnityWithResponse(const std::wstring& endpoint, const std::string& json) {
  EnsureDiscovered();
  RequestPlan req = ResolveRequest(endpoint, json);
  if (req.isStub) {
    ULog("Returning stub for endpoint " + ToUtf8(endpoint));
    return req.stubResponse;
  }
  std::string body;
  if (!SendRawHttp(req, true, &body, NULL, NULL)) return req.stubResponse;
  return body;
}

bool PostToUnityWithResponseStream(const std::wstring& endpoint, const std::string& json,
                                   UnityStreamLineCallback cb, void* user) {
  EnsureDiscovered();
  RequestPlan req = ResolveRequest(endpoint, json);
  if (req.isStub) {
    if (cb && !req.stubResponse.empty()) cb(req.stubResponse, user);
    return true;
  }
  std::string ignored;
  return SendRawHttp(req, true, &ignored, cb, user);
}

void AsyncPostToUnity(const std::wstring& endpoint, const std::string& json) {
  HttpTask* t = new HttpTask();
  t->endpoint = endpoint;
  t->data = json;
  CreateThread(NULL, 0, AsyncHttpThread, t, 0, NULL);
}

void AsyncPostToUnitySerial(const std::wstring& endpoint, const std::string& json) {
  // Milestone: serial == async. If ordered delivery matters later, port STOBE's
  // SerialHttpWorkerThread queue here.
  AsyncPostToUnity(endpoint, json);
}

// --- Event helper ----------------------------------------------------------
std::string SendUnityEvent(const std::string& eventType, long long gamets,
                           const std::string& eventData) {
  std::wstring path = L"/UNITYServer/stream.php?DATA=" +
                      ToWide(BuildStreamQueryData(eventType, eventData, gamets));
  return PostToUnityWithResponse(path, "");  // empty body -> GET
}

namespace {
struct EventTask { std::string type; long long gamets; std::string data; };
DWORD WINAPI EventThread(LPVOID p) {
  EventTask* t = (EventTask*)p;
  std::string resp = SendUnityEvent(t->type, t->gamets, t->data);
  REX::INFO("UnityComm: event '{}' -> server replied '{}'", t->type, resp);
  delete t;
  return 0;
}
} // namespace

void SendUnityEventAsync(const std::string& eventType, long long gamets,
                         const std::string& eventData) {
  EventTask* t = new EventTask();
  t->type = eventType;
  t->gamets = gamets;
  t->data = eventData;
  CreateThread(NULL, 0, EventThread, t, 0, NULL);
}

std::string GetUnityServerHomeUrl() {
  EnsureDiscovered();
  std::string host = ToUtf8(g_unityHost);
  if (host.empty()) host = "127.0.0.1";
  return "http://" + host + ":" + IntToStr((long long)g_unityPort) + "/UNITYServer/";
}

// --- Speech-to-text upload ---------------------------------------------
std::string PostAudioForTranscription(const std::string& wavBytes) {
  EnsureDiscovered();

  if (wavBytes.empty()) {
    UErr("PostAudioForTranscription: empty WAV data, refusing to upload.");
    return "";
  }

  // Manual multipart/form-data body, matching stt.php's $_FILES['file']
  // expectation (single field, filename/content-type set so PHP's file
  // upload handling recognizes it correctly).
  std::string boundary = "----UnityVoiceBoundary" + IntToStr((long long)GetTickCount());
  std::string body;
  body += "--" + boundary + "\r\n";
  body += "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n";
  body += "Content-Type: audio/wav\r\n";
  body += "Content-Transfer-Encoding: binary\r\n\r\n";
  body += wavBytes;
  body += "\r\n--" + boundary + "--\r\n";

  HINTERNET hSession = WinHttpOpen(L"Unity/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    UErr("PostAudioForTranscription: Failed to create WinHTTP session.");
    return "";
  }
  // STT round-trip (upload + OpenAI transcription) can take a while -
  // matching the 60s timeout the reference PHP implementation itself
  // uses for the OpenAI call.
  WinHttpSetTimeouts(hSession, 60000, 60000, 60000, 60000);

  HINTERNET hConnect = WinHttpConnect(hSession, g_unityHost.c_str(), g_unityPort, 0);
  if (!hConnect) {
    UErr("PostAudioForTranscription: Failed to connect to resolved UNITY server.");
    WinHttpCloseHandle(hSession);
    return "";
  }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/UNITYServer/stt.php",
                                          NULL, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!hRequest) {
    UErr("PostAudioForTranscription: Failed to create HTTP request handle.");
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return "";
  }

  std::wstring header = L"Content-Type: multipart/form-data; boundary=" + ToWide(boundary) + L"\r\n";
  BOOL sendOk = WinHttpSendRequest(hRequest, header.c_str(), (DWORD)-1L,
                                   (LPVOID)body.data(), (DWORD)body.size(),
                                   (DWORD)body.size(), 0);

  std::string responseText;
  bool success = false;
  if (sendOk && WinHttpReceiveResponse(hRequest, NULL)) {
    responseText = ReadHttpResponse(hRequest);
    success = true;
  }

  if (!success) {
    UErr("PostAudioForTranscription: HTTP request failed with error " +
         IntToStr((long long)GetLastError()));
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);

  if (!success) {
    return "";
  }

  // Response body IS the transcription (plain text, per stt.php's
  // contract) - no parsing needed, just trim whitespace/newlines. BOTH
  // ends, not just trailing - confirmed via a real in-game test that
  // OpenAI's response can carry a LEADING newline (logged transcription
  // was literally "'\nCan you hear me?'"). That leading newline, left
  // untrimmed, corrupts the Bridge.ini [voice] "text" entry once written
  // via WritePrivateProfileStringA: the classic Win32 INI-writing API
  // doesn't handle an embedded newline inside a value correctly, so the
  // entry gets truncated/malformed on write, and Papyrus reads back
  // something that fails its "voiceText != ''" check even when every
  // other condition (ConversationActive/WaitingForInput/!IsAccumulating)
  // is correctly true - exactly the symptom observed (diagnostic showed
  // active=True waiting=True accumulating=False, yet nothing was sent).
  size_t start = 0;
  while (start < responseText.size() &&
         (responseText[start] == '\n' || responseText[start] == '\r' ||
          responseText[start] == ' ')) {
    start++;
  }
  responseText.erase(0, start);
  while (!responseText.empty() &&
         (responseText.back() == '\n' || responseText.back() == '\r' ||
          responseText.back() == ' ')) {
    responseText.pop_back();
  }
  return responseText;
}

bool IsDwemerDistroConnected() {
  return InterlockedCompareExchange(&g_dwemerDistroConnected, 0, 0) != 0;
}

DWORD GetDwemerDistroLastSuccessAgeMs() {
  DWORD last = g_dwemerDistroLastSuccessTick;
  if (last == 0) return 0xFFFFFFFF;
  DWORD now = GetTickCount();
  return (now >= last) ? (now - last) : 0;
}