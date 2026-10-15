from pathlib import Path
from unittest import TestCase


ROOT = Path(__file__).resolve().parents[2]


class StoryStorageContractTest(TestCase):
    def test_story_files_are_separate_from_music_track_scanning(self) -> None:
        player = (
            ROOT / "Multicore_top_CPU1/src/thread_code/audio_wav_player.c"
        ).read_text(encoding="utf-8")

        self.assertIn(
            '#define AUDIO_WAV_PLAYER_STORY_DIR       SD_CARD_MOUNT_PATH "/STORY"',
            player,
        )
        self.assertIn('"%s/%s", AUDIO_WAV_PLAYER_STORY_DIR, name', player)
        self.assertIn('AUDIO_WAV_PLAYER_STORY_DIR, story_name', player)
        self.assertIn("!audio_story_name_valid(find_data.pcFileName)", player)

