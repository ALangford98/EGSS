#pragma once

// For use by GS applications.

#include "GS/Application.h"
#include "GS/Log.h"
#include "GS/Layer.h"
#include "GS/Timestep.h"
#include "GS/Debug/Instrumentor.h"
#include "GS/Debug/ScreenCapture.h"
#include "GS/Debug/Replay.h"
#include "GS/Debug/ReplayParams.h"
#include "GS/ImGui/ImGuiLayer.h"

#include "GS/Json.h"
#include "GS/Input.h"
#include "GS/KeyCodes.h"
#include "GS/MouseButtonCodes.h"

#include "GS/Events/Event.h"
#include "GS/Events/ApplicationEvent.h"
#include "GS/Events/KeyEvent.h"
#include "GS/Events/MouseEvent.h"

// AUDIO ******************

#include "GS/Audio/AudioClip.h"
#include "GS/Audio/AudioEngine.h"
#include "GS/Audio/Acoustics2D.h"
#include "GS/Audio/Acoustics3D.h"

#include "GS/Voxel/VoxelField3D.h"
#include "GS/Voxel/MarchingCubes.h"
#include "GS/Voxel/MarchingTetrahedra.h"
#include "GS/Voxel/VoxelTransition.h"
#include "GS/Voxel/VoxelIslands.h"
#include "GS/Voxel/VoxelStress.h"

// SCENE ******************

#include "GS/Scene/Entity.h"
#include "GS/Scene/Components.h"
#include "GS/Scene/Scene.h"

// PHYSICS ****************

#include "GS/Physics/RigidBody2D.h"
#include "GS/Physics/PhysicsWorld2D.h"
#include "GS/Physics/PhysicsWorld3D.h"
#include "GS/Physics/Raycast3D.h"
#include "GS/Physics/Sat2D.h"
#include "GS/Physics/Sat3D.h"

// RENDERER ***************

#include "GS/Renderer/Renderer.h"
#include "GS/Renderer/Renderer2D.h"
#include "GS/Renderer/RenderCommand.h"
#include "GS/Renderer/Buffer.h"
#include "GS/Renderer/VertexArray.h"
#include "GS/Renderer/Shader.h"
#include "GS/Renderer/ShaderLibrary.h"
#include "GS/Renderer/Material.h"
#include "GS/Renderer/Texture.h"
#include "GS/Renderer/Framebuffer.h"
#include "GS/Renderer/SubTexture2D.h"
#include "GS/Renderer/Mesh.h"
#include "GS/Renderer/ObjLoader.h"
#include "GS/Renderer/MtlLoader.h"
#include "GS/Renderer/GltfLoader.h"
#include "GS/Renderer/MeshCache.h"
#include "GS/Renderer/Camera.h"
#include "GS/Renderer/OrthographicCamera.h"
#include "GS/Renderer/PerspectiveCamera.h"
#include "GS/Renderer/Frustum.h"

// ENTRY POINT ************

#include "GS/EntryPoint.h"
