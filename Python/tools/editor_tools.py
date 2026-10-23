"""
Editor Tools for Unreal MCP.

This module provides tools for controlling the Unreal Editor viewport and other editor functionality.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_editor_tools(mcp: FastMCP):
    """Register editor tools with the MCP server."""

    @mcp.tool()
    def get_actors_in_level(ctx: Context) -> List[Dict[str, Any]]:
        """Get a list of all actors in the current level."""
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.warning("Failed to connect to Unreal Engine")
                return []

            response = unreal.send_command("get_actors_in_level", {})

            if not response:
                logger.warning("No response from Unreal Engine")
                return []

            # Log the complete response for debugging
            logger.info(f"Complete response from Unreal: {response}")

            # Check response format
            if "result" in response and "actors" in response["result"]:
                actors = response["result"]["actors"]
                logger.info(f"Found {len(actors)} actors in level")
                return actors
            elif "actors" in response:
                actors = response["actors"]
                logger.info(f"Found {len(actors)} actors in level")
                return actors

            logger.warning(f"Unexpected response format: {response}")
            return []

        except Exception as e:
            logger.error(f"Error getting actors: {e}")
            return []

    @mcp.tool()
    def find_actors_by_name(ctx: Context, pattern: str) -> List[str]:
        """Find actors by name pattern."""
        from unreal_mcp_server import get_unreal_connection, result_payload

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.warning("Failed to connect to Unreal Engine")
                return []

            response = unreal.send_command("find_actors_by_name", {
                "pattern": pattern
            })

            if not response:
                return []

            # 插件把负载包在 result 内，必须按内层取值
            return result_payload(response).get("actors", [])

        except Exception as e:
            logger.error(f"Error finding actors: {e}")
            return []

    @mcp.tool()
    def spawn_actor(
        ctx: Context,
        name: str,
        type: str,
        location: List[float] = [0.0, 0.0, 0.0],
        rotation: List[float] = [0.0, 0.0, 0.0],
        scale: List[float] = [1.0, 1.0, 1.0],
        mesh_path: str = ""
    ) -> Dict[str, Any]:
        """Create a new actor in the current level.

        Args:
            ctx: The MCP context
            name: The name to give the new actor (must be unique)
            type: The type of actor to create (e.g. StaticMeshActor, PointLight)
            location: The [x, y, z] world location to spawn at
            rotation: The [pitch, yaw, roll] rotation in degrees
            scale: The [x, y, z] scale to apply (default [1, 1, 1])
            mesh_path: Optional static mesh asset path for a StaticMeshActor
                       (e.g. "/Engine/BasicShapes/Cube.Cube")

        Returns:
            Dict containing the created actor's properties
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            # Ensure all parameters are properly formatted
            params = {
                "name": name,
                "type": type,
                "location": location,
                "rotation": rotation,
                "scale": scale
            }
            if mesh_path:
                params["mesh_path"] = mesh_path

            # Validate location/rotation/scale formats
            for param_name in ["location", "rotation", "scale"]:
                param_value = params[param_name]
                if not isinstance(param_value, list) or len(param_value) != 3:
                    logger.error(f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
                    return {"success": False, "message": f"Invalid {param_name} format. Must be a list of 3 float values."}
                # Ensure all values are float
                params[param_name] = [float(val) for val in param_value]

            logger.info(f"Creating actor '{name}' of type '{type}' with params: {params}")
            response = unreal.send_command("spawn_actor", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            # Log the complete response for debugging
            logger.info(f"Actor creation response: {response}")

            # Handle error responses correctly
            if response.get("status") == "error":
                error_message = response.get("error", "Unknown error")
                logger.error(f"Error creating actor: {error_message}")
                return {"success": False, "message": error_message}

            return response

        except Exception as e:
            error_msg = f"Error creating actor: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def delete_actor(ctx: Context, name: str) -> Dict[str, Any]:
        """Delete an actor by name."""
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("delete_actor", {
                "name": name
            })
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            logger.error(f"Error deleting actor: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_actor_transform(
        ctx: Context,
        name: str,
        location: Optional[List[float]]  = None,
        rotation: Optional[List[float]]  = None,
        scale: Optional[List[float]] = None
    ) -> Dict[str, Any]:
        """Set the transform of an actor."""
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params: Dict[str, Any] = {"name": name}
            if location is not None:
                params["location"] = location
            if rotation is not None:
                params["rotation"] = rotation
            if scale is not None:
                params["scale"] = scale

            response = unreal.send_command("set_actor_transform", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            logger.error(f"Error setting transform: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def get_actor_properties(ctx: Context, name: str) -> Dict[str, Any]:
        """Get all properties of an actor."""
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("get_actor_properties", {
                "name": name
            })
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            logger.error(f"Error getting properties: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_actor_property(
        ctx: Context,
        name: str,
        property_name: str,
        property_value,
    ) -> Dict[str, Any]:
        """
        Set a property on an actor.

        Args:
            name: Name of the actor
            property_name: Name of the property to set
            property_value: Value to set the property to

        Returns:
            Dict containing response from Unreal with operation status
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("set_actor_property", {
                "name": name,
                "property_name": property_name,
                "property_value": property_value
            })

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Set actor property response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error setting actor property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    # @mcp.tool() commented out because it's buggy
    def focus_viewport(
        ctx: Context,
        target: Optional[str] = None,
        location: Optional[List[float]] = None,
        distance: float = 1000.0,
        orientation: Optional[List[float]] = None
    ) -> Dict[str, Any]:
        """
        Focus the viewport on a specific actor or location.

        Args:
            target: Name of the actor to focus on (if provided, location is ignored)
            location: [X, Y, Z] coordinates to focus on (used if target is None)
            distance: Distance from the target/location
            orientation: Optional [Pitch, Yaw, Roll] for the viewport camera

        Returns:
            Response from Unreal Engine
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params: Dict[str, Any] = {}
            if target:
                params["target"] = target
            elif location:
                params["location"] = location

            if distance:
                params["distance"] = distance

            if orientation:
                params["orientation"] = orientation

            response = unreal.send_command("focus_viewport", params)
            return response or {}

        except Exception as e:
            logger.error(f"Error focusing viewport: {e}")
            return {"status": "error", "message": str(e)}

    @mcp.tool()
    def spawn_blueprint_actor(
        ctx: Context,
        blueprint_name: str,
        actor_name: str,
        location: List[float] = [0.0, 0.0, 0.0],
        rotation: List[float] = [0.0, 0.0, 0.0]
    ) -> Dict[str, Any]:
        """Spawn an actor from a Blueprint.

        Args:
            ctx: The MCP context
            blueprint_name: Name of the Blueprint to spawn from
            actor_name: Name to give the spawned actor
            location: The [x, y, z] world location to spawn at
            rotation: The [pitch, yaw, roll] rotation in degrees

        Returns:
            Dict containing the spawned actor's properties
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            # Ensure all parameters are properly formatted
            params = {
                "blueprint_name": blueprint_name,
                "actor_name": actor_name,
                "location": location or [0.0, 0.0, 0.0],
                "rotation": rotation or [0.0, 0.0, 0.0]
            }

            # Validate location and rotation formats
            for param_name in ["location", "rotation"]:
                param_value = params[param_name]
                if not isinstance(param_value, list) or len(param_value) != 3:
                    logger.error(f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
                    return {"success": False, "message": f"Invalid {param_name} format. Must be a list of 3 float values."}
                # Ensure all values are float
                params[param_name] = [float(val) for val in param_value]

            logger.info(f"Spawning blueprint actor with params: {params}")
            response = unreal.send_command("spawn_blueprint_actor", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Spawn blueprint actor response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error spawning blueprint actor: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def duplicate_level(ctx: Context, source_path: str, destination_path: str) -> Dict[str, Any]:
        """Duplicate a UE level as a new map asset with a unique internal asset ID.

        Use this instead of copying .umap files directly: raw copies retain the source
        Map PrimaryAssetID and make UE warn about duplicate map identities.
        Example: duplicate_level('/Game/Maps/NewMap', '/Game/Maps/MainMap').
        """
        from unreal_mcp_server import get_unreal_connection, no_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()
            response = unreal.send_command("duplicate_level", {
                "source_path": source_path,
                "destination_path": destination_path,
            })
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error duplicating level: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def take_screenshot(ctx: Context, filepath: str, pie: bool = False) -> Dict[str, Any]:
        """Capture the active viewport (or PIE game viewport with pie=True) to PNG.

        Use this to verify what the editor is actually rendering (materials, lighting,
        layout) instead of inferring it from properties.

        Args:
            filepath: Absolute output path; ".png" is appended when missing.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("take_screenshot", {"filepath": filepath, "pie": pie})
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error taking screenshot: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def get_pie_state(ctx: Context) -> Dict[str, Any]:
        """Report whether a PIE (Play In Editor) session is running and ready.

        "ready" means the play world has begun play and has a local player
        controller, which is when runtime actors become safe to address.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection, result_payload

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("get_pie_state", {})
            if not response:
                return {"success": False, "message": "No response from Unreal Engine"}
            payload = result_payload(response)
            if payload:
                return payload
            return response
        except Exception as e:
            logger.error(f"Error reading PIE state: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def start_pie(ctx: Context) -> Dict[str, Any]:
        """Queue a Play In Editor session; poll get_pie_state until ready.

        Runtime-only features (dynamic material instances, gameplay events) need a live
        session. Pass pie=true to material tools afterwards to address the play world.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("start_pie", {})
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error starting PIE: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def stop_pie(ctx: Context) -> Dict[str, Any]:
        """Queue PIE shutdown; poll get_pie_state until playing is false."""
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("stop_pie", {})
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error stopping PIE: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def save_level(ctx: Context, path: str = "") -> Dict[str, Any]:
        """Save the editor world after deferred actor/material changes.

        path optionally saves the level to a different /Game map path.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection
        unreal = get_unreal_connection()
        if not unreal:
            return no_connection()
        return unreal.send_command("save_level", {"path": path}) or {
            "success": False, "message": "No response from Unreal Engine"}

    logger.info("Editor tools registered successfully")
