"""蓝图图表查询与局部排版工具。"""
from typing import Any, Dict, List, Optional
from mcp.server.fastmcp import Context, FastMCP


def register_layout_tools(mcp: FastMCP):
    @mcp.tool()
    def begin_blueprint_edit(ctx: Context, blueprint_name: str, graph_name: str = "") -> Dict[str, Any]:
        """Snapshot a graph before editing so unused nodes can be identified later.

        Call this before a batch of node edits, then finish_blueprint_edit when done.
        Only nodes created within the batch are ever candidates for pruning.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection
        unreal = get_unreal_connection()
        if not unreal:
            return no_connection()
        return unreal.begin_edit_batch(blueprint_name, graph_name)

    @mcp.tool()
    def finish_blueprint_edit(ctx: Context, blueprint_name: str, graph_name: str = "",
                              prune: bool = True, format_graph: bool = True,
                              compile_after: bool = True) -> Dict[str, Any]:
        """Prune nodes created in this batch that nothing uses, then lay out, compile, verify.

        Rules: a pure data node is unused when none of its outputs are connected, even if
        its inputs are still wired; an impure node is removed only when nothing at all is
        connected (a node attached to an execution chain is always kept). Entry, exit,
        event, tunnel and comment nodes are never removed. Recursion stops at nodes that
        existed before the batch.

        Order matters: prune -> layout -> compile -> save -> re-read the graph and check
        that removed nodes are gone and kept nodes survived. A compile or save failure is
        reported as a failure, never as success.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection, result_payload
        unreal = get_unreal_connection()
        if not unreal:
            return no_connection()

        steps: Dict[str, Any] = {}
        new_nodes = unreal.diff_new_nodes(blueprint_name, graph_name)
        if new_nodes.get("status") != "success":
            return new_nodes
        candidates = result_payload(new_nodes).get("new_node_ids", [])
        steps["new_nodes"] = candidates

        removed: list = []
        kept: list = []
        if prune and candidates:
            params: Dict[str, Any] = {
                "blueprint_name": blueprint_name,
                "candidate_node_ids": candidates,
                "defer_save": True,
            }
            if graph_name:
                params["graph_name"] = graph_name
            pruned = unreal.send_command("prune_unused_nodes", params)
            if not pruned or pruned.get("status") != "success":
                return pruned or {"status": "error", "error": "No response while pruning"}
            removed = result_payload(pruned).get("removed", [])
            kept = result_payload(pruned).get("kept", [])
        steps["removed"] = removed
        steps["kept"] = kept

        if format_graph:
            layout_params: Dict[str, Any] = {
                "blueprint_name": blueprint_name, "mode": "unplaced", "defer_save": True}
            if graph_name:
                layout_params["graph_name"] = graph_name
            laid_out = unreal.send_command("format_blueprint_graph", layout_params)
            if not laid_out or laid_out.get("status") != "success":
                return laid_out or {"status": "error", "error": "No response while formatting"}
            steps["formatted"] = result_payload(laid_out).get("formatted_count", 0)

        compile_params: Dict[str, Any] = {"blueprint_name": blueprint_name}
        if compile_after:
            compiled = unreal.send_command("compile_blueprint", compile_params)
            if not compiled or compiled.get("status") != "success":
                # 编译失败不保存，失败响应里保留编译器诊断
                return compiled or {"status": "error", "error": "No response while compiling"}
            steps["diagnostics"] = result_payload(compiled).get("diagnostics", {})

        saved = unreal.send_command("save_assets", {"asset_paths": [blueprint_name]})
        if not saved or saved.get("status") != "success":
            return saved or {"status": "error", "error": "No response while saving"}

        # 独立回读：确认删除的确实消失、保留的确实存在
        verify_params: Dict[str, Any] = {"blueprint_name": blueprint_name}
        if graph_name:
            verify_params["graph_name"] = graph_name
        verify = unreal.send_command("get_blueprint_graph", verify_params)
        nodes = result_payload(verify).get("nodes")
        if not isinstance(nodes, list):
            return {"status": "error", "error": "Could not re-read graph to verify"}
        present = {n["node_id"] for n in nodes if isinstance(n, dict) and "node_id" in n}
        removed_ids = {r.get("node_id") for r in removed}
        kept_ids = {k.get("node_id") for k in kept}
        still_there = sorted(removed_ids & present)
        vanished = sorted(kept_ids - present)
        steps["verify"] = {
            "node_count": len(present),
            "removed_still_present": still_there,
            "kept_missing": vanished,
        }
        unreal.baseline_nodes.pop((blueprint_name, graph_name), None)
        if still_there or vanished:
            return {"status": "error", "error": "Post-edit graph verification failed", "result": steps}
        return {"status": "success", "result": steps}

    @mcp.tool()
    def list_blueprint_graphs(ctx: Context, blueprint_name: str) -> Dict[str, Any]:
        """List Event, function and macro graphs with names and node counts."""
        from unreal_mcp_server import get_unreal_connection, no_connection
        unreal = get_unreal_connection()
        if not unreal:
            return no_connection()
        return unreal.send_command("list_blueprint_graphs", {"blueprint_name": blueprint_name}) or {
            "success": False, "message": "No response from Unreal Engine"}

    @mcp.tool()
    def create_blueprint_graph(ctx: Context, blueprint_name: str, graph_name: str,
                               graph_type: str = "function", defer_save: bool = False) -> Dict[str, Any]:
        """Create a function or macro graph with the engine's entry/exit nodes.

        Use graph_name on node tools to edit the graph after creating it.
        defer_save leaves it in memory until save_assets is called; it does not
        roll back changes when a later command fails.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection
        if graph_type not in {"function", "macro"}:
            return {"success": False, "message": "graph_type must be function or macro"}
        unreal = get_unreal_connection()
        if not unreal:
            return no_connection()
        return unreal.send_command("create_blueprint_graph", {
            "blueprint_name": blueprint_name, "graph_name": graph_name,
            "graph_type": graph_type, "defer_save": defer_save}) or {
                "success": False, "message": "No response from Unreal Engine"}

    @mcp.tool()
    def get_blueprint_graph(ctx: Context, blueprint_name: str, graph_name: str = "") -> Dict[str, Any]:
        """Read a Blueprint graph: node IDs, positions, measured sizes and pin links.

        Defaults to the first Event Graph; pass graph_name to inspect a function
        or macro graph. Does not create a graph. size_source distinguishes Slate
        desired size from fallback estimates; neither is a screenshot inspection.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection
        unreal = get_unreal_connection()
        if not unreal:
            return no_connection()
        params: Dict[str, Any] = {"blueprint_name": blueprint_name}
        if graph_name:
            params["graph_name"] = graph_name
        return unreal.send_command("get_blueprint_graph", params) or {
            "success": False, "message": "No response from Unreal Engine"}

    @mcp.tool()
    def format_blueprint_graph(ctx: Context, blueprint_name: str,
                               node_ids: Optional[List[str]] = None,
                               whole_graph: bool = False,
                               graph_name: str = "",
                               mode: str = "selection",
                               dry_run: bool = False,
                               defer_save: bool = False) -> Dict[str, Any]:
        """Arrange one Blueprint graph without changing nodes or connections.

        Layout model: each independent execution flow gets its own horizontal band
        (flows never interleave), nodes in the same execution column share one X,
        and data nodes are placed in depth-based columns so equal-depth reads align.

        Choose the scope:
        - node_ids: arrange exactly these nodes; everything else stays put.
        - whole_graph=True: arrange every non-comment node (moves hand-placed nodes too).
        - mode="unplaced": arrange only nodes never positioned (still at the origin),
          which is the safe default for cleaning up machine-created nodes.

        Comments are never moved or treated as containers, but nodes are kept out of them.
        Cross-flow links are reported in "warnings" and never broken automatically.
        Use dry_run=True to preview target positions without touching the asset.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection
        if mode not in {"selection", "unplaced"}:
            return {"success": False, "message": "mode must be selection or unplaced"}
        if mode == "selection" and whole_graph == (node_ids is not None):
            return {"success": False, "message": "Choose node_ids or whole_graph=True"}
        if mode == "unplaced" and (node_ids is not None or whole_graph):
            return {"success": False, "message": "mode=unplaced does not take node_ids or whole_graph"}
        unreal = get_unreal_connection()
        if not unreal:
            return no_connection()
        params: Dict[str, Any] = {"blueprint_name": blueprint_name, "mode": mode}
        if graph_name:
            params["graph_name"] = graph_name
        if dry_run:
            params["dry_run"] = True
        if mode == "selection":
            if whole_graph:
                params["whole_graph"] = True
            else:
                params["node_ids"] = node_ids
        params["defer_save"] = defer_save
        return unreal.send_command("format_blueprint_graph", params) or {
            "success": False, "message": "No response from Unreal Engine"}
