"""
UMG Tools for Unreal MCP.

This module provides tools for creating and manipulating UMG Widget Blueprints in Unreal Engine.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_umg_tools(mcp: FastMCP):
    """Register UMG tools with the MCP server."""

    @mcp.tool()
    def create_umg_widget_blueprint(
        ctx: Context,
        widget_name: str,
        parent_class: str = "UserWidget",
        path: str = "/Game/UI"
    ) -> Dict[str, Any]:
        """
        Create a new UMG Widget Blueprint.

        Args:
            widget_name: Name of the widget blueprint to create
            parent_class: Parent class for the widget (default: UserWidget)
            path: Content browser path where the widget should be created

        Returns:
            Dict containing success status and widget path
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params = {
                "widget_name": widget_name,
                "parent_class": parent_class,
                "path": path
            }

            logger.info(f"Creating UMG Widget Blueprint with params: {params}")
            response = unreal.send_command("create_umg_widget_blueprint", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Create UMG Widget Blueprint response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error creating UMG Widget Blueprint: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_text_block_to_widget(
        ctx: Context,
        widget_name: str,
        text_block_name: str,
        text: str = "",
        position: List[float] = [0.0, 0.0],
        size: List[float] = [200.0, 50.0],
        font_size: int = 12,
        color: List[float] = [1.0, 1.0, 1.0, 1.0]
    ) -> Dict[str, Any]:
        """
        Add a Text Block widget to a UMG Widget Blueprint.

        Args:
            widget_name: Name of the target Widget Blueprint
            text_block_name: Name to give the new Text Block
            text: Initial text content
            position: [X, Y] position in the canvas panel
            size: [Width, Height] of the text block
            font_size: Font size in points
            color: [R, G, B, A] color values (0.0 to 1.0)

        Returns:
            Dict containing success status and text block properties
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params = {
                "widget_name": widget_name,
                "text_block_name": text_block_name,
                "text": text,
                "position": position,
                "size": size,
                "font_size": font_size,
                "color": color
            }

            logger.info(f"Adding Text Block to widget with params: {params}")
            response = unreal.send_command("add_text_block_to_widget", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Add Text Block response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error adding Text Block to widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_button_to_widget(
        ctx: Context,
        widget_name: str,
        button_name: str,
        text: str = "",
        position: List[float] = [0.0, 0.0],
        size: List[float] = [200.0, 50.0],
        font_size: int = 12,
        color: List[float] = [1.0, 1.0, 1.0, 1.0],
        background_color: List[float] = [0.1, 0.1, 0.1, 1.0]
    ) -> Dict[str, Any]:
        """
        Add a Button widget to a UMG Widget Blueprint.

        Args:
            widget_name: Name of the target Widget Blueprint
            button_name: Name to give the new Button
            text: Text to display on the button
            position: [X, Y] position in the canvas panel
            size: [Width, Height] of the button
            font_size: Font size for button text
            color: [R, G, B, A] text color values (0.0 to 1.0)
            background_color: [R, G, B, A] button background color values (0.0 to 1.0)

        Returns:
            Dict containing success status and button properties
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params = {
                "widget_name": widget_name,
                "button_name": button_name,
                "text": text,
                "position": position,
                "size": size,
                "font_size": font_size,
                "color": color,
                "background_color": background_color
            }

            logger.info(f"Adding Button to widget with params: {params}")
            response = unreal.send_command("add_button_to_widget", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Add Button response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error adding Button to widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_widget_blueprints(
        ctx: Context,
        path: str = "/Game",
        recursive: bool = True
    ) -> Dict[str, Any]:
        """
        List Widget Blueprints under a content path.

        Use this to discover project widgets before creating an interface;
        candidates are not ranked by how commonly they are used.

        Args:
            path: Content browser path to search (default /Game)
            recursive: Include subdirectories (default True)

        Returns:
            Dict with widget entries (path and parent class)
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("list_widget_blueprints", {
                "path": path,
                "recursive": recursive
            })

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error listing widget blueprints: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_widget_tree(
        ctx: Context,
        widget_name: str
    ) -> Dict[str, Any]:
        """
        Read the widget hierarchy of a Widget Blueprint.

        Returns each widget's name, class, parent and slot layout (canvas
        anchors/position/size, box padding and fill, overlay alignment).

        Args:
            widget_name: Widget Blueprint name or full /Game path

        Returns:
            Dict with the root name and widget entries
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            response = unreal.send_command("get_widget_tree", {
                "blueprint_name": widget_name
            })

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error reading widget tree: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_widget_to_widget(
        ctx: Context,
        widget_name: str,
        child_name: str,
        widget_class: str,
        parent_name: str = "",
        layout: Optional[Dict[str, Any]] = None,
        properties: Optional[Dict[str, Any]] = None
    ) -> Dict[str, Any]:
        """
        Add any native or project widget to a host Widget Blueprint.

        widget_class accepts a native class (e.g. "TextBlock", "HorizontalBox",
        "/Script/UMG.HorizontalBox") or a project Widget Blueprint path
        (e.g. "/Game/UI/WBP_Badge"). parent_name selects a panel in the host
        tree; empty uses the root panel.

        layout defaults to {} and depends on the parent:
        - CanvasPanel: position [x,y], size [w,h], anchors [minx,miny,maxx,maxy],
          alignment [x,y], z_order, autosize. With stretched anchors, position
          and size follow UE Canvas offsets (left/top and right/bottom margins).
        - HorizontalBox/VerticalBox: padding [left,top,right,bottom], fill
          (positive weight, null for auto), h_align (fill/left/center/right),
          v_align (fill/top/center/bottom).
        - Overlay/Border/SizeBox/Button: padding, h_align, v_align.
        SizeBox dimension overrides are widget properties, not slot layout fields.
        For complete interfaces, request a sketch or confirm a textual layout
        and target resolution first; prefer user-selected/project widgets.

        Invalid layout fields are rejected, not ignored.

        Args:
            widget_name: Host Widget Blueprint name or full /Game path
            child_name: Name for the new widget (must be unique in the host tree)
            widget_class: Native class or project widget asset path
            parent_name: Target panel name; empty means the root panel
            layout: Slot layout object (required, {} for defaults)
            properties: Extra property values set on the new widget

        Returns:
            Dict with the created widget's name, class and parent
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params = {
                "blueprint_name": widget_name,
                "widget_name": child_name,
                "widget_class": widget_class,
                "layout": layout if layout is not None else {},
                "properties": properties if properties is not None else {}
            }

            if parent_name:
                params["parent_name"] = parent_name
            logger.info(f"Adding widget '{child_name}' ({widget_class}) to '{widget_name}'")
            response = unreal.send_command("add_widget_to_widget", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error adding widget to widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def bind_widget_event(
        ctx: Context,
        widget_name: str,
        widget_component_name: str,
        event_name: str
    ) -> Dict[str, Any]:
        """
        Bind an event on a widget component (e.g. a button's OnClicked).

        The editor creates a component-bound event node for the widget, so no
        function name is taken: the node is the binding.

        Args:
            widget_name: Name of the target Widget Blueprint
            widget_component_name: Name of the widget component (button, etc.)
            event_name: Name of the event to bind (OnClicked, etc.)

        Returns:
            Dict containing success status and binding information
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params = {
                "widget_name": widget_name,
                "widget_component_name": widget_component_name,
                "event_name": event_name
            }

            logger.info(f"Binding widget event with params: {params}")
            response = unreal.send_command("bind_widget_event", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Bind widget event response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error binding widget event: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_widget_to_viewport(
        ctx: Context,
        widget_name: str,
        z_order: int = 0
    ) -> Dict[str, Any]:
        """
        Add a Widget Blueprint instance to the viewport.

        Args:
            widget_name: Name of the Widget Blueprint to add
            z_order: Z-order for the widget (higher numbers appear on top)

        Returns:
            Dict containing success status and widget instance information
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params = {
                "widget_name": widget_name,
                "z_order": z_order
            }

            logger.info(f"Adding widget to viewport with params: {params}")
            response = unreal.send_command("add_widget_to_viewport", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Add widget to viewport response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error adding widget to viewport: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_text_block_binding(
        ctx: Context,
        widget_name: str,
        text_block_name: str,
        binding_property: str,
        binding_type: str = "Text"
    ) -> Dict[str, Any]:
        """
        Set up a property binding for a Text Block widget.

        Args:
            widget_name: Name of the target Widget Blueprint
            text_block_name: Name of the Text Block to bind
            binding_property: Name of the Text property to read the text from
            binding_type: Must be "Text"; the editor only supports Text bindings

        Returns:
            Dict containing success status and binding information
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params = {
                "widget_name": widget_name,
                "text_block_name": text_block_name,
                "binding_property": binding_property,
                "binding_type": binding_type
            }

            logger.info(f"Setting text block binding with params: {params}")
            response = unreal.send_command("set_text_block_binding", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Set text block binding response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error setting text block binding: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_image_to_widget(
        ctx: Context,
        widget_name: str,
        image_name: str,
        position=None,
        size=None,
        bHidden: bool = False
    ) -> Dict[str, Any]:
        """
        Add an Image widget to a UMG Widget Blueprint's root canvas.

        Args:
            widget_name: Name of the target Widget Blueprint
            image_name: Name for the new Image widget (e.g. Image_GuildIcon)
            position: [X, Y] position in the canvas (optional)
            size: [Width, Height] of the image (optional, default [64, 64])
            bHidden: Create the image hidden (for placeholder art configured later)

        Returns:
            Dict containing the created image details
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            params = {
                "widget_name": widget_name,
                "image_name": image_name,
                "bHidden": bHidden,
            }
            if position:
                params["position"] = list(position)
            if size:
                params["size"] = list(size)

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Adding image to widget: {params}")
            response = unreal.send_command("add_image_to_widget", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error adding image to widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    logger.info("UMG tools registered successfully")
