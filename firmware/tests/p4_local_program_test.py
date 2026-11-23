"""Exercise the device worker, verified admission, actual timer VM and shared PROGRAM broker."""
import json
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]


def test_local_program_consent_reboot_revoke_and_registry(tmp_path):
    main=ROOT/'main'
    tests=ROOT/'tests'
    bg=(main/'pet_p4_miniapp_background.inc').read_text()
    functions=('background_matches_under_registry','background_checkpoint_one','background_prepare_remove',
               'background_other_program_busy','pet_p4_miniapp_background_available','background_start_installed',
               'pet_p4_miniapp_background_start','background_process_locked')
    (tmp_path/'background.inc').write_text('\n'.join(function(bg,n) for n in functions))
    header=(main/'pet_p4_miniapp.h').read_text()
    (tmp_path/'local_api.h').write_text(header[header.index('enum { PET_ALARM_OFF'):header.index('/* Existing protocol storage worker only')])
    # Keep the physical entry and the idle command admission identical to main.
    input_source=(main/'pet_p4_input.c').read_text()
    first=input_source.index('  if (state && !strcmp(state->screen_page, PET_ALARM_PAGE))')
    (tmp_path/'local_input.inc').write_text(input_source[first:input_source.index('#endif',first)])
    worker=(main/'pet_p4_main.c').read_text()
    first=worker.index('    static uint64_t alarm_check_after;')
    (tmp_path/'local_worker.inc').write_text(worker[first:worker.index('#endif\n    if (xQueueReceive',first)])
    package=json.loads((ROOT.parent/'pc/skills/petui/contracts/component-program-v1.json').read_text())['runtime']
    package['program']['background']=True
    package['program']['timers']=[dict(id=1,delayMs=100,intervalMs=100,words=[1])]
    # Two installed, verified images use the same pure integer ABI and independent saves.
    (tmp_path/'runtime.inc').write_text(json.dumps(json.dumps(package,ensure_ascii=True)))
    sources=('resources','resources_device','component_program','component_program_session','program_registry',
             'program_core','program_image','program_runtime','program_host','program_host_device',
             'program_group','program_scheduler','program_timers','event_bus','storage_lane')
    exe=tmp_path/'program'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations',
                    '-Wno-misleading-indentation','-fsanitize=address,undefined','-pthread',
                    '-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
                    '-DPET_COMPONENT_BACKGROUND_ENABLED=1','-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1',
                    '-DPET_P4_PUBLIC_PROGRAM_PROFILE=1','-DPET_P4_EVENTS=1',
                    '-I',str(tmp_path),'-I',str(main),'-I',str(tests/'program_probe_stubs'),'-I',str(tests/'vendor/cjson'),
                    str(tests/'p4_local_program_test.c'),*[str(main/f'pet_p4_{n}.c') for n in sources],
                    str(tests/'vendor/cjson/cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=15)
