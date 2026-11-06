"""Flight-light ownership uses the real feature module with fake engine services."""
import re
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, read, run


def test_owned_lights():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/rocket_lighting.c'), flags=re.M)
    status, output = run(build('rocket_lighting', (('under_test.inc', code),)), 'ownership')
    assert status == 0, output


def test_missing_reconnection_is_detected():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/rocket_lighting.c'), flags=re.M)
    assert code.count('light_reconnect_to_map(light->light_index);') == 1
    code = code.replace('light_reconnect_to_map(light->light_index);', '(void)0;')
    status, output = run(build('rocket_lighting', (('under_test.inc', code),)), 'ownership')
    assert status == CHECK_FAILED, output
