"""Real PCM envelope and scoped PTT feedback lifecycle, compiled as C11."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def test_real_pcm_and_feedback_lifecycle():
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'voice-feedback'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'main'),
                        str(ROOT / 'main/pet_p4_voice_feedback.c'),
                        str(ROOT / 'tests/p4_voice_feedback_test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
