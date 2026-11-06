"""Lighting preference cache observes settings changes without caching policy."""
import re
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from harness import build,read,run


def test_lighting_preferences_refresh_on_change():
    code=re.sub(r'^#include.*$','',read('port/linux/game/lighting_config.c'),flags=re.M)
    status,output=run(build('lighting_config',(('under_test.inc',code),)),'settings')
    assert status==0,output


def test_a_platform_can_leave_a_key_out():
    code=re.sub(r'^#include.*$','',read('port/linux/game/lighting_config.c'),flags=re.M)
    assert code.count('\tNULL\n};')==1
    limited=code.replace('\tNULL\n};','\t"display.muzzle_lighting",\n\tNULL\n};')
    status,output=run(build('lighting_config',(('under_test.inc',limited),)),'limited')
    assert status==0,output


def test_forcing_a_setting_off_leaves_the_config_alone():
    code=re.sub(r'^#include.*$','',read('port/linux/game/lighting_config.c'),flags=re.M)
    assert 'config_write' not in code and 'config_set' not in code
    status,output=run(build('lighting_config',(('under_test.inc',code),)),'forced')
    assert status==0,output
