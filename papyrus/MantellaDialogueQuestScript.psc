ScriptName User:MantellaDialogueQuestScript Extends Quest

Scene Property MantellaScene Auto
ReferenceAlias Property MantellaSpeakerAlias Auto

String Property BridgePath = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\" Auto
String Property BridgeIni = "bridge" Auto
String Property NamesPath = "E:\\Star Wars Genesis\\Game\\overwrite\\SFSE\\MantellaStarfield\\" Auto
String Property NamesIni = "MantellaNames" Auto
Int Property Nonce = 0 Auto
Bool Property ConversationActive = False Auto
Bool Property WaitingForInput = False Auto
Bool Property IsAccumulating = False Auto
Bool Property IsRenaming = False Auto
String Property AccumulatedInput = "" Auto
String Property CurrentSpeaker = "Unknown" Auto
Actor Property CurrentSpeakerActor = None Auto
Actor Property RenameTargetActor = None Auto
Int Property ActivationKey = 0x47 Auto ; G key
Int Property ContinueKey = 0x54 Auto   ; T key
Int Property EndKey = 0x1B Auto        ; Escape key
Int Property RenameKey = 0x4E Auto     ; N key - rename NPC for Mantella

Event OnInit()
    Debug.Trace("Mantella: Quest OnInit")
    RegisterForEvents()
    StartTimer(1.0, 3)
EndEvent

Function RegisterForEvents() global
    CassiopeiaPapyrusExtender.RegisterForNativeEvent("User:MantellaDialogueQuestScript", "BSInputEvent")
    CassiopeiaPapyrusExtender.RegisterForNativeEvent("User:MantellaDialogueQuestScript", "TextInputMenu_EndEditText")
    CassiopeiaPapyrusExtender.RegisterForNativeEvent("User:MantellaDialogueQuestScript", "TESLoadGameEvent")
    Debug.Trace("Mantella: Events registered")
EndFunction

Function TESLoadGameEvent() global
    Quest mantellaQuest = Game.GetFormFromFile(0x00000807, "MantellaStarfield.esp") as Quest
    If mantellaQuest == None
        Return
    EndIf
    User:MantellaDialogueQuestScript script = mantellaQuest as User:MantellaDialogueQuestScript
    If script == None
        Return
    EndIf
    RegisterForEvents()
    script.ConversationActive = False
    script.WaitingForInput = False
    script.IsAccumulating = False
    script.IsRenaming = False
    script.AccumulatedInput = ""
    script.CurrentSpeakerActor = None
    script.RenameTargetActor = None
    If script.MantellaScene != None
        script.MantellaScene.Stop()
    EndIf
    CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "type", "reinit")
    Debug.Trace("Mantella: Re-registered and re-initialized on game load")
EndFunction

Function BSInputEvent(Int aiKeyCode, String asControlName, String asFriendlyName, Bool bPressed, Float afHeldTime) global
    Quest mantellaQuest = Game.GetFormFromFile(0x00000807, "MantellaStarfield.esp") as Quest
    If mantellaQuest == None
        Return
    EndIf
    User:MantellaDialogueQuestScript script = mantellaQuest as User:MantellaDialogueQuestScript
    If script == None
        Return
    EndIf
    If !bPressed
        Return
    EndIf
    If CassiopeiaPapyrusExtender.IsMenuOpen("PauseMenu") || CassiopeiaPapyrusExtender.IsMenuOpen("ContainerMenu") || CassiopeiaPapyrusExtender.IsMenuOpen("InventoryMenu") || CassiopeiaPapyrusExtender.IsMenuOpen("DialogueMenu") || CassiopeiaPapyrusExtender.IsMenuOpen("TextInputMenu")
        Return
    EndIf

    ; N key — rename targeted NPC for Mantella
    If aiKeyCode == script.RenameKey && !script.ConversationActive && !script.IsRenaming
        Actor renameTarget = None
        ObjectReference crosshairRef = CassiopeiaPapyrusExtender.GetCrosshairRef()
        If crosshairRef != None
            renameTarget = crosshairRef as Actor
        EndIf
        If renameTarget == None
            ObjectReference headTrack = CassiopeiaPapyrusExtender.GetHeadTrackTarget(Game.GetPlayer())
            If headTrack != None
                renameTarget = headTrack as Actor
            EndIf
        EndIf
        If renameTarget == None || renameTarget == Game.GetPlayer()
            Debug.Notification("Mantella: No NPC targeted for rename")
            Return
        EndIf
        script.RenameTargetActor = renameTarget
        script.IsRenaming = True
        String currentName = CassiopeiaPapyrusExtender.GetReferenceName(renameTarget)
        CassiopeiaPapyrusExtender.OpenTextInputMenu("Mantella: Rename NPC", "Current: " + currentName)
        Return
    EndIf

    If aiKeyCode == script.ActivationKey && !script.ConversationActive
        ; Start new conversation
        Actor speaker = None
        ObjectReference crosshairRef = CassiopeiaPapyrusExtender.GetCrosshairRef()
        If crosshairRef != None
            speaker = crosshairRef as Actor
        EndIf
        If speaker == None
            ObjectReference headTrack = CassiopeiaPapyrusExtender.GetHeadTrackTarget(Game.GetPlayer())
            If headTrack != None
                speaker = headTrack as Actor
            EndIf
        EndIf
        If speaker == None || speaker == Game.GetPlayer()
            Debug.Notification("Mantella: No NPC targeted - look at an NPC and press G")
            Return
        EndIf
        String speakerName = "Unknown"
        ; Check MantellaNames.ini for custom name first
        String refIDHex = CassiopeiaPapyrusExtender.GetHexFormID(speaker as Form)
        String customName = CassiopeiaPapyrusExtender.ReadIni(script.NamesPath, script.NamesIni, "Names", refIDHex)
        If customName != ""
            speakerName = customName
        Else
            speakerName = CassiopeiaPapyrusExtender.GetReferenceName(speaker)
            If speakerName == ""
                Form baseForm = speaker.GetBaseObject()
                If baseForm != None
                    speakerName = CassiopeiaPapyrusExtender.GetTESFullName(baseForm)
                EndIf
            EndIf
        EndIf
        If speakerName == ""
            speakerName = speaker.GetFormEditorID()
        EndIf
        If speakerName == ""
            speakerName = "Unknown"
        EndIf
        String locationName = "Unknown Location"
        Location currentLocation = Game.GetPlayer().GetCurrentLocation()
        If currentLocation != None
            locationName = CassiopeiaPapyrusExtender.GetTESFullName(currentLocation as Form)
            If locationName == ""
                locationName = "Unknown Location"
            EndIf
        EndIf
        Float localTime = Game.GetLocalTime()
        Int gameHour = localTime as Int
        Float gameDays = Game.GetRealHoursPassed() / 24.0
        CassiopeiaPapyrusExtender.RotateActor(speaker, speaker.GetAngleZ() + speaker.GetHeadingAngle(Game.GetPlayer()), 0.0)
        script.ConversationActive = True
        script.WaitingForInput = False
        script.IsAccumulating = False
        script.AccumulatedInput = ""
        script.CurrentSpeaker = speakerName
        script.CurrentSpeakerActor = speaker
        script.Nonce += 1
        Int speakerRefID = speaker.GetFormID()
        Debug.Trace("Mantella: Speaker GetFormID returned " + speakerRefID)
        Debug.Notification("Mantella: RefID=" + speakerRefID)
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "speaker", speakerName)
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "speaker_refid", speakerRefID as String)
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "location", locationName)
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "hour", gameHour as String)
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "gamedays", gameDays as String)
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "type", "start")
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "nonce", script.Nonce as String)
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "speaking", "active", "0")
        Debug.Notification("Mantella: Starting conversation with " + speakerName)

    ElseIf aiKeyCode == script.ActivationKey && script.ConversationActive && script.WaitingForInput && !script.IsAccumulating
        If script.MantellaScene != None
            script.MantellaScene.Stop()
        EndIf
        script.IsAccumulating = True
        script.AccumulatedInput = ""
        script.WaitingForInput = False
        CassiopeiaPapyrusExtender.OpenTextInputMenu("Talk to " + script.CurrentSpeaker, "Press T to add more")

    ElseIf aiKeyCode == script.ActivationKey && script.ConversationActive && script.IsAccumulating
        If script.AccumulatedInput != ""
            Debug.Trace("Mantella: Player said: " + script.AccumulatedInput)
            script.Nonce += 1
            CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "player_input", script.AccumulatedInput)
            CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "type", "player_input")
            CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "nonce", script.Nonce as String)
            script.AccumulatedInput = ""
            script.IsAccumulating = False
            Debug.Notification("Mantella: Sent!")
        EndIf

    ElseIf aiKeyCode == script.ContinueKey && script.ConversationActive && script.IsAccumulating
        CassiopeiaPapyrusExtender.OpenTextInputMenu("Talk to " + script.CurrentSpeaker + " (adding more)", "Press T again for more, G to send all")

    ElseIf aiKeyCode == script.EndKey && script.ConversationActive
        script.ConversationActive = False
        script.WaitingForInput = False
        script.IsAccumulating = False
        script.AccumulatedInput = ""
        script.CurrentSpeakerActor = None
        If script.MantellaScene != None
            script.MantellaScene.Stop()
        EndIf
        script.Nonce += 1
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "type", "end")
        CassiopeiaPapyrusExtender.WriteIni(script.BridgePath, script.BridgeIni, "request", "nonce", script.Nonce as String)
        Debug.Notification("Mantella: Conversation ended")
    EndIf
EndFunction

Event OnTimer(Int aiTimerID)
    If aiTimerID == 1
        If !ConversationActive
            Return
        EndIf
        String currentType = CassiopeiaPapyrusExtender.ReadIni(BridgePath, BridgeIni, "request", "type")
        If currentType == "await_input"
            If !WaitingForInput && !IsAccumulating
                If MantellaScene != None
                    MantellaScene.Stop()
                EndIf
                WaitingForInput = True
                Debug.Notification("Mantella: Press G to speak")
                Debug.Trace("Mantella: await_input detected, waiting for G key")
            EndIf
        Else
            String speakingActive = CassiopeiaPapyrusExtender.ReadIni(BridgePath, BridgeIni, "speaking", "active")
            String wemReady = CassiopeiaPapyrusExtender.ReadIni(BridgePath, BridgeIni, "speaking", "wem_ready")
            If speakingActive == "1" && wemReady == "1" && CurrentSpeakerActor != None
                String durationStr = CassiopeiaPapyrusExtender.ReadIni(BridgePath, BridgeIni, "speaking", "duration")
                Float duration = durationStr as Float
                If duration <= 0.0
                    duration = 2.0
                EndIf
                CassiopeiaPapyrusExtender.RotateActor(CurrentSpeakerActor, CurrentSpeakerActor.GetAngleZ() + CurrentSpeakerActor.GetHeadingAngle(Game.GetPlayer()), 0.0)
                If MantellaScene != None
                    MantellaScene.Stop()
                EndIf
                If MantellaSpeakerAlias != None && CurrentSpeakerActor != None
                    MantellaSpeakerAlias.ForceRefTo(CurrentSpeakerActor)
                    MantellaScene.ForceStart()
                    Debug.Trace("Mantella: Scene started for " + CurrentSpeaker + " duration=" + duration)
                Else
                    Debug.Trace("Mantella: Could not start scene - alias or actor is None")
                EndIf
                CassiopeiaPapyrusExtender.WriteIni(BridgePath, BridgeIni, "speaking", "active", "0")
                CassiopeiaPapyrusExtender.WriteIni(BridgePath, BridgeIni, "speaking", "wem_ready", "0")
                StartTimer(duration, 2)
            EndIf
        EndIf
        StartTimer(0.5, 1)
    ElseIf aiTimerID == 2
        If MantellaScene != None
            MantellaScene.Stop()
            Debug.Trace("Mantella: Scene stopped after voiceline")
        EndIf
    ElseIf aiTimerID == 3
        RegisterForEvents()
        String currentType = CassiopeiaPapyrusExtender.ReadIni(BridgePath, BridgeIni, "request", "type")
        If currentType == "reinit"
            CassiopeiaPapyrusExtender.WriteIni(BridgePath, BridgeIni, "request", "type", "idle")
            Debug.Trace("Mantella: Reinit flag cleared, system ready")
        EndIf
        If currentType == "start" && ConversationActive
            StartTimer(0.5, 1)
        EndIf
        StartTimer(0.5, 3)
    EndIf
EndEvent

Function TextInputMenu_EndEditText(String sInputText) global
    Quest mantellaQuest = Game.GetFormFromFile(0x00000807, "MantellaStarfield.esp") as Quest
    If mantellaQuest == None
        Return
    EndIf
    User:MantellaDialogueQuestScript script = mantellaQuest as User:MantellaDialogueQuestScript
    If script == None
        Return
    EndIf

    ; Handle rename flow
    If script.IsRenaming
        script.IsRenaming = False
        If sInputText != "" && script.RenameTargetActor != None
            String refIDHex = CassiopeiaPapyrusExtender.GetHexFormID(script.RenameTargetActor as Form)
            CassiopeiaPapyrusExtender.WriteIni(script.NamesPath, script.NamesIni, "Names", refIDHex, sInputText)
            Debug.Notification("Mantella: Renamed to " + sInputText)
            Debug.Trace("Mantella: Saved rename " + refIDHex + " = " + sInputText)
        Else
            Debug.Notification("Mantella: Rename cancelled")
        EndIf
        script.RenameTargetActor = None
        Return
    EndIf

    ; Handle normal player input accumulation
    If sInputText != "" && script.ConversationActive && script.IsAccumulating
        If script.AccumulatedInput == ""
            script.AccumulatedInput = sInputText
        Else
            script.AccumulatedInput = script.AccumulatedInput + " " + sInputText
        EndIf
        Debug.Notification("Mantella: Press T for more, G to send")
        Debug.Trace("Mantella: Accumulated so far: " + script.AccumulatedInput)
    EndIf
EndFunction
