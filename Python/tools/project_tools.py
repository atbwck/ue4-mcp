"""
Project Tools for Unreal MCP.

This module provides tools for managing project-wide settings and configuration.
"""

import logging
from typing import Dict, Any, List
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_project_tools(mcp: FastMCP):
    """Register project tools with the MCP server."""

    @mcp.tool()
    def create_input_mapping(
        ctx: Context,
        action_name: str,
        key: str,
        input_type: str = "Action"
    ) -> Dict[str, Any]:
        """
        Create an input mapping for the project.

        Args:
            action_name: Name of the input action
            key: Key to bind (SpaceBar, LeftMouseButton, etc.)
            input_type: Type of input mapping (Action or Axis)

        Returns:
            Response indicating success or failure
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            # 插件会把未知类型当成 Action，客户端先拒绝拼写错误以免改错配置
            if input_type.lower() not in {"action", "axis"}:
                return {"success": False, "message": "input_type must be Action or Axis"}
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params = {
                "action_name": action_name,
                "key": key,
                "input_type": input_type
            }

            logger.info(f"Creating input mapping '{action_name}' with key '{key}'")
            response = unreal.send_command("create_input_mapping", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Input mapping creation response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error creating input mapping: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def refresh_project_reflection(
        ctx: Context,
        blueprint_name: str = ""
    ) -> Dict[str, Any]:
        """
        Rebuild Blueprint graph nodes against the currently loaded C++ reflection.

        Use this after adding or changing a BlueprintCallable C++ function and
        building the module: existing function nodes keep stale bindings otherwise.
        This does not compile C++ itself; if a function is still missing, build the
        project modules first, then retry.

        Args:
            blueprint_name: Limit to one Blueprint (default: every /Game Blueprint)

        Returns:
            Response with "refreshed" and "failed" asset path lists
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params: Dict[str, Any] = {}
            if blueprint_name:
                params["blueprint_name"] = blueprint_name

            response = unreal.send_command("refresh_project_reflection", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error refreshing project reflection: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def compile_project(ctx: Context) -> Dict[str, Any]:
        """
        Trigger a Live Coding compile of the project's C++ modules.

        Asynchronous and only available when Live Coding is loaded. It patches
        function bodies; whether newly added BlueprintCallable functions reach
        reflection is not guaranteed, so call refresh_project_reflection after it.
        For a guaranteed rebuild, build the project with your build tool instead.

        Returns:
            Response with "compile_triggered" and a note about the remaining caveat
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("compile_project", {})
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error triggering project compile: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def save_assets(ctx: Context, asset_paths: List[str]) -> Dict[str, Any]:
        """
        Save specific assets by path.

        Pairs with defer_save=True on write commands: those skip saving so a
        multi-step edit is only written to disk once it succeeds. Call this to
        commit, and check "failed" for assets that did not save.

        Args:
            asset_paths: Full asset paths, e.g. ["/Game/Blueprints/BP_Bird"]

        Returns:
            Response with "saved" and "failed" lists
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        if not asset_paths:
            return {"success": False, "message": "asset_paths must contain at least one path"}
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("save_assets", {"asset_paths": list(asset_paths)})
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error saving assets: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_asset_property(
        ctx: Context,
        asset_path: str,
        property_name: str,
        property_value,
    ) -> Dict[str, Any]:
        """
        Set a property on any asset object (e.g. a DataAsset instance) and save it.

        The plugin writes through reflection, so Vectors, Rotators, arrays and TSets
        are supported; the underlying save error is reported instead of a silent success.

        Args:
            asset_path: Full asset path, e.g. "/Game/Data/DA_Config"
            property_name: Reflected property name
            property_value: New value; shape depends on the property type

        Returns:
            Response indicating success or the save failure reason
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params = {
                "asset_path": asset_path,
                "property_name": property_name,
                "property_value": property_value,
            }
            response = unreal.send_command("set_asset_property", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error setting asset property: {e}")
            return {"success": False, "message": str(e)}

    logger.info("Project tools registered successfully")
