# CLAUDE.md — UNITY (MantellaStarfield → DwemerDistro/CHIM port)

This file is the standing briefing for any AI assistant (Claude Code, Codex, a claude.ai chat) working on this repo. Read all of it before touching code. It records what the system is, how the pieces talk to each other, why things are built the way they are, and — most importantly — the traps that have already cost hours. If you find something here that's wrong or stale, fix this file in the same change.

---

## 0. Ground rules for working in this repo

1. **Never trust your own copy of a file over the user's live file.** This project has regressed twice because an assistant edited a stale copy (once reintroducing a commented-out `ActivateLipsForCurrentLine()` call, once dropping an `fclose(f)` that caused `PermissionError: [Errno 13]` in the Python watcher). If the user mentions a manual edit, or a file has been through several rounds, ask for the live file (or `git diff`) before editing.
2. **Verify APIs against the actual `.psc` / header sources before using them.** Several functions that "should" exist do not (see §7). The Papyrus sources live at `E:\SteamLibrary\steamapps\common\Starfield\Data\Scripts\Source` (base game + `User\` for ours). CommonLibSF headers are in the xmake package cache.
3. **Skyrim evidence is not Starfield evidence.** CHIM, SkyrimNet, SKSE, and PapyrusExtender (Skyrim) are useful for *design patterns*, never as proof that an API or engine behavior exists in Starfield. Only Starfield `.psc` files, CommonLibSF, the Creation Kit, and in-game tests count.
4. **The user tests in-game; you can't.** Every change should say what to look for in the log / in game to confirm it worked. Prefer changes that log their decisions (which voice folder was resolved, which scene slot was chosen, why a line was dropped).
5. **Pasted text can arrive empty.** On claude.ai, long pastes sometimes become an attachment with no content. If a paste seems missing, ask the user to paste inline or send a screenshot — don't retry the same way.
6. **Rebuild + redeploy order matters.** Starfield must be closed before deploying the DLL (it's locked while the game runs). Papyrus compiles separately from the C++ build (see §9).

---

## 1. What this project is

**UNITY** makes Starfield NPCs converse via an LLM: the player types (or speaks) a line, nearby "activated" NPCs answer with generated text, synthesized voice, working lip sync and subtitles, and can respond to *each other* (rechat) or start talking unprompted (bored events).

It began as **MantellaStarfield** (a Starfield port of the Mantella mod) and is being moved onto **Rangroo's DwemerDistro/CHIM platform** — the same server/tooling family as CHIM (Skyrim) and STOBE (Kenshi). The server (`UNITYServer`) was cloned from **StobeServer**, so it carries Kenshi-era residue.

- Repo: `https://github.com/1100340202/MantellaStarfield.git` — tag `pre-chim-port` is the pre-port restore point.
- Collaborator/tester: **Rangroo** (maintains DwemerDistro, CHIM, the Dwemer launcher and Dwemer-Dashboard). The near-term goal is a build Rangroo can install and test on *his* machine, which is why path configurability (§8) matters.
- Owner/dev: TJ (Windows gaming PC, MO2 modlist "Star Wars Genesis").

**Target experience** (the north star): start the server → named NPCs are auto-activated → type a line in game → whoever's nearby and relevant responds; you're not locked into one NPC's conversation; voices are pulled/generated automatically for any NPC.

---

## 2. Architecture

```
                         ┌───────────────────────────────────────────────┐
  Player keypress ─────► │ Papyrus: MantellaDialogueQuestScript.psc      │
  (G/T/Esc/N/Num-/V/Del) │  - key handling, text input, roster toggle    │
                         │  - forces NPCs into scene aliases, starts     │
                         │    MantellaScene / MantellaScene2 per line    │
                         └──────────────┬────────────────▲───────────────┘
                              writes    │ Bridge.ini     │ polls (timer)
                                        ▼                │
                         ┌───────────────────────────────┴───────────────┐
                         │ SFSE C++ plugin (MantellaStarfield.dll)        │
                         │  main.cpp     – bridge polling, conversation   │
                         │                 loop, roster, Wwise hooks,     │
                         │                 lip activation, subtitles      │
                         │  UnityChat.cpp – server request/stream parse,  │
                         │                 voice-folder mapping, work file│
                         │  UnityComm.cpp – HTTP, discovery               │
                         └───────┬──────────────────────────▲─────────────┘
                   HTTP (8087)   │   UnityWork.ini (append) │ MantellaQueue.ini (read)
                                 ▼                          │
  ┌──────────────────────────────────────┐   ┌──────────────┴───────────────────────┐
  │ UNITYServer (PHP + PostgreSQL, WSL)  │   │ Python watcher: unity_wem_gen.py     │
  │  chat.php / bored.php / rechat.php   │   │  soundcache wav → WwiseConsole .wem  │
  │  LLM + TTS (Cartesia) → soundcache/  │──►│  wav → LipGenerator → ffxc .ffxanim  │
  └──────────────────────────────────────┘   │  deploys into voice folders, queues  │
                                             └──────────────────────────────────────┘
```

### 2.1 Components and where they live (TJ's machine)

| Component | Path |
|---|---|
| C++ project | `C:\Users\elija\MantellaStarfield` (`xmake build`) |
| Built DLL | `build\windows\x64\release\commonlibsf-template.dll` |
| Deployed DLL | `E:\Star Wars Genesis\Game\mods\MantellaStarfield\SFSE\Plugins\MantellaStarfield.dll` |
| Python watcher | `C:\Users\elija\MantellaStarfield\src\unity_wem_gen.py` |
| Papyrus source | `E:\SteamLibrary\steamapps\common\Starfield\Data\Scripts\Source\User\MantellaDialogueQuestScript.psc` |
| Bridge / IPC files (current) | `E:\Star Wars Genesis\Game\overwrite\SFSE\MantellaStarfield\` |
| Voice output (MO2 mod) | `E:\Star Wars Genesis\Game\mods\MantellaStarfield\Sound\Voice\MantellaStarfield.esp\<Folder>\` |
| Voice output (Data) | `E:\SteamLibrary\steamapps\common\Starfield\Data\Sound\Voice\MantellaStarfield.esp\<Folder>\` |
| WSL distro | `DwemerAI4Skyrim3` |
| Server | `/var/www/html/UNITYServer` (WSL) / `\\wsl.localhost\DwemerAI4Skyrim3\var\www\html\UNITYServer` |
| Admin UI | `http://localhost:8087/UNITYServer/ui/config_hub.php` (the `/UNITYServer/` prefix is required) |
| Deploy script | `unity-deploy.ps1` (in TJ's Documents): build → deploy DLL → start WSL services → launch watcher |

### 2.2 Server

- PHP + PostgreSQL in WSL, port **8087** (8086 is taken by PocketTTS; Rangroo patched the launcher's discovery).
- Discovery: `http://127.0.0.1:7135/discover?game=starfield`. Python falls back to hardcoded `172.25.224.225:8087` (WSL IP — changes between reboots on some machines; don't rely on it).
- DB `unity`, user/pass `dwemer`/`dwemer`. **Connect with `-h localhost`** — bare socket fails with "Peer authentication failed" because the WSL user is root.
- WSL services don't auto-start in a bare shell: `sudo service postgresql start && sudo service apache2 start`.
- Git on the UNC path: `git -c safe.directory='*' <cmd>`.
- Config is **DB-driven** (`general_settings` table), edited through the 13-tab `config_hub.php`. There's no conf.php for runtime behavior.
- Key processors: `chat.php` (player input), `processor/bored.php` (unprompted NPC chatter), `processor/rechat.php` (NPC replying to NPC). All three are fully implemented (inherited from CHIM lineage), not stubs.
- NPC tables: `core_npc_master` (+ `_history`), `bio_templates`, `bio_templates_custom`, `combined_bio_templates` (view). Voice ID resolution: `unityAiNpcResolveVoiceId()` in `ai_npcs.php` uses `core_npc_master.voiceid` first, then fuzzy gender/race/faction match (`unitySelectVoiceIdForNpc()` in `lib/data_functions.php`).

**Request envelope** (all three processors): `type|timestamp|gamets|data`, base64'd into `?DATA=` (built by `BuildStreamQueryData` in `UnityChat.cpp`). Participants go in a *separate* `?people=` param: JSON array of `"Name|StorageID"` strings (StorageID optional; parsed by `extractParticipantIdentityToken()`). The server never figures out who is nearby — **proximity is the plugin's job**.

**Relevant settings**: `BORED_EVENT_CHANCE` (server-side probability roll per bored request), `RECHAT_RESPONSES` (max chain rounds), `RECHAT_PROBABILITY`, `SPEAKER_RECHAT`, `ENFORCE_STRICT_RECHAT_RESPONSE`.

### 2.3 IPC files (all in the bridge dir)

| File | Writer → Reader | Purpose |
|---|---|---|
| `Bridge.ini` | Papyrus ↔ C++ | Commands (start/input/end/rename/toggle) with a **nonce** for dedupe; C++ writes back state incl. `[activation]` roster and per-line speaker info |
| `UnityWork.ini` | C++ → Python | Append-only lines `hash\|voice_folder\|speaker_name\|subtitle`, turns terminated by `__ENDTURN__` |
| `MantellaQueue.ini` | Python → C++ | Ready lines: slot, duration, speaker, subtitle, `turn_done` |
| `MantellaNames.ini` | Papyrus/C++ | Rename (N key) mappings |

Work-line format note: `speaker_name` is placed **before** `subtitle` so subtitle remains the last catch-all field and can contain `|`.

### 2.4 Per-line flow (happy path)

1. Player presses **G** on an NPC (crosshair) → Papyrus opens text input → on submit, writes `type=input` + nonce to `Bridge.ini`.
2. C++ poll sees the new nonce → bumps `g_conversationGeneration` → spawns `RunConversationLoop` → `SendUnityConversation` streams from `chat.php` with `people=` from the activated roster.
3. Each streamed line: `ResolveVoiceFolder(speaker)` → `AppendWorkLine()` into `UnityWork.ini`; `__ENDTURN__` at end.
4. Python sees new lines → pulls the Cartesia wav from the server's `soundcache/` → WwiseConsole → `.wem`; sanitized wav → LipGenerator → ffxc → `.ffxanim`; deploys both into the voice folder(s) in a slot `00666A79 + n` → writes `MantellaQueue.ini`.
5. C++ `PlayQueuedLine` drains the queue → writes the speaker name to Bridge → Papyrus resolves the actor (`GetFormsByTESFullName`), picks a scene slot (`SelectSpeakerScene` → 1 or 2), forces the actor into that scene's alias and starts it.
6. The scene's dialogue line fires; the **Wwise path/cookie hooks** redirect the audio to the current `.wem`; `ActivateLipsForCurrentLine()` has already copied the line's ffxanim onto the fixed `00666A79.ffxanim` read slot in every candidate folder; `PatchDialogueSubtitle` swaps in the real text.
7. After the stream finishes, the client dispatches a **rechat** request; the server decides (authoritatively) whether another NPC replies.

---

## 3. Keybinds (Win32 VK codes, not DirectInput scancodes)

| Key | VK | Action |
|---|---|---|
| G | 0x47 | Open text input / submit accumulated input (`ActivationKey`) |
| T | 0x54 | Accumulate more input before sending (`ContinueKey`) |
| Esc | 0x1B | End conversation, or cancel pending input if no conversation (`EndKey`) |
| N | 0x4E | Rename targeted NPC |
| Numpad − | 0x6D | Toggle NPC activation (roster) |
| Delete | 0x2E | Freeform talk |
| V | 0x56 | Voice record (STT) |

VK codes confirmed because 'G'/'T'/'N' equal their ASCII values (known exception: Numpad Plus arrived as 18/Alt).

**Keys are config-driven.** `[Keys]` in `MantellaStarfield_Config.ini` takes names (`G`, `F6`, `NumpadMinus`) or codes. `LoadRuntimeConfig()` resolves them (`ParseKeyName`) and writes `Bridge.ini [keys]` + a `version` stamp. Papyrus re-applies them to its properties on every load via `ReadKeyOr` in timer 3. The .psc property defaults are only fallbacks, since property values are baked into saves. G conflicting with game bindings is now just a config edit; no new key chosen yet.

---

## 4. Key design decisions — and why

### 4.1 Scenes, and the two-scene setup
Dialogue is delivered by starting a CK **Scene** whose single line is redirected at runtime (audio via Wwise hook, lips via ffxanim slot copy, text via subtitle patch). Early multi-NPC tests showed the wrong NPC animating. **Root cause: the scene's Primary Actor Alias** gets cached — reassigning the alias's actor on the same scene doesn't reliably retarget. **Fix:** two genuinely separate Scene objects, each with its own alias set as Primary:
- `MantellaScene` — `MantellaDialogueScene` (FormID `01000816`)
- `MantellaScene2` — `MantellaDialogueSceneCOPY0001` (`01000881`)

`SelectSpeakerScene` returns 1 or 2 so consecutive different speakers alternate. `GetFacingTarget` handles who looks at whom. Confirmed working in-game.

### 4.2 Why not one continuous multi-actor scene (investigated, parked)
Vanilla Starfield does have multi-speaker scenes (Andreja/Sarah with one Primary; a 3-alias Aggie scene in `COM_Quest_Andreja_Q01` with **no** Primary; Crimson Fleet uses Find Matching Reference fills; SharedInfo generic-dialogue quests have no scene at all). TJ's motivation is real: today each NPC is effectively in a separate dialogue event, which affects look-at, who's "in" the conversation, etc.

**But `Scene.psc` in Starfield exposes only:** `ForceStart`, `Start`, `Stop`, `IsPlaying`, `GetOwningQuest`, `IsActionComplete(int)`, `Pause(bool)`, and events `OnPhaseBegin`/`OnPhaseEnd`. **There is no way to advance or jump to a phase from script.** A single scene with N phases can't be driven line-by-line by an LLM stream of unknown length/order. That's why per-line Stop/ForceStart persists. A CK experiment (one scene, all nearby candidates as aliases) is still worth doing for the knowledge, but don't build on it without an in-game proof.

### 4.3 CHIM doesn't use scenes at all (future direction)
CHIM's `SpeakManager.cpp` writes directly to `RE::SubtitleManager` and plays audio itself, bypassing Topic/INFO/Scene. If CommonLibSF exposes an equivalent direct subtitle/voice path, the whole scene apparatus could go. **Unverified for Starfield** — check CommonLibSF before assuming.

**`ObjectReference.Say()` exists in Starfield and is used in vanilla** (18 calls in 13 Bethesda quest scripts, e.g. `QF_MS06`, `QF_City_CY_RedTape03`, `QF_UCR01`). Signature: `Say(Topic, Actor akActorToSpeakAs = None, bool abSpeakInPlayersHead = false, ObjectReference akTarget = None)`; `Actor.IsTalking()` reports whether speech started. **Experiment wired in (untested):** `[Experimental] DeliveryMode=say` makes Papyrus call `DeliverLineWithSay()` (speaker addresses the previous NPC speaker, else the player) instead of starting a scene. Everything upstream is unchanged. It needs the `MantellaSayTopic` property filled in the CK with the topic holding INFO `xx000826`. If that proves out, it replaces the scene machinery in §4.1/§4.2.

### 4.4 Native Wwise hook vs CHIM's XAudio2
We hook the engine's own Wwise path/cookie resolution so audio plays through the game's voice system (positional, correct bus, triggers the engine's lip-sync lookup). Offsets for game version **1.16.244.0**:
- `PATH_CALLSITE = 0xF1BFB1`
- `COOKIE_CALLSITE = 0xF1C0A6`
- `DialogueManager::Say = 0xEEEC60`

**A Starfield update will break these.** If audio goes silent after a patch, check game version first.

### 4.5 Voice folders: dedicated vs generic (the Grand Inquisitor lesson)
- `NpcVoiceTypeMap()` in `UnityChat.cpp` maps **speaker name → dedicated folder** (`"Sarah Morgan" → "NPCFSarahMorgan"`). This drives where the wem/ffxanim are written and which FaceFX actor (male/female) Python compiles with (`is_male_voice_folder()` keys off the folder name).
- Custom NPCs using a *generic* voice type (e.g. Grand Inquisitor, Starkiller → in-game voice type `GenericMaleRough`) got no lip movement because the engine reads lips from the NPC's real voice-type folder. **Fix:** `ActivateLipsForCurrentLine()` blanket-copies the current ffxanim into `00666A79.ffxanim` in *every* generic folder (`GenericMaleRough`, `GenericMaleSmooth`, `GenericMale01–04`, `GenericFemaleRough/Smooth`, `GenericFemale01–04`) plus the line's own folder.
- **Therefore: a map value must never be a generic folder.** Map those NPCs to a unique dedicated folder (`"Grand Inquisitor" → "NPCMGrandInquisitor"`, `"Starkiller" → "NPCMGalenMarek"`); the blanket deployment covers their lips.
- **The map key must match `speaker_name` exactly as the server returns it** (Galen Marek is `"Starkiller"` server-side). Unmapped names fall back to `DEFAULT_VOICE_FOLDER = "NPCFSarahMorgan"` and log "no voice folder mapping" — the telltale for a male NPC with female lips.
- The whole map is **temporary**; see §6.1 (auto voice type).

### 4.6 The 00666A79 slot scheme
Python rotates through 200 slots (`00666A79`…`00666A79+199`). The scene always reads lips from `00666A79.ffxanim`, so each line's ffxanim is copied onto that fixed name just before it plays. The copy intermittently fails with *file in use* (`ActivateLips COPY FAILED` on 00666A79) — known, open, usually a missed lip-sync on one line.

### 4.7 Addressee default is empty string, not `"Player"`
The client used to send `"Player"` as addressee/initiator. `rechat.php` excludes candidates by the *real player character name*, so the literal `"Player"` slipped through as a legitimate candidate and won → fabricated player dialogue. Default is now `""` (`pushCandidate()` short-circuits on empty). A client-side filter also drops any line with `actor == "Player"` as a backstop. **Don't reintroduce `"Player"` as a placeholder anywhere.**

### 4.8 `RechatThread` must not call `ResetWorkFile()`
Truncating `UnityWork.ini` while Python was mid-read silently dropped lines — the real cause of an old "bored event produces zero lines" mystery. Python's watcher handles a continuously-growing file with multiple `__ENDTURN__` markers, and detects truncation ("work file was truncated; resetting consumed counter"). Only a genuinely new conversation resets the file.

### 4.9 Conversation generation counter
Detached `RunConversationLoop` threads can outlive the conversation they belong to. `g_conversationActive` alone isn't enough (Esc resets it before the old thread notices). Every loop captures `myGeneration` and checks `g_conversationGeneration` each iteration, including mid-sleep. Any new start/end bumps it.

### 4.9a Chain epoch + one `__ENDTURN__` per chain
A "chain" is a player turn or bored event plus every rechat hop that follows it. `UnityChat.cpp` tags each chain with `g_chatEpoch`. `CancelPendingChats()` (called on conversation start/end/reinit, and by every new `SendUnityConversation`/`SendUnityBoredEvent`) invalidates older chains: they stop queuing lines and stop firing hops. `FinishStream()` writes `__ENDTURN__` **only when the chain ends** (no follow-up hop fired). Python never clears `turn_done`, so the old per-hop marker set it to 1 after the first hop, and if playback caught up during the gap before the next hop, the loop ended the turn and the rest of the chain was never played. Tradeoff: the player gets "Press G to speak" only after the whole chain has played.

### 4.10 Bored events: interval + server chance
`kBoredEventIntervalMs = 60 * 1000` (matches CHIM's `GlobalBoredEventTimeOut`). Every 60s the client *asks*; `bored.php` rolls `BORED_EVENT_CHANCE` and may return nothing. So "chance 30%" means ~30% of the 60s ticks produce chatter — a request that returns nothing is expected, not a bug.

### 4.11 Roster is session-only; manual activation first
`Numpad −` toggles an NPC in `g_activatedNpcs` (mutex-guarded vector of `{formId, name}`), mirroring CHIM's manual `setManuallyAdded(true)` path. Guards: never the player (`0x14`/`IsPlayer()`), never dead/disabled/deleted, dedupe by FormID (toggle = remove). Not persisted across saves by choice (CHIM uses cosave serialization; we deferred it). Roster state is polled by Papyrus from **`aiTimerID == 3`** (always-on heartbeat) — *not* timer 1, which only runs while `ConversationActive`, exactly when toggling is disallowed. Auto-activation by proximity (CHIM reads `ProcessLists::highActorHandles` + distance) is the eventual goal.

### 4.12 Speaker identity by name, not FormID
Papyrus can't turn an int FormID back into a Form in this ecosystem (see §7), so the pipeline carries `speaker_name` and Papyrus resolves with `CassiopeiaPapyrusExtender.GetFormsByTESFullName(name)`, taking the first result that casts to `Actor`. Empty name → falls back to `CurrentSpeakerActor`. Duplicate display names will resolve to the first match — known limitation.

---

## 5. Lip-sync / audio pipeline details (unity_wem_gen.py)

Every one of these was a real bug; don't "simplify" them away:

- **LipGenerator takes the raw WAV, not the WEM.** Args: `LipGenerator wav text actor.facefx analysis.facefx ...`.
- **The `text` arg is a raw string**, not a path, despite the usage banner showing `test.txt`. Matches Mantella's working `starfield.py`.
- **Cartesia WAVs have placeholder `data` sizes** (`0` or `0xFFFFFFFF`) — `parse_wav_pcm()`/`sanitize_wav_for_lipgen()` walk RIFF chunks manually and treat bad sizes as "rest of file".
- **The declared `byte_rate` can also be wrong** — recompute `sample_rate * channels * bytes_per_sample`.
- **ffxc names output after the source WAV's basename**, ignoring `-AnimationGroupName`/`-OutputFileName`. The sanitized wav must keep the slot number as its basename (different subfolder, no `_clean` suffix).
- **ffxc `-o=` must be a real registered Data voice folder** (`NPCFSarahMorgan` for female / `NPCMBarrett` for male — `REGISTERED_FEMALE_FOLDER`/`REGISTERED_MALE_FOLDER`), not a scratch folder, or output silently vanishes.
- **Duration comes from the parsed header**, not `(size-44)/44100`.
- Male/female FaceFX actor is picked by `is_male_voice_folder()` from the folder name (`MALE_VOICE_TYPES` set + `NPCM` prefix). Robot folders (`RobotModelAVasco`) have no mouth; a guard to skip lip gen for `RobotModel*` would be tidy.
- ffxc prints ~200 warnings per build; that's normal (filtered in logs).
- Tools required: Wwise 2021.1.14.8108 (WwiseConsole), Starfield CK tools (`Tools\LipGenerator`, `Tools\FaceFX\ffxc.exe`, `StarfieldHumanFemale/Male.facefx`, `GenesisDefault.facefx`), `Intermediate\Slice_00/01`.

---

## 6. Current open threads (priority order, as of 2026-09-29)

### 6.1 Automatic voice-type resolution (in progress — next up)
Goal: any activated NPC gets the right voice folder without editing `NpcVoiceTypeMap`. Confirmed native chain in Papyrus:
```papyrus
VoiceType vt = akActor.GetVoiceType()                       ; ObjectReference.psc (native)
String folder = CassiopeiaPapyrusExtender.GetFormEditorID(vt) ; e.g. "NPCFSarahMorgan", "GenericMaleRough"
```
Plan: resolve in `ToggleNpcActivation`, write `name → folder` into `Bridge.ini`'s roster, have C++ use it in `ResolveVoiceFolder` before the static map, and forward it to the server if useful. **Careful:** for generic voice types the resolved folder *is* a generic folder — per §4.5 we should keep routing audio to a unique per-NPC folder (e.g. derived from the NPC name) and let the blanket copy handle lips, or rework the blanket copy so it targets only the resolved folder. Decide this explicitly; don't just plug the EditorID into the map.

Longer term (Rangroo's CHIM approach): `*_voices.csv` (`voicetype,voicefile,transcript`) parsed by `FindVoiceCSVFiles`/`ParseVoiceCSV` to auto-pick a voice sample per voice type for TTS cloning. Rangroo suggested **StarfieldVoiceTool** (yak3d) for extracting Starfield voice samples; LazyVoiceFinder doesn't support Starfield.

### 6.2 Path configurability (done in code — needs a build + in-game test)
Everything now resolves from the **game folder**, so no machine paths remain in C++, Papyrus or Python:
- **C++** `LoadPathConfig()`: game dir = `GetModuleFileNameA(nullptr)`'s folder. `BridgeDir` defaults to `<game>\Data\SFSE\MantellaStarfield\`, and voice bases to `<game>\Data\Sound\Voice\MantellaStarfield.esp` (+ optional `VoiceDir2`). Overrides go in `MantellaStarfield_Config.ini` next to the DLL. The startup log prints every resolved path.
- **Papyrus** `BridgePath`/`NamesPath` = `.\Data\SFSE\MantellaStarfield\` (game-relative, the same convention Cassiopeia's own docs use). Inside the game process, MO2's VFS maps this onto `<MO2>\overwrite\SFSE\MantellaStarfield\`, so C++ and Papyrus agree with zero config. **Unverified:** that Cassiopeia's `ReadIni`/`WriteIni` accept relative paths (its docs only show them for the file functions). Property defaults are baked into saves, so test from a *new game or `coc` from the main menu*, not an old save.
- **Python** reads `unity_wem_gen.ini` (next to the script, or argv[1]). `StarfieldDir` derives every CK tool path. The watcher runs *outside* MO2's VFS, so MO2 users must set `BridgeDir` to the real overwrite folder and `MO2VoiceDir`. Server discovery is real now (1.5s timeout, cached), falling back to `127.0.0.1:8087` (WSL2 localhost forwarding) instead of a raw WSL IP.
- TJ's exact old behavior is reproduced by `build/MantellaStarfield_Config.TJ.ini` + `build/unity_wem_gen.TJ.ini`.

### 6.3 Stuck-input fixes (written, need recompile + play-test)
Two confirmed paths that left `IsAccumulating = True` forever (G dead until reload):
1. **Esc with no active conversation** — EndKey guard now handles `ConversationActive || IsAccumulating`; the accumulating branch clears `IsAccumulating`, `AccumulatedInput`, `PendingAutoSendInput` and notifies "Input cancelled".
2. **Empty submit** — submit branch now copies input, resets state unconditionally, and only sends if non-empty.

Suspected third path: `TextInputMenu` cancelled (returns `""`) — verify it goes through the same reset.

### 6.4 Mid-conversation target redirect (designed, not built)
Today, once G starts a conversation with Sarah, you can't G-address Noel without ending it. Agreed design: redirect on **crosshair target at submit time** or **name in the message** — *not* proximity. Longer term this becomes "ConversationActive means input is open," with the server choosing responders from the whole roster (would need `chat.php` to accept a roster and pick server-side, like rechat already does — unconfirmed).

### 6.5 NPCs don't "hear" each other's rechat lines
Noel and Walter can reply to each other, but Sarah later acts unaware. Suspect: `chat.php` builds context via `DataEventLog(contextHistory, $targetNpc)` where `$targetNpc` = profile or `extractDialogueTarget(message)` — history may be filtered per-target. **Uninvestigated.**

### 6.6 Other known issues
- `ActivateLips COPY FAILED` on `00666A79` (file in use), intermittent.
- Plugin reports `plugin index 0xB` — FormIDs like `01000816` may assume a load-order position; verify on a different load order (Rangroo's).
- G key conflicts with game bindings: now a `[Keys]` config edit; a new default just needs choosing.
- Server data: Muria Siarkiewicz never seeded into `core_npc_master`; Cora Coe has no voice ID; `schema.sql` `PROMPT_HEAD` may still contain Kenshi text (`#HERIKA_NAME#`, "post-apocalyptic... desolate planet") — the live DB row was fixed, the seed may not be; no systematic Kenshi-residue audit done.
- Dwemer-Dashboard (Rangroo's repo) is hardcoded to `dwemer` + `stobe` DBs — UNITY support there is new feature work for Rangroo, not config.
- Parked: SFSE Menu Framework (in-game config UI), cosave roster persistence, proximity auto-activation.

### 6.7 "Send to Rangroo" checklist
- [ ] Recompile Papyrus + DLL with the stuck-input fixes; play-test.
- [x] Path configurability in code (C++, Papyrus, Python). [ ] Test the relative Papyrus path from a new game.
- [ ] Remove/flag everything machine-specific (E:\ drive, MO2 "Star Wars Genesis" paths, WSL IP fallback, custom Star Wars NPCs in the voice map).
- [ ] Verify FormIDs/plugin index on a clean load order.
- [ ] Confirm Wwise/CK tool requirements and versions are documented.
- [ ] Package: DLL, esp/esm, compiled `.pex`, Python watcher + requirements, config template, server migration/seed SQL.
- [ ] "What works / what's rough" note (known issues above).
- [x] Keys configurable (pick a new default for G if wanted).

---

## 7. Traps and gotchas (read before writing code)

**Papyrus / Starfield scripting**
- `StringUtil` **does not exist** in Starfield Papyrus. Use `CassiopeiaPapyrusExtender` string functions / `FindStr`.
- `Game.GetFormEx()` **does not exist** here (it's Skyrim PapyrusExtender). There is no int→Form lookup; resolve by name (`GetFormsByTESFullName`) or pass references.
- The extender is **CassiopeiaPapyrusExtender**, not PapyrusExtender/PO3.
- `Scene` has **no phase-advance** API (§4.2).
- Timer 1 only runs during a conversation; use timer 3 for anything that must work outside one.
- Always handle *both* `ConversationActive` and `IsAccumulating` in key handlers — mismatched state = dead G key.
- Papyrus doesn't hot-reload into a save reliably; test on a save that hasn't baked the old script state, or expect stale properties (hardcoded path properties are baked into saves!).

**C++ / CommonLibSF**
- `RE::FormID`-style Skyrim-isms and `GetFormEx` equivalents may not exist or differ; check headers.
- Hook offsets are version-pinned (§4.4).
- Every `fopen_s` needs its `fclose` — the Python watcher shares these files and a leaked handle causes `PermissionError` on the other side.
- Don't call `ResetWorkFile()` except when a brand-new conversation starts (§4.8).
- Threads are detached; always check the generation counter (§4.9).

- FormIDs sent to Papyrus must be written with `FormIdToPapyrusString()` (signed). Papyrus `as Int` is signed 32-bit, so unsigned decimals for `0xFE…`/`0xFF…` forms don't parse.
- Queue/work files carry UTF-8 subtitles. Never parse them with a codepage-dependent reader (configparser on Windows = cp1252). The watcher reads `write_index` from raw bytes.
- Every watcher line gets at most `MAX_LINE_ATTEMPTS` tries. An exception must never stop `consumed` from advancing.

**Voice / lips**
- Map key = exact server `speaker_name`; value = unique dedicated folder, never a generic one (§4.5).
- Unmapped NPC → Sarah's folder → female FaceFX actor → wrong lips on men. Check the log line "no voice folder mapping".
- If a *new custom NPC* has no mouth movement: check its in-game voice type is in the generic list in `ActivateLipsForCurrentLine()` (~line 1102), and that it has a map entry.

**Server / environment**
- Start postgres + apache manually in WSL; use `psql -h localhost -U dwemer -d unity`.
- Admin UI URL needs `/UNITYServer/`.
- `BORED_EVENT_CHANCE` gates server-side; empty bored responses are normal.
- Seed data (`data/schema.sql`) and live DB can disagree; fixing one doesn't fix the other.

**Process**
- Stale copies (§0.1). Diff before editing.
- Close Starfield before deploying the DLL.
- "Nothing happens in game" is usually not a build failure: check the SFSE log for plugin load, check the bridge dir is the one Papyrus/Python use (§6.2 trap), check the roster isn't empty, check the watcher window is running.

---

## 8. Configuration reference

`MantellaStarfield_Config.ini` (next to the DLL). All keys are optional; see `build/MantellaStarfield_Config.ini` for the commented template:
```ini
[Paths]
BridgeDir=   ; default <game>\Data\SFSE\MantellaStarfield\  (MO2 VFS -> overwrite)
VoiceDir1=   ; default <game>\Data\Sound\Voice\MantellaStarfield.esp
VoiceDir2=   ; optional, e.g. <MO2>\mods\MantellaStarfield\Sound\Voice\MantellaStarfield.esp
[Keys]       ; Talk, AddMoreText, EndConversation, RenameNpc, ToggleActivation, TalkToNearest, PushToTalk
[Tuning]     ; BoredEventsEnabled, BoredIntervalSeconds, BoredPerNpcCooldownSeconds,
             ; BoredRadiusInterior/Exterior, PlaybackBufferMs
[Compat]     ; SkipVersionCheck=1 installs hooks on an unsupported game version (may crash)
```
Inline `; comments` are allowed in `[Keys]`/`[Tuning]`/`[Compat]` (`ReadConfigValue` strips them), NOT in `[Paths]`.

**Status line.** `UpdateStatus()` (bridge monitor loop, every 2s) sums up: game version / hooks, server reachability (background check every 30s), and the watcher heartbeat (`UnityWatcher.ini` in the bridge dir, rewritten every 2s by a thread in `unity_wem_gen.py`, stale after 15s, plus a `problems` string for missing tools or failing audio downloads). The result goes into `Bridge.ini [status] message` **only when it changes**, and Papyrus shows each new message once, and again after every load. "UNITY ready" appears only after the first server check passes. Game version check: `ReadGameVersion()` reads Starfield.exe's file version and compares it to `kSupportedGameVersion`. On a mismatch the hooks are skipped.
`unity_wem_gen.ini` (next to the watcher):
```ini
[Paths]
StarfieldDir=   ; required; CK tools derived from <StarfieldDir>\Tools
BridgeDir=      ; REAL folder; MO2 users: <MO2>\overwrite\SFSE\MantellaStarfield
MO2VoiceDir=    ; optional second voice output
WwiseConsole=   ; default C:\Audiokinetic\Wwise_2021.1.14.8108\...\WwiseConsole.exe
ServerHostPort= ; fallback if discovery fails; default 127.0.0.1:8087
ScratchDir=     ; default %TEMP%\unity_wemgen
```
Templates: `build/*.example.ini`, TJ's values in `build/*.TJ.ini`.

## 9. Build, deploy, test

```powershell
# Everything except Papyrus (close Starfield first):
powershell -ExecutionPolicy Bypass -File .\unity-deploy.ps1

# Manual C++ build:
cd C:\Users\elija\MantellaStarfield ; xmake build
copy build\windows\x64\release\commonlibsf-template.dll "E:\Star Wars Genesis\Game\mods\MantellaStarfield\SFSE\Plugins\MantellaStarfield.dll"

# Papyrus (from Starfield\Tools\Papyrus Compiler):
.\PapyrusCompiler.exe "...\Data\Scripts\Source\User\MantellaDialogueQuestScript.psc" `
  -f="...\Data\Scripts\Source\Starfield_Papyrus_Flags.flg" -i="...\Data\Scripts\Source" -o="...\Data\Scripts"

# Server (WSL):
wsl -d DwemerAI4Skyrim3
sudo service postgresql start ; sudo service apache2 start

# Watcher:
cd C:\Users\elija\MantellaStarfield\src ; python unity_wem_gen.py
```

**Smoke test:** load save → Numpad − on Sarah (notification + roster log line) → G on Sarah → type → watch the watcher window (`processing … voice=NPCFSarahMorgan`, `queued line`, `turn_done=1`) → hear voice, see lips + subtitle → Esc. Then: activate a second NPC, confirm rechat alternates `MantellaScene`/`MantellaScene2` and the correct mouth moves. Then: G + Esc without typing, G + empty submit — G must still work afterward.

**Logs to check:** SFSE plugin log (`My Games\Starfield\SFSE\Logs\`), Papyrus log (`Debug.Trace` lines prefixed `Mantella:`), watcher console, Apache/PHP error log in WSL.

---

## 10. Reference: CHIM patterns worth copying

- Proximity: `InspectSurroundings()` over `ProcessLists::highActorHandles` + distance, tagging `(dead)/(hostile)/(busy)/(in combat)/(far away)/(restrained)`; filter downstream.
- Director: `getLessBoredAgentNearby()` picks the agent with the lowest `boredEventsFired` (cap/reset at 5).
- One global bored cooldown (`controlLastBoredTriggerTS`) pushed forward by *any* conversational activity.
- `MAX_RECHAT_DEPTH = 10` client-side cap regardless of server math.
- Manual activation + cosave persistence (`AgentCountRecord`).
- Voice: `*_voices.csv` per game, voice type → sample file + transcript for cloning.
