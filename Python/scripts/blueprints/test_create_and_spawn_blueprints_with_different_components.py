#!/usr/bin/env python
"""
Comprehensive test suite for creating and spawning Blueprints with various component types in Unreal Engine via MCP.

The script runs three main test scenarios:

1. Static Mesh Components Test:
   - Creates three different Blueprints with StaticMeshComponents:
     * BP_CubeMesh: A basic cube at [0, 0, 100]
     * BP_SphereMesh: A sphere at [100, 0, 100]
     * BP_CylinderMesh: A cylinder at [0, 100, 100], rotated 90° and stretched (scale: [0.75, 0.75, 2.0])

2. Collision Components Test:
   - Tests creation of physics collision shapes:
     * BP_BoxCollision: Box collision with 'BlockAll' profile
     * BP_SphereCollision: Sphere collision with 'OverlapAll' profile and 100 unit radius

3. Scene Component Hierarchy Test (Currently Disabled):
   - Demonstrates component parenting with:
     * Root SceneComponent
     * Child cube mesh offset 50 units right
     * Child sphere mesh offset 50 units left

Each test follows a consistent workflow:
1. Create the Blueprint
2. Add and configure components
3. Set necessary properties (meshes, collision profiles, etc.)
4. Compile the Blueprint
5. Spawn an instance in the level

The script provides detailed logging and error handling for each operation,
making it useful both as a test suite and as an example of the MCP Blueprint API usage.
"""

import pathlib
import sys
import os
import time
import socket
import json
import logging
from typing import Dict, Any, Optional, List

# Add the parent directory to the path so we can import the server module
sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

# Set up logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger("TestComponentCreation")


# 脚本可从仓库根或其子目录运行：把 scripts/ 加入导入路径
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from _common import send_command
def create_blueprint(name: str, parent_class: str = "Actor") -> bool:
    """Create a blueprint with the given name and parent class."""
    bp_params = {
        "name": name,
        "parent_class": parent_class
    }

    response = send_command("create_blueprint", bp_params)

    # Check response
    if not response or response.get("status") != "success":
        logger.error(f"Failed to create blueprint: {response}")
        return False

    # Check if blueprint already existed
    if response.get("result", {}).get("already_exists"):
        logger.info(f"Blueprint '{name}' already exists, reusing it")
    else:
        logger.info(f"Blueprint '{name}' created successfully!")

    return True

def add_component(
    blueprint_name: str,
    component_type: str,
    component_name: str,
    location: List[float] = [],
    rotation: List[float] = [],
    scale: List[float] = [],
    properties: Dict[str, Any] = {}
) -> bool:
    """Add a component to the specified blueprint."""
    component_params: Dict[str, Any] = {
        "blueprint_name": blueprint_name,
        "component_type": component_type,
        "component_name": component_name
    }

    # Add optional parameters if provided
    if location:
        component_params["location"] = location
    if rotation:
        component_params["rotation"] = rotation
    if scale:
        component_params["scale"] = scale
    response = send_command("add_component_to_blueprint", component_params)

    # Check response
    if not response or response.get("status") != "success":
        logger.error(f"Failed to add {component_type} component '{component_name}': {response}")
        return False

    # 组件创建接口不处理属性字典，必须逐项设置并检查失败
    for property_name, property_value in (properties or {}).items():
        response = send_command("set_component_property", {
            "blueprint_name": blueprint_name,
            "component_name": component_name,
            "property_name": property_name,
            "property_value": property_value,
        })
        if not response or response.get("status") != "success":
            logger.error(f"Failed to set component property '{property_name}': {response}")
            return False

    logger.info(f"Component '{component_name}' of type '{component_type}' added successfully!")
    return True

def set_static_mesh(
    blueprint_name: str,
    component_name: str,
    mesh_type: str
) -> bool:
    """Set the static mesh for a component."""
    # Convert simple shape name to full asset path if needed
    if mesh_type in ["Cube", "Sphere", "Cylinder", "Cone", "Plane"]:
        static_mesh = f"/Engine/BasicShapes/{mesh_type}.{mesh_type}"
    else:
        # Assume it's already a full path
        static_mesh = mesh_type

    params = {
        "blueprint_name": blueprint_name,
        "component_name": component_name,
        "static_mesh": static_mesh
    }

    response = send_command("set_static_mesh_properties", params)

    # Check response
    if not response or response.get("status") != "success":
        logger.error(f"Failed to set static mesh '{mesh_type}' for component '{component_name}': {response}")
        return False

    logger.info(f"Static mesh '{mesh_type}' set for component '{component_name}' successfully!")
    return True

def compile_blueprint(blueprint_name: str) -> bool:
    """Compile the specified blueprint."""
    compile_params = {
        "blueprint_name": blueprint_name
    }

    response = send_command("compile_blueprint", compile_params)

    # Check response
    if not response or response.get("status") != "success":
        logger.error(f"Failed to compile blueprint '{blueprint_name}': {response}")
        return False

    logger.info(f"Blueprint '{blueprint_name}' compiled successfully!")
    return True

def spawn_blueprint_actor(blueprint_name: str, actor_name: str, location: List[float] = []) -> bool:
    """Spawn an actor from the specified blueprint."""
    spawn_params: Dict[str, Any] = {
        "blueprint_name": blueprint_name,
        "actor_name": actor_name
    }

    # Add location if provided
    if location:
        spawn_params["location"] = location
    else:
        spawn_params["location"] = [0.0, 0.0, 100.0]  # Default 100 units up

    response = send_command("spawn_blueprint_actor", spawn_params)

    # Check response
    if not response or response.get("status") != "success":
        logger.error(f"Failed to spawn actor from blueprint '{blueprint_name}': {response}")
        return False

    logger.info(f"Actor '{actor_name}' spawned from blueprint '{blueprint_name}' successfully!")
    return True

def test_static_mesh_components():
    """Test creating blueprints with different static mesh components."""
    logger.info("\n=== Testing Static Mesh Components ===\n")

    # Test cube mesh
    logger.info("Testing cube mesh component...")
    bp_cube_name = "BP_CubeMesh"
    if not create_blueprint(bp_cube_name):
        return False

    if not add_component(
        blueprint_name=bp_cube_name,
        component_type="StaticMeshComponent",
        component_name="CubeMeshComponent",
        location=[0.0, 0.0, 0.0],
        scale=[1.0, 1.0, 1.0],
        properties={"bVisible": True}
    ):
        return False

    if not set_static_mesh(
        blueprint_name=bp_cube_name,
        component_name="CubeMeshComponent",
        mesh_type="Cube"
    ):
        return False

    if not compile_blueprint(bp_cube_name):
        return False

    if not spawn_blueprint_actor(bp_cube_name, "CubeMeshActor", [0.0, 0.0, 100.0]):
        return False

    # Test sphere mesh
    logger.info("Testing sphere mesh component...")
    bp_sphere_name = "BP_SphereMesh"
    if not create_blueprint(bp_sphere_name):
        return False

    if not add_component(
        blueprint_name=bp_sphere_name,
        component_type="StaticMeshComponent",
        component_name="SphereMeshComponent",
        location=[0.0, 0.0, 0.0],
        scale=[1.0, 1.0, 1.0],
        properties={"bVisible": True}
    ):
        return False

    if not set_static_mesh(
        blueprint_name=bp_sphere_name,
        component_name="SphereMeshComponent",
        mesh_type="Sphere"
    ):
        return False

    if not compile_blueprint(bp_sphere_name):
        return False

    if not spawn_blueprint_actor(bp_sphere_name, "SphereMeshActor", [100.0, 0.0, 100.0]):
        return False

    # Test cylinder mesh
    logger.info("Testing cylinder mesh component...")
    bp_cylinder_name = "BP_CylinderMesh"
    if not create_blueprint(bp_cylinder_name):
        return False

    if not add_component(
        blueprint_name=bp_cylinder_name,
        component_type="StaticMeshComponent",
        component_name="CylinderMeshComponent",
        location=[0.0, 0.0, 0.0],
        rotation=[0.0, 0.0, 90.0],  # Rotated 90 degrees
        scale=[0.75, 0.75, 2.0],  # Stretched in Z
        properties={"bVisible": True}
    ):
        return False

    if not set_static_mesh(
        blueprint_name=bp_cylinder_name,
        component_name="CylinderMeshComponent",
        mesh_type="Cylinder"
    ):
        return False

    if not compile_blueprint(bp_cylinder_name):
        return False

    if not spawn_blueprint_actor(bp_cylinder_name, "CylinderMeshActor", [0.0, 100.0, 100.0]):
        return False

    return True

def test_collision_components():
    """Test creating blueprints with different collision components."""
    logger.info("\n=== Testing Collision Components ===\n")

    # Test box collision component
    logger.info("Testing box collision component...")
    bp_box_name = "BP_BoxCollision"
    if not create_blueprint(bp_box_name):
        return False

    if not add_component(
        blueprint_name=bp_box_name,
        component_type="BoxComponent",
        component_name="BoxCollisionComponent",
        location=[0.0, 0.0, 0.0],
        scale=[1.0, 1.0, 1.0],
        properties={
            "bVisible": True,
            "CollisionProfileName": "BlockAll"
        }
    ):
        return False

    if not compile_blueprint(bp_box_name):
        return False

    if not spawn_blueprint_actor(bp_box_name, "BoxCollisionActor", [0.0, 0.0, 100.0]):
        return False

    # Test sphere collision component
    logger.info("Testing sphere collision component...")
    bp_sphere_name = "BP_SphereCollision"
    if not create_blueprint(bp_sphere_name):
        return False

    if not add_component(
        blueprint_name=bp_sphere_name,
        component_type="SphereComponent",
        component_name="SphereCollisionComponent",
        location=[0.0, 0.0, 0.0],
        scale=[1.0, 1.0, 1.0],
        properties={
            "bVisible": True,
            "CollisionProfileName": "OverlapAll",
            "SphereRadius": 100.0  # Custom radius
        }
    ):
        return False

    if not compile_blueprint(bp_sphere_name):
        return False

    if not spawn_blueprint_actor(bp_sphere_name, "SphereCollisionActor", [100.0, 0.0, 100.0]):
        return False

    return True

def test_scene_component_hierarchy():
    """Test creating a blueprint with a hierarchy of scene components."""
    logger.info("\n=== Testing Scene Component Hierarchy ===\n")

    # Create a blueprint for the hierarchy test
    bp_name = "BP_ComponentHierarchy"
    if not create_blueprint(bp_name):
        return False

    # Add a root scene component
    logger.info("Adding root SceneComponent...")
    if not add_component(
        blueprint_name=bp_name,
        component_type="SceneComponent",
        component_name="RootSceneComponent",
        location=[0.0, 0.0, 0.0]
    ):
        return False

    # Compile the blueprint after adding the root component
    logger.info("Compiling blueprint after adding root component...")
    if not compile_blueprint(bp_name):
        return False

    # Add cube mesh as first child
    logger.info("Adding cube mesh as first child...")
    cube_component_name = "ChildCubeMeshComponent"
    if not add_component(
        blueprint_name=bp_name,
        component_type="StaticMeshComponent",
        component_name=cube_component_name,
        location=[50.0, 0.0, 0.0]
    ):
        return False

    # Compile the blueprint after adding the first child
    logger.info("Compiling blueprint after adding first child...")
    if not compile_blueprint(bp_name):
        return False

    # Set mesh type for cube
    if not set_static_mesh(
        blueprint_name=bp_name,
        component_name=cube_component_name,
        mesh_type="Cube"
    ):
        return False

    # Compile the blueprint after setting the mesh
    logger.info("Compiling blueprint after setting first mesh...")
    if not compile_blueprint(bp_name):
        return False

    # Add sphere mesh as second child
    logger.info("Adding sphere mesh as second child...")
    sphere_component_name = "ChildSphereMeshComponent"
    if not add_component(
        blueprint_name=bp_name,
        component_type="StaticMeshComponent",
        component_name=sphere_component_name,
        location=[-50.0, 0.0, 0.0]
    ):
        return False

    # Compile the blueprint after adding the second child
    logger.info("Compiling blueprint after adding second child...")
    if not compile_blueprint(bp_name):
        return False

    # Set mesh type for sphere
    if not set_static_mesh(
        blueprint_name=bp_name,
        component_name=sphere_component_name,
        mesh_type="Sphere"
    ):
        return False

    # Final compilation
    logger.info("Final blueprint compilation...")
    if not compile_blueprint(bp_name):
        return False

    # Spawn an actor from the blueprint
    logger.info("Spawning hierarchy actor...")
    if not spawn_blueprint_actor(bp_name, "HierarchyTestActor", [0.0, 0.0, 100.0]):
        return False

    return True

def main():
    """Main function to test different component creation scenarios."""
    try:
        # Test different component scenarios
        tests = [
            test_static_mesh_components,
            test_collision_components,
            # test_scene_component_hierarchy # TODO: Not working
        ]

        success_count = 0

        for test_func in tests:
            if test_func():
                success_count += 1
                logger.info(f"{test_func.__name__} completed successfully!")
            else:
                logger.error(f"{test_func.__name__} failed!")

            # Add a delay between tests
            time.sleep(2)

        # Report overall results
        logger.info(f"\n=== Test Results: {success_count}/{len(tests)} tests passed ===\n")

    except Exception as e:
        logger.error(f"Error during testing: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
