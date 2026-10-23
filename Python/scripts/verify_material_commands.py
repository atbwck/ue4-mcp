"""材质命令的端到端验证，直接通过 TCP 驱动真实编辑器。

运行前需要一台已加载 UnrealMCP 的编辑器在 127.0.0.1:55557 上监听。
脚本只在 /Game/MCPVerifyTests 下创建延迟保存资产，不调用 save_assets。资产不会写入磁盘，但会在当前编辑器会话中保留；结束后关闭该临时工程以丢弃它们。
"""
import atexit
import json
import os
import socket
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from unreal_transport import receive_response

HOST, PORT = "127.0.0.1", 55557
TEST_DIR = "/Game/MCPVerifyTests"
# 每次运行用唯一名字：残留资产不应让下一次验证失败，也不该被静默覆盖。
RUN_TAG = uuid.uuid4().hex[:8]


class Client:
    """按编辑器插件的花括号定界协议收发，复用服务端同一套接收逻辑。"""

    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=60)

    def send(self, command, params=None):
        payload = json.dumps({"type": command, "params": params or {}}).encode("utf-8")
        self.sock.sendall(payload)
        return json.loads(receive_response(self.sock))

    def close(self):
        self.sock.close()


FAILURES = []


def check(label, condition, detail=""):
    """记录并打印一项断言，失败项汇总到 FAILURES。"""
    print(f"{'PASS' if condition else 'FAIL'} {label}{'' if condition else '  ' + str(detail)}")
    if not condition:
        FAILURES.append(label)
    return condition


def main():
    client = Client()
    atexit.register(client.close)
    run = client.send

    def payload(response):
        """命令处理器把负载放在 result.data 里；没有 data 时退回 result 本身。"""
        result = response.get("result")
        if not isinstance(result, dict):
            return {}
        data = result.get("data")
        return data if isinstance(data, dict) else result

    response = run("create_material", {"material_name": f"MCP_E2E_{RUN_TAG}", "save_path": TEST_DIR,
                                       "defer_save": True})
    if not check("create_material", response.get("status") == "success", response):
        return FAILURES
    material_path = payload(response)["material_path"]

    response = run("add_material_expression", {
        "material_path": material_path, "expression_class": "VectorParameter",
        "parameter_name": "Tint", "properties": {"DefaultValue": [0.2, 0.4, 0.6, 1.0]},
        "defer_save": True})
    if not check("add vector parameter", response.get("status") == "success", response):
        return FAILURES
    tint_id = payload(response)["node_id"]

    response = run("connect_material_property", {
        "material_path": material_path, "from_node_id": tint_id, "property": "BaseColor",
        "defer_save": True})
    check("connect BaseColor", response.get("status") == "success", response)

    response = run("add_material_expression", {
        "material_path": material_path, "expression_class": "ScalarParameter",
        "parameter_name": "Rough", "properties": {"DefaultValue": 0.25},
        "defer_save": True})
    rough_id = payload(response).get("node_id") if response.get("status") == "success" else None
    check("add scalar parameter", bool(rough_id), response)

    response = run("connect_material_property", {
        "material_path": material_path, "from_node_id": rough_id, "property": "Roughness",
        "defer_save": True})
    check("connect Roughness", response.get("status") == "success", response)

    response = run("get_material_graph", {"material_path": material_path})
    graph = payload(response)
    node_ids = {node["node_id"] for node in graph.get("expressions", [])}
    check("graph lists both nodes", {tint_id, rough_id} <= node_ids, graph.get("expressions"))
    properties = {entry["property"]: entry["source_node_id"] for entry in graph.get("property_inputs", [])}
    check("BaseColor input reported", properties.get("BaseColor") == tint_id, properties)
    check("Roughness input reported", properties.get("Roughness") == rough_id, properties)
    check("graph reports default settings", graph.get("settings", {}).get("blend_mode") == "BLEND_Opaque",
          graph.get("settings"))

    response = run("format_material_graph", {"material_path": material_path, "defer_save": True})
    formatted = payload(response)
    check("format placed both nodes", formatted.get("formatted_count") == 2, formatted)
    positions = {item["node_id"]: (item["x"], item["y"]) for item in formatted.get("planned", [])}
    check("nodes have distinct positions", positions.get(tint_id) != positions.get(rough_id), positions)

    response = run("set_material_settings", {
        "material_path": material_path, "blend_mode": "Masked",
        "shading_model": "Unlit", "two_sided": True,
        "opacity_mask_clip_value": 0.33, "defer_save": True})
    settings = payload(response).get("settings", {})
    check("blend mode applied", settings.get("blend_mode") == "BLEND_Masked", settings)
    check("shading model applied", settings.get("shading_model") == "MSM_Unlit", settings)
    check("two sided applied", settings.get("two_sided") is True, settings)
    check("clip value applied", abs(settings.get("opacity_mask_clip_value", 0) - 0.33) < 1e-4, settings)

    response = run("list_material_parameters", {"material_path": material_path})
    parameters = payload(response)
    check("one scalar parameter", len(parameters.get("scalar_parameters", [])) == 1, parameters)
    check("one vector parameter", len(parameters.get("vector_parameters", [])) == 1, parameters)

    response = run("disconnect_material_property", {
        "material_path": material_path, "property": "Roughness", "defer_save": True})
    check("disconnect Roughness", response.get("status") == "success", response)
    response = run("delete_material_expression", {
        "material_path": material_path, "node_id": rough_id, "defer_save": True})
    check("delete expression", response.get("status") == "success", response)
    response = run("get_material_graph", {"material_path": material_path})
    remaining = {node["node_id"] for node in payload(response).get("expressions", [])}
    check("deleted node gone", rough_id not in remaining, remaining)

    response = run("create_material_instance", {
        "instance_name": f"MCP_E2E_Inst_{RUN_TAG}", "parent_material_path": material_path,
        "save_path": TEST_DIR, "defer_save": True})
    if not check("create_material_instance", response.get("status") == "success", response):
        return FAILURES
    instance_path = payload(response)["instance_path"]

    response = run("set_material_instance_parameter", {
        "instance_path": instance_path, "param_name": "Tint",
        "vector": [0.9, 0.1, 0.1], "defer_save": True})
    check("vector override accepted", response.get("status") == "success", response)

    response = run("set_material_instance_parameter", {
        "instance_path": instance_path, "param_name": "Nope", "scalar": 1.0, "defer_save": True})
    check("unknown parameter rejected", response.get("status") == "error", response)

    response = run("get_material_instance_parameters", {"instance_path": instance_path})
    values = payload(response)
    vectors = {entry["name"]: entry["value"] for entry in values.get("vector_parameters", [])}
    # FLinearColor 是 float32，回读值只能按容差比较。
    read_back = vectors.get("Tint") or [0.0]
    check("override reads back", abs(read_back[0] - 0.9) < 1e-4, vectors)

    return FAILURES


if __name__ == "__main__":
    fail = main()
    print("ALL PASS" if not fail else f"FAILURES: {fail}")
    sys.exit(0 if not fail else 1)
