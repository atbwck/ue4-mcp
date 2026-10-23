#!/usr/bin/env python
"""
Test script for creating and manipulating actors in Unreal Engine via MCP.

This script demonstrates the basic actor manipulation capabilities of the MCP system:
- Creating actors with specific names and transforms
- Getting actor properties
- Modifying actor transforms
- Error handling and validation
"""

import pathlib
import sys
import os
import time
import socket
import json
import logging
from typing import Dict, Any, Optional

# Add the parent directory to the path so we can import the server module
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Set up logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger("TestCube")


# 脚本可从仓库根或其子目录运行：把 scripts/ 加入导入路径
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from _common import send_command
def create_test_cube(name: str, location: list[float]) -> Optional[Dict[str, Any]]:
    """Create a test cube actor with the specified name and location.

    Args:
        name: The name to give the cube actor
        location: The [x, y, z] world location to spawn at

    Returns:
        Optional[Dict[str, Any]]: The response from the create command, or None if failed
    """
    cube_params = {
        "name": name,
        "type": "StaticMeshActor",
        "location": location,
        "rotation": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0]
    }

    response = send_command("create_actor", cube_params)
    if not response or response.get("status") != "success":
        logger.error(f"Failed to create cube: {response}")
        return None

    logger.info(f"Created cube '{name}' successfully at location {location}")
    return response

def get_actor_properties(name: str) -> Optional[Dict[str, Any]]:
    """Get the properties of an actor by name.

    Args:
        name: The name of the actor to get properties for

    Returns:
        Optional[Dict[str, Any]]: The actor properties, or None if not found/error
    """
    response = send_command("get_actor_properties", {"name": name})
    if not response or response.get("status") != "success":
        logger.error(f"Failed to get properties for actor '{name}': {response}")
        return None

    logger.info(f"Got properties for actor '{name}' successfully")
    return response

def set_actor_transform(
    name: str,
    location: Optional[list[float]] = None,
    rotation: Optional[list[float]] = None,
    scale: Optional[list[float]] = None
) -> Optional[Dict[str, Any]]:
    """Set the transform of an actor.

    Args:
        name: The name of the actor to modify
        location: Optional new [x, y, z] location
        rotation: Optional new [pitch, yaw, roll] rotation in degrees
        scale: Optional new [x, y, z] scale

    Returns:
        Optional[Dict[str, Any]]: The updated actor properties, or None if failed
    """
    transform_params = {"name": name}
    if location is not None:
        transform_params["location"] = location
    if rotation is not None:
        transform_params["rotation"] = rotation
    if scale is not None:
        transform_params["scale"] = scale

    response = send_command("set_actor_transform", transform_params)
    if not response or response.get("status") != "success":
        logger.error(f"Failed to set transform for actor '{name}': {response}")
        return None

    logger.info(f"Modified transform for actor '{name}' successfully")
    return response

def main():
    """Main function to test actor creation and manipulation."""
    try:
        # Create first test cube
        cube1_name = "TestCube_001"
        cube1 = create_test_cube(cube1_name, [0.0, 0.0, 100.0])
        if not cube1:
            logger.error("Failed to create first test cube")
            return

        # Get its properties to verify creation
        props = get_actor_properties(cube1_name)
        if not props:
            logger.error("Failed to verify first test cube properties")
            return

        # Modify its transform
        result = set_actor_transform(
            cube1_name,
            location=[0.0, 0.0, 200.0],
            rotation=[0.0, 45.0, 0.0],
            scale=[2.0, 2.0, 2.0]
        )
        if not result:
            logger.error("Failed to modify first test cube transform")
            return

        # Create a second test cube at a different location
        cube2_name = "TestCube_002"
        cube2 = create_test_cube(cube2_name, [100.0, 100.0, 100.0])
        if not cube2:
            logger.error("Failed to create second test cube")
            return

        logger.info("All test operations completed successfully!")

    except Exception as e:
        logger.error(f"Error in main: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
