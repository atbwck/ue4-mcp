"""
Material Tools for Unreal MCP.

材质资产图编辑、材质实例参数编辑，以及网格组件上的材质分配与运行态验证。

寻址约定：
- 材质图节点用 node_id（优先表达式 GUID，没有 GUID 时用对象名），由 get_material_graph 返回。
- 材质属性用名字，接受 BaseColor、MP_BaseColor 这类写法。
- 材质实例参数按名字寻址，值类型由 scalar / vector / texture / static_switch 之一决定。
"""

import logging
import math
from typing import Any, Dict, List, Optional

from mcp.server.fastmcp import FastMCP, Context

logger = logging.getLogger("UnrealMCP")


def _color_channels(value: Any) -> Optional[List[float]]:
    """把 3 或 4 分量颜色规整成 float 列表；非法输入返回 None。"""
    if not isinstance(value, (list, tuple)) or not 3 <= len(value) <= 4:
        return None
    channels = []
    for item in value:
        if isinstance(item, bool) or not isinstance(item, (int, float)) or not math.isfinite(item):
            return None
        channels.append(float(item))
    return channels


def _send(command: str, params: Dict[str, Any]) -> Dict[str, Any]:
    """统一发送命令：连接缺失与空响应用同一种可识别结构报错。"""
    from unreal_mcp_server import get_unreal_connection, no_connection

    try:
        unreal = get_unreal_connection()
        if not unreal:
            return no_connection()
        response = unreal.send_command(command, params)
        if not response:
            logger.error(f"No response from Unreal Engine for {command}")
            return {"success": False, "message": "No response from Unreal Engine"}
        return response
    except Exception as e:
        logger.error(f"Error sending {command}: {e}")
        return {"success": False, "message": str(e)}


def register_material_tools(mcp: FastMCP):
    """Register Material tools with the MCP server."""


    @mcp.tool()
    def create_material(
        ctx: Context,
        material_name: str,
        save_path: str = "/Game/Materials",
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Create a new Material asset.

        Args:
            material_name: Name of the new material asset (no path, no extension).
            save_path: Content browser package path (default "/Game/Materials").
            defer_save: Leave the new asset in memory for a later save_assets call.

        Returns:
            Response with the created material's asset path.
        """
        return _send("create_material", {
            "material_name": material_name,
            "save_path": save_path,
            "defer_save": defer_save,
        })

    @mcp.tool()
    def create_material_instance(
        ctx: Context,
        instance_name: str,
        parent_material_path: str,
        save_path: str = "/Game/Materials",
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Create a persistent Material Instance Constant asset from a parent material.

        The instance overrides parameters; it does not own the node graph. Set values
        with set_material_instance_parameter afterwards.

        Args:
            instance_name: New asset name (no path, no extension).
            parent_material_path: Parent material or material instance asset path.
            save_path: Content browser package path (default "/Game/Materials").
            defer_save: Leave the new asset in memory for a later save_assets call.

        Returns:
            Response with the created instance path and its parent.
        """
        return _send("create_material_instance", {
            "instance_name": instance_name,
            "parent_material_path": parent_material_path,
            "save_path": save_path,
            "defer_save": defer_save,
        })


    @mcp.tool()
    def get_material_graph(ctx: Context, material_path: str) -> Dict[str, Any]:
        """
        Read a material asset's graph: expressions, links, property inputs and settings.

        Use this before editing. Every node reports node_id, class, caption, position,
        inputs (with the node each one consumes) and outputs (with names).

        Args:
            material_path: Asset path, or a bare asset name.

        Returns:
            Response with "expressions", "property_inputs", "comments" and "settings".
        """
        return _send("get_material_graph", {"material_path": material_path})

    @mcp.tool()
    def list_material_parameters(ctx: Context, material_path: str) -> Dict[str, Any]:
        """
        List the scalar, vector, texture and static switch parameters on a material.

        This is how you learn which parameter names a material instance may override.

        Args:
            material_path: Asset path, or a bare asset name.

        Returns:
            Response with one array per parameter kind, each holding name and defaults.
        """
        return _send("list_material_parameters", {"material_path": material_path})

    @mcp.tool()
    def get_material_instance_parameters(ctx: Context, instance_path: str) -> Dict[str, Any]:
        """
        Read a material instance's parent and its current parameter overrides.

        Args:
            instance_path: Material instance asset path, or a bare asset name.

        Returns:
            Response with parent path and scalar, vector, texture and static switch overrides.
        """
        return _send("get_material_instance_parameters", {"instance_path": instance_path})


    @mcp.tool()
    def add_material_expression(
        ctx: Context,
        material_path: str,
        expression_class: str,
        parameter_name: str = "",
        properties: Optional[Dict[str, Any]] = None,
        x: Optional[int] = None,
        y: Optional[int] = None,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Add an expression node to a material graph.

        Args:
            material_path: Asset path, or a bare asset name.
            expression_class: Engine class name. Accepts ScalarParameter, VectorParameter,
                TextureSample, TextureSampleParameter2D, Constant, Constant3Vector,
                Multiply, Add, LinearInterpolate and StaticSwitchParameter. Use
                add_material_comment for comment boxes.
            parameter_name: Sets the parameter name; rejected for non-parameter classes.
            properties: Node properties written after creation, for example
                {"Constant": [1, 0, 0, 1]} or {"DefaultValue": 0.5}.
            x: Optional explicit X. Omit to let the layout place the node.
            y: Optional explicit Y. Omit to let the layout place the node.

        Returns:
            Response with the new node's node_id, class, caption and position.
        """
        params: Dict[str, Any] = {
            "material_path": material_path,
            "expression_class": expression_class,
        }
        if parameter_name:
            params["parameter_name"] = parameter_name
        if properties:
            params["properties"] = properties
        if x is not None:
            params["x"] = int(x)
        if y is not None:
            params["y"] = int(y)
        params["defer_save"] = defer_save
        return _send("add_material_expression", params)

    @mcp.tool()
    def set_material_expression_property(
        ctx: Context,
        material_path: str,
        node_id: str,
        properties: Dict[str, Any],
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Write editable properties on an existing material expression.

        Supported value shapes: strings (Desc, ParameterName), numbers (DefaultValue),
        booleans, name strings (Group), colours ([r, g, b] or [r, g, b, a] for Constant
        and DefaultValue), enum names (SamplerType) and asset paths for texture slots.

        Args:
            material_path: Asset path, or a bare asset name.
            node_id: Node id from get_material_graph.
            properties: Property name to value map.

        Returns:
            Response echoing the node and the properties that were written.
        """
        return _send("set_material_expression_property", {
            "material_path": material_path,
            "node_id": node_id,
            "properties": properties,
            "defer_save": defer_save,
        })

    @mcp.tool()
    def delete_material_expression(
        ctx: Context,
        material_path: str,
        node_id: str,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Delete an expression from a material graph and report the links it broke.

        Args:
            material_path: Asset path, or a bare asset name.
            node_id: Node id from get_material_graph.
            defer_save: Leave the change in memory for a later save_assets call.

        Returns:
            Response with the deleted node and any material property inputs it cleared.
        """
        return _send("delete_material_expression", {
            "material_path": material_path,
            "node_id": node_id,
            "defer_save": defer_save,
        })

    @mcp.tool()
    def connect_material_expressions(
        ctx: Context,
        material_path: str,
        from_node_id: str,
        to_node_id: str,
        from_output: str = "",
        from_output_index: Optional[int] = None,
        to_input: str = "",
        to_input_index: Optional[int] = None,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Connect one expression output to one expression input.

        Address the pins by name when they have one, otherwise by index. Omit both
        output locators to use the first output.

        Args:
            material_path: Asset path, or a bare asset name.
            from_node_id: Source node id.
            to_node_id: Target node id.
            from_output: Source output name, such as "R" or "RGB".
            from_output_index: Source output index when it has no name.
            to_input: Target input name.
            to_input_index: Target input index when it has no name.

        Returns:
            Response echoing the resolved pins.
        """
        params: Dict[str, Any] = {
            "material_path": material_path,
            "from_node_id": from_node_id,
            "to_node_id": to_node_id,
        }
        if from_output:
            params["from_output"] = from_output
        if from_output_index is not None:
            params["from_output_index"] = int(from_output_index)
        if to_input:
            params["to_input"] = to_input
        if to_input_index is not None:
            params["to_input_index"] = int(to_input_index)
        if not to_input and to_input_index is None:
            return {"success": False, "message": "Provide to_input or to_input_index"}
        params["defer_save"] = defer_save
        return _send("connect_material_expressions", params)

    @mcp.tool()
    def disconnect_material_expressions(
        ctx: Context,
        material_path: str,
        to_node_id: str,
        from_node_id: str = "",
        to_input: str = "",
        to_input_index: Optional[int] = None,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Break the link into one expression input.

        Pass from_node_id to remove a single link. Omit it to clear every source on
        that input, which is one link unless the expression fans inputs together.

        Args:
            material_path: Asset path, or a bare asset name.
            to_node_id: Target node id.
            from_node_id: Optional source node id to match.
            to_input: Target input name.
            to_input_index: Target input index when it has no name.

        Returns:
            Response listing the links that were removed.
        """
        params: Dict[str, Any] = {
            "material_path": material_path,
            "to_node_id": to_node_id,
        }
        if from_node_id:
            params["from_node_id"] = from_node_id
        if to_input:
            params["to_input"] = to_input
        if to_input_index is not None:
            params["to_input_index"] = int(to_input_index)
        if not to_input and to_input_index is None:
            return {"success": False, "message": "Provide to_input or to_input_index"}
        params["defer_save"] = defer_save
        return _send("disconnect_material_expressions", params)

    @mcp.tool()
    def connect_material_property(
        ctx: Context,
        material_path: str,
        from_node_id: str,
        property: str,
        from_output: str = "",
        from_output_index: Optional[int] = None,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Connect an expression output to a material property such as BaseColor.

        Args:
            material_path: Asset path, or a bare asset name.
            from_node_id: Source node id.
            property: Property name. Accepts BaseColor, Metallic, Specular, Roughness,
                Normal, EmissiveColor, Opacity, OpacityMask, AmbientOcclusion and MP_ forms.
            from_output: Source output name.
            from_output_index: Source output index when it has no name.

        Returns:
            Response echoing the property and resolved output.
        """
        params: Dict[str, Any] = {
            "material_path": material_path,
            "from_node_id": from_node_id,
            "property": property,
        }
        if from_output:
            params["from_output"] = from_output
        if from_output_index is not None:
            params["from_output_index"] = int(from_output_index)
        params["defer_save"] = defer_save
        return _send("connect_material_property", params)

    @mcp.tool()
    def disconnect_material_property(
        ctx: Context,
        material_path: str,
        property: str,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Clear the expression connected to one material property.

        Args:
            material_path: Asset path, or a bare asset name.
            property: Property name, such as BaseColor or Roughness.

        Returns:
            Response with the node that used to feed the property.
        """
        return _send("disconnect_material_property", {
            "material_path": material_path,
            "property": property,
            "defer_save": defer_save,
        })

    @mcp.tool()
    def set_material_settings(
        ctx: Context,
        material_path: str,
        blend_mode: str = "",
        shading_model: str = "",
        material_domain: str = "",
        two_sided: Optional[bool] = None,
        opacity_mask_clip_value: Optional[float] = None,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Change material settings, then recompile and report the resulting state.

        Args:
            material_path: Asset path, or a bare asset name.
            blend_mode: Opaque, Masked, Translucent, Additive, Modulate,
                AlphaComposite or AlphaHoldout.
            shading_model: DefaultLit, Unlit, Subsurface, PreintegratedSkin, ClearCoat,
                SubsurfaceProfile, TwoSidedFoliage, Hair, Cloth, Eye, SingleLayerWater
                or ThinTranslucent.
            material_domain: Surface, DeferredDecal, LightFunction, Volume,
                PostProcess or UI.
            two_sided: Render back faces.
            opacity_mask_clip_value: Mask threshold between 0 and 1.
            defer_save: Leave the change in memory for a later save_assets call.

        Returns:
            Response with the graph description and the settings that changed.
        """
        params: Dict[str, Any] = {"material_path": material_path, "defer_save": defer_save}
        if blend_mode:
            params["blend_mode"] = blend_mode
        if shading_model:
            params["shading_model"] = shading_model
        if material_domain:
            params["material_domain"] = material_domain
        if two_sided is not None:
            params["two_sided"] = bool(two_sided)
        if opacity_mask_clip_value is not None:
            value = float(opacity_mask_clip_value)
            if not math.isfinite(value) or not 0.0 <= value <= 1.0:
                return {"success": False, "message": "opacity_mask_clip_value must be between 0 and 1"}
            params["opacity_mask_clip_value"] = value
        return _send("set_material_settings", params)

    @mcp.tool()
    def add_material_comment(
        ctx: Context,
        material_path: str,
        text: str,
        x: Optional[int] = None,
        y: Optional[int] = None,
        size_x: Optional[int] = None,
        size_y: Optional[int] = None,
        color: Optional[List[float]] = None,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Add a comment box to a material graph to label a group of nodes.

        Args:
            material_path: Asset path, or a bare asset name.
            text: Comment text.
            x: Left edge. Omit to keep the engine default.
            y: Top edge. Omit to keep the engine default.
            size_x: Box width in graph units.
            size_y: Box height in graph units.
            color: Comment colour as [r, g, b] or [r, g, b, a].
            defer_save: Leave the change in memory for a later save_assets call.

        Returns:
            Response with the comment's node_id and box.
        """
        params: Dict[str, Any] = {
            "material_path": material_path,
            "text": text,
            "defer_save": defer_save,
        }
        if color is not None:
            channels = _color_channels(color)
            if channels is None:
                return {"success": False, "message": "color must be three or four finite numbers"}
            params["color"] = channels
        for name, value in (("x", x), ("y", y), ("size_x", size_x), ("size_y", size_y)):
            if value is not None:
                params[name] = int(value)
        return _send("add_material_comment", params)

    @mcp.tool()
    def format_material_graph(
        ctx: Context,
        material_path: str,
        dry_run: bool = False,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Lay out a material graph by data depth without touching nodes or links.

        Layout model: material property inputs sit in the rightmost column, each data
        step moves one column left, nodes in one column share X, and one property's
        chain occupies one row band. Comment boxes are never moved.

        Args:
            material_path: Asset path, or a bare asset name.
            dry_run: Return planned positions without changing the asset.
            defer_save: Leave the change in memory for a later save_assets call.

        Returns:
            Response with planned positions, comment boxes and warnings.
        """
        return _send("format_material_graph", {
            "material_path": material_path,
            "dry_run": dry_run,
            "defer_save": defer_save,
        })


    @mcp.tool()
    def set_material_instance_parameter(
        ctx: Context,
        instance_path: str,
        param_name: str,
        scalar: Optional[float] = None,
        vector: Optional[List[float]] = None,
        texture: Optional[str] = None,
        static_switch: Optional[bool] = None,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Override one parameter on a material instance constant.

        Provide exactly one of scalar, vector, texture or static_switch. The parameter
        must exist on the instance's parent; unknown names are rejected instead of
        being dropped.

        Args:
            instance_path: Material instance asset path, or a bare asset name.
            param_name: Parameter name on the parent material.
            scalar: Number for a scalar parameter.
            vector: [r, g, b] or [r, g, b, a] for a vector parameter.
            texture: Texture asset path for a texture parameter; empty clears it.
            static_switch: Boolean for a static switch parameter.
            defer_save: Leave the change in memory for a later save_assets call.

        Returns:
            Response echoing the parameter kind and value written.
        """
        provided = [value is not None for value in (scalar, vector, texture, static_switch)]
        if sum(provided) != 1:
            return {"success": False,
                    "message": "Provide exactly one of scalar, vector, texture or static_switch"}

        params: Dict[str, Any] = {
            "instance_path": instance_path,
            "param_name": param_name,
            "defer_save": defer_save,
        }
        if scalar is not None:
            if isinstance(scalar, bool) or not math.isfinite(scalar):
                return {"success": False, "message": "scalar must be finite"}
            params["scalar"] = float(scalar)
        elif vector is not None:
            channels = _color_channels(vector)
            if channels is None:
                return {"success": False, "message": "vector must be three or four finite numbers"}
            params["vector"] = channels
        elif texture is not None:
            params["texture"] = texture
        else:
            params["static_switch"] = bool(static_switch)
        return _send("set_material_instance_parameter", params)


    @mcp.tool()
    def add_vector_parameter(
        ctx: Context,
        material_name: str,
        param_name: str,
        default_color: Optional[List[float]] = None,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Add a vector parameter expression and connect it to BaseColor.

        This is the short path for the common case. For any other wiring, use
        add_material_expression with connect_material_expressions.

        Args:
            material_name: Material asset name or full asset path.
            param_name: Name of the vector parameter (for example "Color").
            default_color: Default colour as [r, g, b, a] in 0..1 range.
            defer_save: Leave the change in memory for a later save_assets call.

        Returns:
            Response with the new node and the property it feeds.
        """
        channels = [0.5, 0.5, 0.5, 1.0]
        if default_color is not None:
            parsed = _color_channels(default_color)
            if parsed is None:
                return {"success": False, "message": "default_color must be three or four finite numbers"}
            channels = parsed if len(parsed) == 4 else parsed + [1.0]
        return _send("add_vector_parameter", {
            "material_name": material_name,
            "param_name": param_name,
            "default_color": channels,
            "defer_save": defer_save,
        })

    @mcp.tool()
    def set_material_on_component(
        ctx: Context,
        actor_name: str,
        component_name: str,
        material_path: str,
        material_index: int = 0,
        defer_save: bool = False,
    ) -> Dict[str, Any]:
        """
        Assign a material asset to a mesh component on a level actor.

        Args:
            actor_name: Name of the actor in the current level.
            component_name: Exact name of the mesh component on that actor.
            material_path: Full asset path of the material
                (for example "/Game/Materials/M_Example.M_Example").
            material_index: Material slot index to set (default 0).
            defer_save: Assign without saving the level; call save_level afterwards.

        Returns:
            Response confirming the assignment.
        """
        return _send("set_material_on_component", {
            "actor_name": actor_name,
            "component_name": component_name,
            "material_path": material_path,
            "material_index": int(material_index),
            "defer_save": defer_save,
        })

    @mcp.tool()
    def create_dynamic_material_instance(
        ctx: Context,
        actor_name: str,
        component_name: str,
        material_index: int = 0,
        pie: bool = True,
    ) -> Dict[str, Any]:
        """
        Create a dynamic material instance on a mesh slot and assign it to the component.

        A DMI is a runtime object: it exists only in a play session, so call start_pie
        first and pass pie=true. Reuses an existing DMI on the slot instead of stacking.

        Args:
            actor_name: Actor name in the target world.
            component_name: Mesh component name on that actor.
            material_index: Material slot index (default 0).
            pie: Look the actor up in the PIE world instead of the editor world.

        Returns:
            Response with dynamic_instance path, parent material and created flag.
        """
        return _send("create_dynamic_material_instance", {
            "actor_name": actor_name,
            "component_name": component_name,
            "material_index": int(material_index),
            "pie": bool(pie),
        })

    @mcp.tool()
    def set_material_parameter(
        ctx: Context,
        actor_name: str,
        component_name: str,
        param_name: str,
        scalar: Optional[float] = None,
        vector: Optional[List[float]] = None,
        material_index: int = 0,
        pie: bool = True,
    ) -> Dict[str, Any]:
        """
        Set a scalar or vector parameter on a dynamic material instance.

        The slot must already hold a DMI (call create_dynamic_material_instance first);
        writing parameters onto a material asset would affect every user of that asset
        and is rejected. For persistent overrides use set_material_instance_parameter.

        Args:
            actor_name: Actor name in the target world.
            component_name: Mesh component name on that actor.
            param_name: Material parameter name.
            scalar: Scalar value; provide this or vector, not both.
            vector: [R, G, B] vector value.
            material_index: Material slot index (default 0).
            pie: Look the actor up in the PIE world instead of the editor world.

        Returns:
            Response echoing the parameter name, kind and value.
        """
        if (scalar is None) == (vector is None):
            return {"success": False, "message": "Provide exactly one of 'scalar' or 'vector'"}
        if vector is not None and (len(vector) != 3 or any(
                isinstance(c, bool) or not isinstance(c, (int, float)) or not math.isfinite(c) for c in vector)):
            return {"success": False, "message": "vector must contain three finite numbers"}
        if scalar is not None and (isinstance(scalar, bool) or not math.isfinite(scalar)):
            return {"success": False, "message": "scalar must be finite"}

        params = {
            "actor_name": actor_name,
            "component_name": component_name,
            "param_name": param_name,
            "material_index": int(material_index),
            "pie": bool(pie),
        }
        if scalar is not None:
            params["scalar"] = float(scalar)
        else:
            params["vector"] = [float(c) for c in vector]
        return _send("set_material_parameter", params)

    @mcp.tool()
    def get_component_materials(
        ctx: Context,
        actor_name: str,
        component_name: str,
        pie: bool = False,
    ) -> Dict[str, Any]:
        """
        Report every material slot on a mesh component.

        For each slot: the assigned material path, whether it is a dynamic material
        instance, its parent material, and the current scalar/vector parameter values.
        This is how you verify a material assignment or parameter write actually landed.

        Args:
            actor_name: Actor name in the target world.
            component_name: Mesh component name on that actor.
            pie: Look the actor up in the PIE world instead of the editor world.

        Returns:
            Response with "slots", each holding material_index, material_path,
            is_dynamic_instance and (for DMIs) parent_material and parameter values.
        """
        return _send("get_component_materials", {
            "actor_name": actor_name,
            "component_name": component_name,
            "pie": bool(pie),
        })

    logger.info("Material tools registered successfully")
