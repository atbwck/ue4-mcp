"""手动脚本契约检查；只调用注入假响应的辅助函数。"""

from pathlib import Path
import runpy
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent


class ScriptContractTests(unittest.TestCase):
    def test_input_mapping_accepts_success_without_inner_flag(self):
        namespace = runpy.run_path(str(ROOT / "scripts/node/test_input_mapping.py"),
                                   run_name="__inspection__")
        helper = namespace["setup_input_mapping"]
        with mock.patch.dict(helper.__globals__, {
            "send_command": lambda *args: {"status": "success", "result": {}},
        }):
            self.assertTrue(helper(None, "Jump", "SpaceBar"))
        with mock.patch.dict(helper.__globals__, {
            "send_command": lambda *args: {"status": "error", "error": "failed"},
        }):
            self.assertFalse(helper(None, "Jump", "SpaceBar"))

    def test_component_properties_use_setter_and_propagate_failure(self):
        namespace = runpy.run_path(str(ROOT / "scripts/blueprints/test_create_and_spawn_blueprints_with_different_components.py"),
                                   run_name="__inspection__")
        helper = namespace["add_component"]
        for failed in [False, True]:
            calls = []
            def send(command, params):
                calls.append((command, params))
                if failed and command == "set_component_property":
                    return {"status": "error", "error": "failed"}
                return {"status": "success", "result": {}}
            with mock.patch.dict(helper.__globals__, {"send_command": send}):
                result = helper("B", "SphereComponent", "C", properties={"bEnableGravity": True})
            self.assertEqual(not failed, result)
            self.assertEqual(["add_component_to_blueprint", "set_component_property"],
                             [command for command, params in calls])
            self.assertNotIn("component_properties", calls[0][1])
            self.assertEqual("bEnableGravity", calls[1][1]["property_name"])


if __name__ == "__main__":
    unittest.main()
