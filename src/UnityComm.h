#pragma once
#include <string>
#include <vector>
#include <windows.h>

// -----------------------------------------------------------------------------
// UnityComm - transport layer for the UNITY Starfield plugin.
//
// Adapted from STOBE's Comm.cpp/.h (Dwemer Dynamics). Talks to UNITYServer
// running in the DwemerDistro WSL environment, via the launcher's discovery
// service on 127.0.0.1:7135.
//
// Flow:
//   1. EnsureDiscovered() GETs 127.0.0.1:7135/discover?game=starfield
//      -> parses "<wsl-ip>:8087", caches it (falls back to 127.0.0.1:8087).
//   2. Event posts hit /UNITYServer/stream.php?DATA=<urlencode(base64(packet))>
//      where packet = "eventType|unixtime|gamets|eventData".
//   3. main.php explode("|")s that and dispatches on eventType.
//
// This file has NO dependency on any Kenshi/Starfield engine header. It uses
// CommonLibSF's REX logging (REX::INFO / REX::ERROR) and winhttp only.
// Link winhttp.lib (already in the main plugin's #pragma comment).
// -----------------------------------------------------------------------------

// Streaming line callback: return false to stop consuming the stream.
typedef bool (*UnityStreamLineCallback)(const std::string& line, void* userData);

// --- Discovery -------------------------------------------------------------
// Resolve the UNITYServer host:port once (idempotent, thread-safe). Optional to
// call directly; every Post* below calls it lazily.
void EnsureDiscovered();

// Current resolved base URL, e.g. "http://172.25.224.225:8087/UNITYServer/".
std::string GetUnityServerHomeUrl();

// --- Core posts ------------------------------------------------------------
// Fire-and-forget POST (8s timeout, no response read). endpoint is a friendly
// path like L"/chat" or a raw L"/UNITYServer/....php" (see ResolveRequest).
void PostToUnity(const std::wstring& endpoint, const std::string& jsonData);

// POST and return the full response body (60s timeout).
std::string PostToUnityWithResponse(const std::wstring& endpoint,
                                    const std::string& jsonData);

// POST and stream the response line-by-line into callback (120s timeout).
bool PostToUnityWithResponseStream(const std::wstring& endpoint,
                                   const std::string& jsonData,
                                   UnityStreamLineCallback callback,
                                   void* userData);

// Async variants (spawn a detached worker thread).
void AsyncPostToUnity(const std::wstring& endpoint, const std::string& jsonData);
void AsyncPostToUnitySerial(const std::wstring& endpoint,
                            const std::string& jsonData);

// --- Event helper (the connectivity-milestone entry point) -----------------
// Builds "eventType|time|gamets|eventData", base64+urlencodes it, and GETs
// /UNITYServer/stream.php?DATA=... Returns the server body (e.g. "ok").
// Use eventType "action" for a no-LLM eventlog write (routes to infoaction.php).
std::string SendUnityEvent(const std::string& eventType,
                           long long gamets,
                           const std::string& eventData);

// Fire the same event asynchronously (detached thread; ignores response).
void SendUnityEventAsync(const std::string& eventType,
                         long long gamets,
                         const std::string& eventData);

// --- Speech-to-text upload ---------------------------------------------
// POSTs raw WAV bytes as multipart/form-data (field name "file", matching
// UNITYServer's stt.php contract) to /UNITYServer/stt.php. Returns the
// transcribed text as a plain string, or empty on any failure - stt.php's
// response body IS the transcription (or empty), not JSON, so there's no
// parsing needed on this side. Separate from PostToUnityWithResponse
// because that function hardcodes Content-Type: application/json for
// every POST, which a multipart file upload can't use.
std::string PostAudioForTranscription(const std::string& wavBytes);

// --- Packet building (exposed for reuse) -----------------------------------
std::wstring ToWide(const std::string& value);
std::string  UrlEncode(const std::string& input);
std::string  Base64Encode(const std::string& input);
// packet = eventType|time(now)|gamets|eventData  (then base64 + urlencode)
std::string  BuildStreamQueryData(const std::string& eventType,
                                  const std::string& eventData,
                                  long long gamets = 0);

// --- Connection status -----------------------------------------------------
bool  IsDwemerDistroConnected();
DWORD GetDwemerDistroLastSuccessAgeMs();