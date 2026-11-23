import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave
from sound_effects import generate, check_budget, MAX_BYTES


def spec(ms=120):
    return {'cues': [{'id': 'confirm', 'segments': [{'wave': 'sine', 'ms': ms, 'hz': [500, 1000], 'volume': 0.4}]}]}


class Effects(unittest.TestCase):
    def test_default_volume_is_eighty_percent_and_explicit_mute_is_preserved(self):
        for encoding in ['wav', 'hab']:
            with self.subTest(encoding=encoding), tempfile.TemporaryDirectory() as tmp:
                data = spec(100)
                part = data['cues'][0]['segments'][0]
                part['hz'] = [1000, 1000]
                del part['volume']
                generate(data, tmp, encoding)
                self.assertNotIn('volume', part)
                if encoding == 'wav':
                    with wave.open(str(Path(tmp) / 'confirm.wav')) as sound:
                        pcm = struct.unpack('<1600h', sound.readframes(1600))
                    self.assertEqual(max(map(abs, pcm)), 26214)
                    self.assertEqual((pcm[0], pcm[-1]), (0, 0))
                else:
                    self.assertTrue((Path(tmp) / 'speech.hab').read_bytes().startswith(b'HAB1'))
                part['volume'] = 0
                generate(data, tmp, 'wav')
                with wave.open(str(Path(tmp) / 'confirm.wav')) as sound:
                    self.assertEqual(set(sound.readframes(1600)), {0})

    def test_wav_is_audible_bounded_mono_and_fades_to_silence(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = generate(spec(), tmp, 'wav')
            data = (Path(tmp) / 'confirm.wav').read_bytes()
            self.assertEqual(report['bytes'], len(data))
            with wave.open(io.BytesIO(data)) as sound:
                self.assertEqual((sound.getnchannels(), sound.getsampwidth(), sound.getframerate()), (1, 2, 16000))
                pcm = struct.unpack('<1920h', sound.readframes(1920))
            self.assertEqual((pcm[0], pcm[-1]), (0, 0))
            self.assertGreater(max(pcm), 5000)
            self.assertLessEqual(max(map(abs, pcm)), 13107)
            self.assertLessEqual(check_budget([tmp])['bytes'], MAX_BYTES)

    def test_hab_is_accepted_and_decoded_by_the_actual_browser_player(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = generate(spec(), tmp, 'hab')
            self.assertLess(report['bytes'], 1100)
            repo = Path(__file__).resolve().parents[4]
            module = repo / 'pc/src/component-center/wasm-preview/audio-bank.mjs'
            script = """import fs from 'node:fs';
import {loadAudioBank} from MODULE;
const b=fs.readFileSync(process.argv[1]);const pcm=loadAudioBank(b.buffer.slice(b.byteOffset,b.byteOffset+b.byteLength))(0);
if(pcm.length!==1920||Math.max(...pcm)<0.1||Math.max(...pcm)>1)throw Error('invalid decoded sound');
console.log(JSON.stringify({frames:pcm.length,peak:Math.max(...pcm)}));""".replace('MODULE', json.dumps(module.as_uri()))
            result = subprocess.run(['node', '--input-type=module', '-e', script, str(Path(tmp) / 'speech.hab')], capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)['frames'], 1920)

    def test_combined_budget_rejects_before_changing_existing_resources(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'confirm.wav'
            target.write_bytes(b'previous')
            with self.assertRaisesRegex(ValueError, '超过'):
                generate(spec(1000), tmp, 'wav')
            self.assertEqual(target.read_bytes(), b'previous')
            (Path(tmp) / 'a.wav').write_bytes(b'0' * 16000)
            (Path(tmp) / 'b.wav').write_bytes(b'0' * 16000)
            with self.assertRaisesRegex(ValueError, '超过'):
                check_budget([tmp])

    def test_multiple_waves_padding_and_exact_combined_budget(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = {'cues': [dict(id=shape, segments=[{'wave': shape, 'ms': 137, 'hz': [400, 700], 'volume': 0.2}]) for shape in ['sine', 'triangle', 'noise']]}
            report = generate(data, tmp, 'hab')
            self.assertEqual(report['bytes'], 8 + 24 + 3 * 7 * 166)
            self.assertEqual(check_budget([tmp, Path(tmp) / 'speech.hab'])['files'], 1)

    def test_invalid_and_nonfinite_values_cannot_escape_the_budget(self):
        for bad in [float('nan'), float('inf'), True, -1, 1.01]:
            data = spec();data['cues'][0]['segments'][0]['volume'] = bad
            with tempfile.TemporaryDirectory() as tmp, self.assertRaises(ValueError):
                generate(data, tmp, 'hab')
        with tempfile.TemporaryDirectory() as tmp:
            data = spec();data['cues'][0]['id'] = '../escape'
            with self.assertRaises(ValueError):
                generate(data, tmp, 'wav')


if __name__ == '__main__':
    unittest.main()
