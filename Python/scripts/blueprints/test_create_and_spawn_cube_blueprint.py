#!/usr/bin/env python
"""
Test script for creating and manipulating a basic Blueprint in Unreal Engine via MCP.

This script demonstrates the core Blueprint creation and manipulation workflow:
1. Creates a new Blueprint class (TestBP) derived from Actor
2. Adds a StaticMeshComponent named 'CubeVisual' to the Blueprint
3. Sets the component's mesh to a basic cube shape (/Engine/BasicShapes/Cube.Cube)
4. Compiles the Blueprint to apply the changes
5. Spawns an instance of the Blueprint in the level at [0, 0, 100]

The script serves as both a test of the MCP Blueprint manipulation functionality
and a basic example of how to programmatically create and modify Blueprints
through the MCP interface.
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
sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

# Set up logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger("TestBasicBlueprint")


# 脚本可从仓库根或其子目录运行：把 scripts/ 加入导入路径
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from _common import send_command_on as send_command
def main():
    """Main function to test creating a basic blueprint."""
    try:
        # Connect to Unreal MCP server
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("127.0.0.1", 55557))

        try:
            # Step 1: Create a blueprint
            bp_params = {
                "name": "TestBP",
                "parent_class": "Actor"
            }

            response = send_command(sock, "create_blueprint", bp_params)

            # Fixed response check to handle nested structure
            if not response or response.get("status") != "success":
                logger.error(f"Failed to create blueprint: {response}")
                return

            # Check if blueprint already existed
            if response.get("result", {}).get("already_exists"):
                logger.info(f"Blueprint 'TestBP' already exists, reusing it")
            else:
                logger.info("Blueprint created successfully!")

            # Step 2: Add a static mesh component
            component_params = {
                "blueprint_name": "TestBP",
                "component_type": "StaticMeshComponent",
                "component_name": "CubeVisual",
                "location": [0.0, 0.0, 0.0],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0]
            }

            # Close and reopen connection for each command
            sock.close()
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(("127.0.0.1", 55557))

            response = send_command(sock, "add_component_to_blueprint", component_params)

            # Fixed response check to handle nested structure
            if not response or response.get("status") != "success":
                logger.error(f"Failed to add component: {response}")
                return

            logger.info("Component added successfully!")

            # Step 3: Set the static mesh properties
            sock.close()
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(("127.0.0.1", 55557))

            mesh_params = {
                "blueprint_name": "TestBP",
                "component_name": "CubeVisual",
                "static_mesh": "/Engine/BasicShapes/Cube.Cube"
            }

            response = send_command(sock, "set_static_mesh_properties", mesh_params)

            # Fixed response check to handle nested structure
            if not response or response.get("status") != "success":
                logger.error(f"Failed to set static mesh properties: {response}")
                return

            logger.info("Static mesh properties set successfully!")

            # Step 4: Compile the blueprint
            sock.close()
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(("127.0.0.1", 55557))

            compile_params = {
                "blueprint_name": "TestBP"
            }

            response = send_command(sock, "compile_blueprint", compile_params)

            # Fixed response check to handle nested structure
            if not response or response.get("status") != "success":
                logger.error(f"Failed to compile blueprint: {response}")
                return

            logger.info("Blueprint compiled successfully!")

            # Step 5: Spawn an instance of the blueprint
            sock.close()
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(("127.0.0.1", 55557))

            spawn_params = {
                "blueprint_name": "TestBP",
                "actor_name": "TestBPInstance",
                "location": [0.0, 0.0, 100.0],  # 100 units up
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0]
            }

            response = send_command(sock, "spawn_blueprint_actor", spawn_params)

            # Fixed response check to handle nested structure
            if not response or response.get("status") != "success":
                logger.error(f"Failed to spawn blueprint actor: {response}")
                return

            logger.info("Blueprint actor spawned successfully!")

        finally:
            # Close the socket
            sock.close()

    except Exception as e:
        logger.error(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
