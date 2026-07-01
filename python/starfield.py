import os
import shutil
import threading
import subprocess
import configparser
import queue as audio_queue_module
from pathlib import Path
from typing import Any, TYPE_CHECKING

if TYPE_CHECKING:
    from src.conversation.context import Context
from src.character_manager import Character
from src.config.config_loader import ConfigLoader
from src.llm.sentence import Sentence
from src.games.external_character_info import external_character_info
from src.games.gameable import Gameable
import src.utils as utils
from src.config.definitions.tts_definitions import TTSEnum

logger = utils.get_logger()

WWISE_CONSOLE = r"C:\Audiokinetic\Wwise_2021.1.14.8108\Authoring\x64\Release\bin\WwiseConsole.exe"
WWISE_PROJ = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\wwise\Starfield\Starfield.wproj"
LIPGENERATOR_EXE = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\LipGenerator\LipGenerator.exe"
FFXC_EXE = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\FaceFX\ffxc.exe"
FACEFX_FEMALE = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\FaceFX\StarfieldHumanFemale.facefx"
FACEFX_MALE = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\FaceFX\StarfieldHumanMale.facefx"
FACEFX_ANALYSIS = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\FaceFX\GenesisDefault.facefx"
INTERMEDIATE_FEMALE = r"E:\SteamLibrary\steamapps\common\Starfield\Intermediate\Slice_01"
INTERMEDIATE_MALE = r"E:\SteamLibrary\steamapps\common\Starfield\Intermediate\Slice_00"
DATA_VOICE_BASE = r"E:\SteamLibrary\steamapps\common\Starfield\Data\Sound\Voice\MantellaStarfield.esp"
MO2_VOICE_BASE = r"E:\Star Wars Genesis\Game\mods\MantellaStarfield\Sound\Voice\MantellaStarfield.esp"
WSOURCES_TEMP = r"C:\Temp\mantella_sources.wsources"

# Registered folders - must match what's in the ffxcproj animset
REGISTERED_FEMALE_FOLDER = "NPCFSarahMorgan"
REGISTERED_MALE_FOLDER = "NPCMBarrett"

# Fallback voice types
DEFAULT_FEMALE_VOICE_TYPE = "NPCFSarahMorgan"
DEFAULT_MALE_VOICE_TYPE = "NPCMBarrett"

# Mapping of NPC name -> Starfield voice type folder name
NPC_VOICE_TYPE_MAP = {
    "Sarah Morgan": "NPCFSarahMorgan",
    "Noel": "NPCFNoel",
    "Walter Stroud": "NPCMWalterStroud",
    "Sam Coe": "NPCMSamCoe",
    "Andreja": "NPCFAndreja",
    "Barrett": "NPCMBarrett",
    "Vasco": "RobotModelAVasco",
    "Matteo Khatri": "NPCMMatteoKhatri",
    "Cora Coe": "NPCFCoraCoe",
    "Vladimir Sall": "NPCMVladimirSall",
    "Heller": "NPCMHeller",
    "Lin": "NPCFLin",
    "Grand Inquisitor": "NPCMGrandInquisitor",
    "Mara Jade": "NPCFMuria",
    "Luke Skywalker": "NPCMLukeSkywalker",
}

# Voice types that use male face rig
MALE_VOICE_TYPES = {
    "NPCMBarrett", "NPCMWalterStroud", "NPCMSamCoe", "NPCMVladimirSall",
    "NPCMMatteoKhatri", "NPCMHeller", "GenericMaleEvenToned",
    "NPCMGrandInquisitor", "NPCMLukeSkywalker",
}


class Starfield(Gameable):
    DIALOGUELINE1_FILENAME = "00666A79"
    DIALOGUELINE2_FILENAME = "00666A79"
    MANTELLA_ESP_VOICE_FOLDER = "MantellaStarfield.esp"
    MANTELLA_VOICE_TYPE = "NPCFSarahMorgan"

    # Rotating wem filename pool — each slot gets a fresh Wwise cache entry
    WEM_SLOTS = [
        "00666A79", "00666A7A", "00666A7B", "00666A7C", "00666A7D",
        "00666A7E", "00666A7F", "00666A80", "00666A81", "00666A82",
        "00666A83", "00666A84", "00666A85", "00666A86", "00666A87",
        "00666A88", "00666A89", "00666A8A", "00666A8B", "00666A8C",
        "00666A8D", "00666A8E", "00666A8F", "00666A90", "00666A91",
        "00666A92", "00666A93", "00666A94", "00666A95", "00666A96",
        "00666A97", "00666A98", "00666A99", "00666A9A", "00666A9B",
        "00666A9C", "00666A9D", "00666A9E", "00666A9F", "00666AA0",
        "00666AA1", "00666AA2", "00666AA3", "00666AA4", "00666AA5",
        "00666AA6", "00666AA7", "00666AA8", "00666AA9", "00666AAA",
        "00666AAB", "00666AAC", "00666AAD", "00666AAE", "00666AAF",
        "00666AB0", "00666AB1", "00666AB2", "00666AB3", "00666AB4",
        "00666AB5", "00666AB6", "00666AB7", "00666AB8", "00666AB9",
        "00666ABA", "00666ABB", "00666ABC", "00666ABD", "00666ABE",
        "00666ABF", "00666AC0", "00666AC1", "00666AC2", "00666AC3",
        "00666AC4", "00666AC5", "00666AC6", "00666AC7", "00666AC8",
        "00666AC9", "00666ACA", "00666ACB", "00666ACC", "00666ACD",
        "00666ACE", "00666ACF", "00666AD0", "00666AD1", "00666AD2",
        "00666AD3", "00666AD4", "00666AD5", "00666AD6", "00666AD7",
        "00666AD8", "00666AD9", "00666ADA", "00666ADB", "00666ADC",
        "00666ADD", "00666ADE", "00666ADF", "00666AE0", "00666AE1",
        "00666AE2", "00666AE3", "00666AE4", "00666AE5", "00666AE6",
        "00666AE7", "00666AE8", "00666AE9", "00666AEA", "00666AEB",
        "00666AEC", "00666AED", "00666AEE", "00666AEF", "00666AF0",
        "00666AF1", "00666AF2", "00666AF3", "00666AF4", "00666AF5",
        "00666AF6", "00666AF7", "00666AF8", "00666AF9", "00666AFA",
        "00666AFB", "00666AFC", "00666AFD", "00666AFE", "00666AFF",
        "00666B00", "00666B01", "00666B02", "00666B03", "00666B04",
        "00666B05", "00666B06", "00666B07", "00666B08", "00666B09",
        "00666B0A", "00666B0B", "00666B0C", "00666B0D", "00666B0E",
        "00666B0F", "00666B10", "00666B11", "00666B12", "00666B13",
        "00666B14", "00666B15", "00666B16", "00666B17", "00666B18",
        "00666B19", "00666B1A", "00666B1B", "00666B1C", "00666B1D",
        "00666B1E", "00666B1F", "00666B20", "00666B21", "00666B22",
        "00666B23", "00666B24", "00666B25", "00666B26", "00666B27",
        "00666B28", "00666B29", "00666B2A", "00666B2B", "00666B2C",
        "00666B2D", "00666B2E", "00666B2F", "00666B30", "00666B31",
        "00666B32", "00666B33", "00666B34", "00666B35", "00666B36",
        "00666B37", "00666B38", "00666B39", "00666B3A", "00666B3B",
        "00666B3C", "00666B3D", "00666B3E", "00666B3F", "00666B40",
    ]
    _wem_slot_index = -1

    def __init__(self, config: ConfigLoader):
        super().__init__(config, 'data/Starfield/starfield_characters.csv', "Starfield")
        self.__tts_service: TTSEnum = config.tts_service
        self.__image_analysis_filepath = ""
        os.makedirs("C:\\Temp", exist_ok=True)
        self._preload_voice_folders()

        # Reset wem slot index to -1 so first increment lands on slot 0 (00666A79)
        Starfield._wem_slot_index = -1
        # Clear the line queue on startup so no stale lines from a prior run remain.
        # (C++ also clears it at conversation start; this keeps the file tidy.)
        queue_ini = r"E:\Star Wars Genesis\Game\overwrite\SFSE\MantellaStarfield\MantellaQueue.ini"
        try:
            qf = configparser.ConfigParser()
            qf.add_section("queue")
            qf.set("queue", "write_index", "0")
            qf.set("queue", "read_index", "0")
            with open(queue_ini, 'w') as f:
                qf.write(f)
        except Exception:
            pass
        logger.log(23, "Reset wem slot to -1, cleared line queue")

    def _preload_voice_folders(self):
        """Pre-deploy placeholder wem+ffxanim to all NPC folders for all wem slots."""
        female_wem = os.path.join(DATA_VOICE_BASE, REGISTERED_FEMALE_FOLDER,
                                   f"{self.DIALOGUELINE1_FILENAME}.wem")
        female_ffxanim = os.path.join(DATA_VOICE_BASE, REGISTERED_FEMALE_FOLDER,
                                       f"{self.DIALOGUELINE1_FILENAME}.ffxanim")
        male_wem = os.path.join(DATA_VOICE_BASE, REGISTERED_MALE_FOLDER,
                                 f"{self.DIALOGUELINE1_FILENAME}.wem")
        male_ffxanim = os.path.join(DATA_VOICE_BASE, REGISTERED_MALE_FOLDER,
                                     f"{self.DIALOGUELINE1_FILENAME}.ffxanim")

        for npc_name, voice_type in NPC_VOICE_TYPE_MAP.items():
            is_male = self._is_male_voice_type(voice_type)
            src_wem = male_wem if is_male else female_wem
            src_ffxanim = male_ffxanim if is_male else female_ffxanim
            mo2_path = os.path.join(MO2_VOICE_BASE, voice_type)
            data_path = os.path.join(DATA_VOICE_BASE, voice_type)
            os.makedirs(mo2_path, exist_ok=True)
            os.makedirs(data_path, exist_ok=True)

            for slot in self.WEM_SLOTS:
                for src, ext in [(src_wem, "wem"), (src_ffxanim, "ffxanim")]:
                    if not os.path.exists(src):
                        continue
                    for dst_base in [mo2_path, data_path]:
                        dst = os.path.join(dst_base, f"{slot}.{ext}")
                        if not os.path.exists(dst):
                            try:
                                shutil.copyfile(src, dst)
                            except Exception as e:
                                logger.warning(f"Failed to pre-deploy {slot}.{ext} for {npc_name}: {e}")
            logger.log(23, f"Pre-deployed {len(self.WEM_SLOTS)} wem slots for {npc_name}")

    @property
    def extender_name(self) -> str:
        return 'SFSE'

    @property
    def game_name_in_filepath(self) -> str:
        return 'starfield'

    @property
    def image_path(self) -> str:
        return self.__image_analysis_filepath

    def modify_sentence_text_for_game(self, text: str) -> str:
        starfield_max_characters = 500
        if len(text) > starfield_max_characters:
            abbreviated = text[0:starfield_max_characters - 3] + "..."
            return abbreviated
        return text

    @utils.time_it
    def load_external_character_info(self, base_id: str, name: str, race: str, gender: int, ingame_voice_model: str) -> external_character_info:
        character_info, is_generic_npc = self.find_character_info(base_id, name, race, gender, ingame_voice_model)
        parts = ingame_voice_model.split('<')
        actor_voice_model_name = parts[1].split(' ')[0] if len(parts) > 1 else ingame_voice_model
        bio = character_info["bio"]
        llm_service_value = utils.safe_str(character_info.get('llm_service', ''))
        llm_model_value = utils.safe_str(character_info.get('model', ''))
        tts_service_value = utils.safe_str(character_info.get('tts_service', ''))
        return external_character_info(
            name, is_generic_npc, bio, actor_voice_model_name,
            character_info['voice_model'], character_info['starfield_voice_folder'],
            character_info['advanced_voice_model'], character_info.get('voice_accent', None),
            llm_service=llm_service_value, llm_model=llm_model_value, tts_service=tts_service_value
        )

    @utils.time_it
    def find_best_voice_model(self, actor_race: str | None, actor_sex: int | None, ingame_voice_model: str, library_search: bool = True) -> str:
        voice_model = ''
        actor_voice_model = ingame_voice_model
        if '<' in actor_voice_model:
            actor_voice_model_name = actor_voice_model.split('<')[1].split(' ')[0]
        else:
            actor_voice_model_name = actor_voice_model
        if actor_race and 'Race <' in actor_race:
            actor_race = actor_race.split('Race <', 1)[1].split(' ')[0]
            if actor_race.endswith('Race'):
                actor_race = actor_race[:actor_race.rfind('Race')].strip()
        if self.__tts_service == TTSEnum.XVASYNTH:
            male_voice_model_dictionary = Starfield.MALE_VOICE_MODELS_XVASYNTH
            female_voice_model_dictionary = Starfield.FEMALE_VOICE_MODELS_XVASYNTH
        elif self.__tts_service == TTSEnum.PIPER:
            male_voice_model_dictionary = Starfield.MALE_VOICE_MODELS_PIPER
            female_voice_model_dictionary = Starfield.FEMALE_VOICE_MODELS_PIPER
        else:
            male_voice_model_dictionary = Starfield.MALE_VOICE_MODELS_XTTS
            female_voice_model_dictionary = Starfield.FEMALE_VOICE_MODELS_XTTS
        if library_search:
            try:
                voice_model = self.character_df.loc[
                    self.character_df['starfield_voice_folder'].astype(str).str.lower() == actor_voice_model_name.lower(),
                    'voice_model'
                ].values[0]
                return voice_model
            except (IndexError, KeyError):
                pass
        if voice_model == '':
            voice_model = self.dictionary_match(female_voice_model_dictionary, male_voice_model_dictionary, actor_race, actor_sex)
        return voice_model

    def dictionary_match(self, female_voice_model_dictionary: dict, male_voice_model_dictionary: dict, actor_race: str | None, actor_sex: int | None) -> str:
        if actor_race is None:
            actor_race = "Human"
        if actor_sex is None:
            actor_sex = 0
        modified_race_key = actor_race + "Race"
        if actor_sex == 1:
            voice_model = female_voice_model_dictionary.get(modified_race_key, 'Female Human')
        else:
            voice_model = male_voice_model_dictionary.get(modified_race_key, 'Male Human')
        return voice_model

    @utils.time_it
    def load_unnamed_npc(self, name: str, actor_race: str, actor_sex: int, ingame_voice_model: str) -> dict[str, Any]:
        voice_model = self.find_best_voice_model(actor_race, actor_sex, ingame_voice_model)
        try:
            starfield_voice_folder = self.character_df.loc[
                self.character_df['voice_model'].astype(str).str.lower() == voice_model.lower(),
                'starfield_voice_folder'
            ].values[0]
        except (IndexError, KeyError):
            starfield_voice_folder = voice_model.replace(' ', '')
        return {
            'name': name,
            'bio': f'You are a {"male" if actor_sex == 0 else "female"} {actor_race if actor_race.lower() != name.lower() else ""} {name}.',
            'voice_model': voice_model,
            'advanced_voice_model': '',
            'voice_accent': 'en',
            'starfield_voice_folder': starfield_voice_folder,
        }

    def _get_voice_type(self, speaker_name: str, gender: int) -> str:
        voice_type = NPC_VOICE_TYPE_MAP.get(speaker_name)
        if voice_type:
            return voice_type
        if gender == 1:
            return DEFAULT_FEMALE_VOICE_TYPE
        return DEFAULT_MALE_VOICE_TYPE

    def _ensure_voice_type_preloaded(self, voice_type: str):
        """Ensure placeholder wems exist for a voice type — for custom NPCs not in NPC_VOICE_TYPE_MAP."""
        mo2_path = os.path.join(MO2_VOICE_BASE, voice_type)
        data_path = os.path.join(DATA_VOICE_BASE, voice_type)
        os.makedirs(mo2_path, exist_ok=True)
        os.makedirs(data_path, exist_ok=True)
        base_wem = os.path.join(mo2_path, "00666A79.wem")
        if not os.path.exists(base_wem):
            is_male = self._is_male_voice_type(voice_type)
            src_wem = os.path.join(DATA_VOICE_BASE,
                REGISTERED_MALE_FOLDER if is_male else REGISTERED_FEMALE_FOLDER,
                "00666A79.wem")
            src_ffxanim = os.path.join(DATA_VOICE_BASE,
                REGISTERED_MALE_FOLDER if is_male else REGISTERED_FEMALE_FOLDER,
                "00666A79.ffxanim")
            for slot in self.WEM_SLOTS:
                for src, ext in [(src_wem, "wem"), (src_ffxanim, "ffxanim")]:
                    if not os.path.exists(src):
                        continue
                    for dst_base in [mo2_path, data_path]:
                        dst = os.path.join(dst_base, f"{slot}.{ext}")
                        if not os.path.exists(dst):
                            try:
                                shutil.copyfile(src, dst)
                            except Exception as e:
                                logger.warning(f"Failed to preload {slot}.{ext} for {voice_type}: {e}")
            logger.log(23, f"Auto-preloaded slots for new voice type: {voice_type}")

    def _is_male_voice_type(self, voice_type: str) -> bool:
        if voice_type in MALE_VOICE_TYPES:
            return True
        # Auto-detect from naming convention: NPCM = male, NPCF = female
        if voice_type.startswith("NPCM") or voice_type.startswith("GenericMale"):
            return True
        return False

    def _generate_wsources(self, wav_path: str, filename: str, voice_folder: str) -> str:
        """Generate a .wsources XML file for WwiseConsole external source conversion."""
        xml = f'''<?xml version="1.0" encoding="UTF-8"?>
<ExternalSourcesList SchemaVersion="1" Root="{voice_folder}">
    <Source Path="{filename}.wav" Conversion="Vorbis Quality High" Destination="{filename}"/>
</ExternalSourcesList>'''
        with open(WSOURCES_TEMP, 'w', encoding='utf-8') as f:
            f.write(xml)
        return WSOURCES_TEMP

    def _delete_wem(self, path: str):
        """Safely delete a wem file to force Wwise to reopen it next play."""
        try:
            if os.path.exists(path):
                os.remove(path)
                logger.log(23, f"Deleted wem to force Wwise reopen: {path}")
        except Exception as e:
            logger.warning(f"Could not delete wem {path}: {e}")

    def _generate_voice_assets(self, audio_file: str, filename: str, voice_type: str, text: str = ""):
        """Generate wem+ffxanim from TTS wav. Deploy to both Data folder and MO2."""
        is_male = self._is_male_voice_type(voice_type)
        facefx_actor = FACEFX_MALE if is_male else FACEFX_FEMALE
        facefx_actor_name = "StarfieldHumanMale" if is_male else "StarfieldHumanFemale"
        intermediate_dir = INTERMEDIATE_MALE if is_male else INTERMEDIATE_FEMALE
        registered_folder = REGISTERED_MALE_FOLDER if is_male else REGISTERED_FEMALE_FOLDER
        registered_data_path = os.path.join(DATA_VOICE_BASE, registered_folder)
        mo2_voice_path = os.path.join(MO2_VOICE_BASE, voice_type)
        data_voice_path = os.path.join(DATA_VOICE_BASE, voice_type)
        animation_group = f"{registered_folder}_{filename}"

        try:
            os.makedirs(registered_data_path, exist_ok=True)
            os.makedirs(intermediate_dir, exist_ok=True)
            os.makedirs(mo2_voice_path, exist_ok=True)
            os.makedirs(data_voice_path, exist_ok=True)

            # ----------------------------------------------------------------
            # Step 1: Copy TTS wav to registered Data folder
            # ----------------------------------------------------------------
            registered_wav = os.path.join(registered_data_path, f"{filename}.wav")
            shutil.copyfile(audio_file, registered_wav)
            logger.log(23, f"Copied wav to: {registered_wav}")

            # ----------------------------------------------------------------
            # Step 2: Generate wem via WwiseConsole + wsources
            # ----------------------------------------------------------------
            wsources_path = self._generate_wsources(registered_wav, filename, registered_data_path)

            wem_windows_path = os.path.join(registered_data_path, "Windows", f"{filename}.wem")
            if os.path.exists(wem_windows_path):
                os.remove(wem_windows_path)

            result = subprocess.run(
                [
                    WWISE_CONSOLE,
                    "convert-external-source",
                    WWISE_PROJ,
                    "--source-file", wsources_path,
                    "--output", registered_data_path,
                    "--platform", "Windows",
                ],
                capture_output=True, timeout=60
            )
            if result.returncode != 0:
                logger.warning(f"WwiseConsole failed: {result.stderr.decode()}")
            else:
                logger.log(23, f"WwiseConsole generated wem for {filename}")

            wem_src = wem_windows_path
            if not os.path.exists(wem_src):
                found = list(Path(registered_data_path).rglob(f"{filename}.wem"))
                if found:
                    wem_src = str(max(found, key=lambda p: p.stat().st_mtime))
                else:
                    logger.warning(f"wem not found after WwiseConsole run")
                    wem_src = None

            if wem_src and os.path.exists(wem_src):
                wem_mo2 = os.path.join(mo2_voice_path, f"{filename}.wem")
                wem_data = os.path.join(data_voice_path, f"{filename}.wem")
                shutil.copyfile(wem_src, wem_mo2)
                logger.log(23, f"Deployed {filename}.wem to MO2 ({voice_type}, {os.path.getsize(wem_mo2)} bytes)")
                shutil.copyfile(wem_src, wem_data)
                logger.log(23, f"Deployed {filename}.wem to Data ({voice_type})")
            else:
                logger.warning(f"Skipping wem deploy - file not found")

            # ----------------------------------------------------------------
            # Step 3: Generate animset via LipGenerator
            # ----------------------------------------------------------------
            animset_path = os.path.join(intermediate_dir, f"{animation_group}.animset")
            safe_text = text.strip() if text.strip() else "dialogue"

            result = subprocess.run(
                [
                    LIPGENERATOR_EXE,
                    registered_wav,
                    safe_text,
                    facefx_actor,
                    FACEFX_ANALYSIS,
                    f"-OutputFileName:{animset_path}",
                    f"-AnimationGroupName:{animation_group}",
                ],
                capture_output=True, timeout=60
            )
            if not os.path.exists(animset_path):
                logger.warning(f"LipGenerator failed to produce animset for {filename}")
                return
            logger.log(23, f"LipGenerator generated animset ({os.path.getsize(animset_path)} bytes)")

            # ----------------------------------------------------------------
            # Step 4: Compile animset to ffxanim via ffxc
            # Write a PER-LINE proj file (not the shared Slice_00/01.ffxcproj) so
            # ffxc calls for different lines never collide on the same file. This is
            # what makes parallel/ahead-of-time compilation possible — each line's
            # ffxc reads its own proj pointing at its own animset.
            # ----------------------------------------------------------------
            per_line_proj = os.path.join(intermediate_dir, f"{animation_group}.ffxcproj")
            proj_yaml = (
                "version: 10300\n"
                "items:\n"
                f" - facefx_file: {facefx_actor}\n"
                "   mount_files:\n"
                f"    - {animset_path}\n"
            )
            try:
                with open(per_line_proj, 'w', encoding='utf-8') as f:
                    f.write(proj_yaml)
            except Exception as e:
                logger.warning(f"Could not write per-line ffxcproj: {e}")
                return

            result = subprocess.run(
                [FFXC_EXE, f"-o={registered_data_path}\\", "-p=x86", "--rebuild", per_line_proj],
                capture_output=True, timeout=60
            )
            if result.returncode != 0:
                logger.warning(f"ffxc failed: {result.stderr.decode()}")
                return
            logger.log(23, f"ffxc compiled ffxanim for {filename}")

            # ----------------------------------------------------------------
            # Step 5: Find ffxanim and deploy to both MO2 and Data
            # ----------------------------------------------------------------
            ffxanim_src = os.path.join(
                registered_data_path, facefx_actor_name, "x86",
                animation_group, f"{filename}.ffxanim"
            )
            if not os.path.exists(ffxanim_src):
                found = list(Path(registered_data_path).rglob(f"{filename}.ffxanim"))
                if found:
                    ffxanim_src = str(max(found, key=lambda p: p.stat().st_size))
                else:
                    base_ffxanim = os.path.join(
                        registered_data_path, facefx_actor_name, "x86",
                        f"{registered_folder}_00666A79", "00666A79.ffxanim"
                    )
                    if os.path.exists(base_ffxanim):
                        ffxanim_src = base_ffxanim
                        logger.log(23, f"Using base ffxanim as fallback for {filename}")
                    else:
                        logger.warning(f"ffxanim not found after ffxc run")
                        return

            ffxanim_mo2 = os.path.join(mo2_voice_path, f"{filename}.ffxanim")
            shutil.copyfile(ffxanim_src, ffxanim_mo2)
            logger.log(23, f"Deployed {filename}.ffxanim to MO2 ({voice_type}, {os.path.getsize(ffxanim_mo2)} bytes)")

            ffxanim_data = os.path.join(data_voice_path, f"{filename}.ffxanim")
            shutil.copyfile(ffxanim_src, ffxanim_data)
            logger.log(23, f"Deployed {filename}.ffxanim to Data ({voice_type})")

            # NOTE: The 00666A79.ffxanim "activation" swap is NO LONGER done here.
            # Generation may run several lines ahead of playback, so overwriting the
            # shared 00666A79 read-slot during generation would put the LAST generated
            # line's lips on the slot — corrupting earlier lines still waiting to play.
            # Instead, the C++ plugin copies THIS line's own-slot ffxanim onto
            # 00666A79.ffxanim at the moment the line actually starts (keyed to the
            # line's wem path), so each line gets its correct lips at play time.
            # Each line's ffxanim remains deployed to its own slot above for C++ to use.

        except subprocess.TimeoutExpired:
            logger.warning(f"Voice asset generation timed out for {filename}")
        except Exception as e:
            logger.warning(f"Failed to generate voice assets: {e}")

    @utils.time_it
    def prepare_sentence_for_game(self, queue_output: Sentence, context_of_conversation: 'Context', config: ConfigLoader, topicID: int, isFirstLine: bool = False):
        audio_file = queue_output.voice_file
        if not os.path.exists(audio_file):
            return

        speaker: Character = queue_output.speaker

        gender = 1 if hasattr(speaker, 'gender') and speaker.gender == 1 else 0
        voice_type = self._get_voice_type(speaker.name, gender)
        logger.log(23, f"Voice type for {speaker.name}: {voice_type}")

        # Ensure voice type folder has placeholder wems (for custom NPCs)
        self._ensure_voice_type_preloaded(voice_type)

        text = getattr(queue_output, 'text', '') or ''

        # Rotate to next wem slot — unique filename = fresh Wwise cache entry
        Starfield._wem_slot_index = (Starfield._wem_slot_index + 1) % len(Starfield.WEM_SLOTS)
        filename = Starfield.WEM_SLOTS[Starfield._wem_slot_index]
        logger.log(23, f"Using wem slot: {filename} (index {Starfield._wem_slot_index})")

        # NOTE: do NOT reset wem_ready here. C++ clears wem_ready=0 itself once it has
        # captured a line's path and activated its lips. The write below waits for that
        # 0 so we never overwrite a line's wem_path before C++ has consumed it.

        # Generate wem+ffxanim and deploy to MO2 and Data folder
        self._generate_voice_assets(audio_file, filename, voice_type, text)

        # ----------------------------------------------------------------
        # Enqueue this line into the QUEUE file as its own [line_N] section.
        # Per-line identity: each line is a distinct, never-overwritten section,
        # so C++ can play them strictly in order (by index) with no clobbering
        # and no single-slot race. C++ is the sole reader; Python the sole writer.
        # ----------------------------------------------------------------
        wem_mo2 = os.path.join(MO2_VOICE_BASE, voice_type, f"{filename}.wem")
        queue_ini = r"E:\Star Wars Genesis\Game\overwrite\SFSE\MantellaStarfield\MantellaQueue.ini"

        # Determine the line's playback duration (seconds).
        # Prefer a value Mantella already computed on the Sentence object; fall
        # back to reading the wav. Always sanity-clamp — a bad header must never
        # produce a huge sleep that freezes playback.
        duration = None
        for _attr in ("voice_line_duration", "_voice_line_duration",
                      "duration", "_duration", "length", "_length"):
            _v = getattr(queue_output, _attr, None)
            if isinstance(_v, (int, float)) and 0 < _v < 600:
                duration = float(_v)
                break

        if duration is None:
            # Read the wav by parsing the header directly. The stdlib `wave`
            # module misreads some Cartesia outputs (returned a fixed garbage
            # frame count). Parsing the RIFF 'data' chunk size / byte-rate is
            # robust to that.
            try:
                with open(audio_file, 'rb') as _f:
                    _hdr = _f.read(44)  # standard 44-byte PCM wav header
                if len(_hdr) >= 44 and _hdr[0:4] == b'RIFF' and _hdr[8:12] == b'WAVE':
                    import struct as _struct
                    byte_rate = _struct.unpack('<I', _hdr[28:32])[0]   # bytes/sec
                    # Find the data chunk size (usually at offset 40 for canonical header)
                    data_size = _struct.unpack('<I', _hdr[40:44])[0]
                    if byte_rate > 0 and 0 < data_size < (1 << 30):
                        duration = data_size / float(byte_rate)
            except Exception:
                duration = None

        # Sanity clamp: real voicelines are a fraction of a second up to ~30s.
        # Anything outside that is a bad read — fall back to a safe estimate from
        # the file size, or a small default, so C++ never sleeps for hours.
        if duration is None or not (0.1 <= duration <= 60.0):
            try:
                _bytes = os.path.getsize(audio_file)
                # ~32 KB/s for 16-bit 16kHz mono PCM; rough but bounded.
                duration = max(0.8, min(30.0, _bytes / 32000.0))
            except Exception:
                duration = 2.0

        q = configparser.ConfigParser()
        q.read(queue_ini)
        if not q.has_section("queue"):
            q.add_section("queue")
        try:
            write_index = int(q.get("queue", "write_index", fallback="0"))
        except Exception:
            write_index = 0

        sec = f"line_{write_index}"
        if not q.has_section(sec):
            q.add_section(sec)
        q.set(sec, "wem_path", wem_mo2)
        q.set(sec, "subtitle", text)
        q.set(sec, "duration", f"{duration:.3f}")
        q.set(sec, "type", "npc_talk")

        q.set("queue", "write_index", str(write_index + 1))
        with open(queue_ini, 'w') as qf:
            q.write(qf)
        logger.log(23, f"Enqueued line_{write_index} ({duration:.2f}s): {wem_mo2}")

        logger.log(23, f"{speaker.name} should speak")

    @utils.time_it
    def is_sentence_allowed(self, text: str, count_sentence_in_text: int) -> bool:
        if ('assist' in text) and (count_sentence_in_text > 0):
            logger.log(23, f"'assist' keyword found. Ignoring sentence: {text.strip()}")
            return False
        return True

    @utils.time_it
    def get_weather_description(self, weather_attributes: dict[str, Any]) -> str:
        return "The weather is clear."

    MALE_VOICE_MODELS_XTTS: dict[str, str] = {
        'HumanRace': 'Male Human', 'HumanChildRace': 'Male Human',
        'RobotRace': 'Male Robot', 'AlienRace': 'Male Alien',
    }
    FEMALE_VOICE_MODELS_XTTS: dict[str, str] = {
        'HumanRace': 'Female Human', 'HumanChildRace': 'Female Human',
        'RobotRace': 'Female Robot', 'AlienRace': 'Female Alien',
    }
    MALE_VOICE_MODELS_XVASYNTH: dict[str, str] = {
        'HumanRace': 'Male Human', 'HumanChildRace': 'Male Human',
        'RobotRace': 'Male Robot', 'AlienRace': 'Male Alien',
    }
    FEMALE_VOICE_MODELS_XVASYNTH: dict[str, str] = {
        'HumanRace': 'Female Human', 'HumanChildRace': 'Female Human',
        'RobotRace': 'Female Robot', 'AlienRace': 'Female Alien',
    }
    MALE_VOICE_MODELS_PIPER: dict[str, str] = {
        'HumanRace': 'Male Human', 'HumanChildRace': 'Male Human',
        'RobotRace': 'Male Robot', 'AlienRace': 'Male Alien',
    }
    FEMALE_VOICE_MODELS_PIPER: dict[str, str] = {
        'HumanRace': 'Female Human', 'HumanChildRace': 'Female Human',
        'RobotRace': 'Female Robot', 'AlienRace': 'Female Alien',
    }