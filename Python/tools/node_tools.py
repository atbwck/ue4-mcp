"""
Blueprint Node Tools for Unreal MCP.

This module provides tools for manipulating Blueprint graph nodes and connections.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_blueprint_node_tools(mcp: FastMCP):
    """Register Blueprint node manipulation tools with the MCP server."""

    @mcp.tool()
    def add_blueprint_event_node(
        ctx: Context,
        blueprint_name: str,
        event_name: str,
        node_position = None,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Add an event node to a Blueprint's event graph.

        Args:
            blueprint_name: Name of the target Blueprint
            event_name: Name of the event. Use 'Receive' prefix for standard events:
                       - 'ReceiveBeginPlay' for Begin Play
                       - 'ReceiveTick' for Tick
                       - etc.
            node_position: Optional [X, Y] position in the graph

        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            # Handle default value within the method body

            params = {
                "blueprint_name": blueprint_name,
                "event_name": event_name
            }
            if graph_name:
                params["graph_name"] = graph_name
            if node_position is not None:
                params["node_position"] = node_position

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Adding event node '{event_name}' to blueprint '{blueprint_name}'")
            params["defer_save"] = defer_save
            response = unreal.send_command("add_blueprint_event_node", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Event node creation response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error adding event node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_blueprint_input_action_node(
        ctx: Context,
        blueprint_name: str,
        action_name: str,
        node_position = None,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Add an input action event node to a Blueprint's event graph.

        Args:
            blueprint_name: Name of the target Blueprint
            action_name: Name of the input action to respond to
            node_position: Optional [X, Y] position in the graph

        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            # Handle default value within the method body

            params = {
                "blueprint_name": blueprint_name,
                "action_name": action_name
            }
            if graph_name:
                params["graph_name"] = graph_name
            if node_position is not None:
                params["node_position"] = node_position

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Adding input action node for '{action_name}' to blueprint '{blueprint_name}'")
            params["defer_save"] = defer_save
            response = unreal.send_command("add_blueprint_input_action_node", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Input action node creation response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error adding input action node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_blueprint_function_node(
        ctx: Context,
        blueprint_name: str,
        target: str,
        function_name: str,
        params = None,
        node_position = None,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Add a function call node to a Blueprint's event graph.

        Args:
            blueprint_name: Name of the target Blueprint
            target: Target object for the function (component name or self)
            function_name: Name of the function to call
            params: Optional parameters to set on the function node
            node_position: Optional [X, Y] position in the graph

        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            # Handle default values within the method body
            if params is None:
                params = {}

            command_params = {
                "blueprint_name": blueprint_name,
                "target": target,
                "function_name": function_name,
                "params": params
            }
            if graph_name:
                command_params["graph_name"] = graph_name
            if node_position is not None:
                command_params["node_position"] = node_position

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Adding function node '{function_name}' to blueprint '{blueprint_name}'")
            command_params["defer_save"] = defer_save
            response = unreal.send_command("add_blueprint_function_node", command_params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Function node creation response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error adding function node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def connect_blueprint_nodes(
        ctx: Context,
        blueprint_name: str,
        source_node_id: str,
        source_pin: str,
        target_node_id: str,
        target_pin: str,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Connect two nodes in a Blueprint's event graph.

        Keep execution chains aligned from left to right and separate unrelated
        event/function flows. Avoid long crossing wires and funneling unrelated
        flows into one operation node. Place branches on separate rows, preserve
        valid branch convergence and data sharing, and never duplicate stateful
        operations solely for layout.

        Args:
            blueprint_name: Name of the target Blueprint
            source_node_id: ID of the source node
            source_pin: Name of the output pin on the source node
            target_node_id: ID of the target node
            target_pin: Name of the input pin on the target node

        Returns:
            Response indicating success or failure
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "source_node_id": source_node_id,
                "source_pin": source_pin,
                "target_node_id": target_node_id,
                "target_pin": target_pin
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Connecting nodes in blueprint '{blueprint_name}'")
            params["defer_save"] = defer_save
            response = unreal.send_command("connect_blueprint_nodes", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Node connection response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error connecting nodes: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_blueprint_variable(
        ctx: Context,
        blueprint_name: str,
        variable_name: str,
        variable_type: str,
        is_exposed: bool = False,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Add a variable to a Blueprint.

        Args:
            blueprint_name: Name of the target Blueprint
            variable_name: Name of the variable
            variable_type: Type of the variable (Boolean, Integer, Float, Vector, etc.)
            is_exposed: Whether to expose the variable to the editor

        Returns:
            Response indicating success or failure
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "variable_name": variable_name,
                "variable_type": variable_type,
                "is_exposed": is_exposed
            }

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Adding variable '{variable_name}' to blueprint '{blueprint_name}'")
            params["defer_save"] = defer_save
            response = unreal.send_command("add_blueprint_variable", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Variable creation response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error adding variable: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_blueprint_get_self_component_reference(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        node_position = None,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Add a node that gets a reference to a component owned by the current Blueprint.
        This creates a node similar to what you get when dragging a component from the Components panel.

        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Name of the component to get a reference to
            node_position: Optional [X, Y] position in the graph

        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            # Handle None case explicitly in the function

            params = {
                "blueprint_name": blueprint_name,
                "component_name": component_name
            }
            if graph_name:
                params["graph_name"] = graph_name
            if node_position is not None:
                params["node_position"] = node_position

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Adding self component reference node for '{component_name}' to blueprint '{blueprint_name}'")
            params["defer_save"] = defer_save
            response = unreal.send_command("add_blueprint_get_self_component_reference", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Self component reference node creation response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error adding self component reference node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_blueprint_self_reference(
        ctx: Context,
        blueprint_name: str,
        node_position = None,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Add a 'Get Self' node to a Blueprint's event graph that returns a reference to this actor.

        Args:
            blueprint_name: Name of the target Blueprint
            node_position: Optional [X, Y] position in the graph

        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:

            params = {
                "blueprint_name": blueprint_name
            }
            if graph_name:
                params["graph_name"] = graph_name
            if node_position is not None:
                params["node_position"] = node_position

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Adding self reference node to blueprint '{blueprint_name}'")
            params["defer_save"] = defer_save
            response = unreal.send_command("add_blueprint_self_reference", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Self reference node creation response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error adding self reference node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def find_blueprint_nodes(
        ctx: Context,
        blueprint_name: str,
        node_type = None,
        event_type = None,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """
        Find nodes in a Blueprint's event graph.

        Args:
            blueprint_name: Name of the target Blueprint
            node_type: Optional type of node to find (Event, Function, Variable, etc.)
            event_type: Optional specific event type to find (BeginPlay, Tick, etc.)

        Returns:
            Response containing array of found node IDs and success status
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_type": node_type,
                "event_type": event_type
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Finding nodes in blueprint '{blueprint_name}'")
            response = unreal.send_command("find_blueprint_nodes", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Node find response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error finding nodes: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_blueprint_node_details(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """
        Read a node's details and all its pin connections in a Blueprint's event graph.

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node GUID returned by find_blueprint_nodes or add tools

        Returns:
            Node info (title, class, position) and each pin with linked_to entries
            (node_id, node_title, pin) describing every connection
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_id": node_id
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Getting node details in blueprint '{blueprint_name}' node '{node_id}'")
            response = unreal.send_command("get_blueprint_node_details", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error getting node details: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def delete_blueprint_node(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Delete a node from a Blueprint's event graph and disconnect all its pins.

        The BeginPlay entry node is protected and cannot be deleted.

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node GUID of the node to delete

        Returns:
            Confirmation with the deleted node's id and title
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_id": node_id
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            logger.info(f"Deleting node '{node_id}' in blueprint '{blueprint_name}'")
            params["defer_save"] = defer_save
            response = unreal.send_command("delete_blueprint_node", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error deleting node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def disconnect_blueprint_nodes(
        ctx: Context,
        blueprint_name: str,
        source_node_id: str,
        source_pin: str,
        target_node_id: str,
        target_pin: str,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Break a single pin-to-pin connection, leaving every other link intact.

        Use this to remove one wire without deleting a node: unlike
        delete_blueprint_node, sibling connections survive.

        Args:
            blueprint_name: Name of the target Blueprint
            source_node_id: ID of the source node
            source_pin: Output pin name on the source node
            target_node_id: ID of the target node
            target_pin: Input pin name on the target node
            graph_name: Optional graph to operate on (ubergraph, function or macro
                        graph name). Defaults to the first Event Graph.

        Returns:
            Confirmation that the specified connection was removed
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "source_node_id": source_node_id,
                "source_pin": source_pin,
                "target_node_id": target_node_id,
                "target_pin": target_pin,
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params["defer_save"] = defer_save
            response = unreal.send_command("disconnect_blueprint_nodes", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error disconnecting nodes: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_blueprint_node_property(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        property_name: str,
        property_value: Any,
        graph_name: str = ""
,
        defer_save: bool = False
    ) -> Dict[str, Any]:
        """
        Set a reflected property on a Blueprint graph node.

        Works for reflected editable node properties, such as NodeComment.
        Function arguments (for example PrintString.bPrintToScreen) are input
        pin defaults: use set_blueprint_pin_default for those. The node is
        reconstructed afterwards so the change is reflected in the editor.

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node GUID of the node to edit
            property_name: Reflected property name
            property_value: New value (string, number, bool or array depending on type)
            graph_name: Optional graph name; defaults to the first Event Graph.

        Returns:
            Confirmation echoing the node id and property name
        """
        from unreal_mcp_server import get_unreal_connection, no_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_id": node_id,
                "property_name": property_name,
                "property_value": property_value,
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                return no_connection()

            params["defer_save"] = defer_save
            response = unreal.send_command("set_blueprint_node_property", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error setting node property: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_blueprint_pin_default(ctx: Context, blueprint_name: str, node_id: str,
                                 pin_name: str, default_value: str, graph_name: str = "",
                                 defer_save: bool = False) -> Dict[str, Any]:
        """Set an unconnected input pin's literal using the graph schema's validation.

        Use this for function arguments such as PrintString.bPrintToScreen;
        function arguments are pin defaults, not properties on the call node.
        default_value uses Unreal's literal format, e.g. 'true', '3.5' or a string.
        """
        from unreal_mcp_server import get_unreal_connection, no_connection
        unreal = get_unreal_connection()
        if not unreal:
            return no_connection()
        return unreal.send_command("set_blueprint_pin_default", {
            "blueprint_name": blueprint_name, "node_id": node_id, "pin_name": pin_name,
            "default_value": default_value, "graph_name": graph_name,
            "defer_save": defer_save}) or {
                "success": False, "message": "No response from Unreal Engine"}

    logger.info("Blueprint node tools registered successfully")
