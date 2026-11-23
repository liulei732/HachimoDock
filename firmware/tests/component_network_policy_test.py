from pathlib import Path
import importlib.util
import json
import pytest
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("component_network", ROOT / "pc/skills/petui/scripts/component_network.py")
network = importlib.util.module_from_spec(spec)
spec.loader.exec_module(network)
@pytest.mark.parametrize("case", json.loads((Path(__file__).with_name("component_network_policies.json")).read_text()))
def test_network_policy(case):
    if case["valid"]:
        network.validate(case["policy"])
    else:
        with pytest.raises(ValueError):
            network.validate(case["policy"])
