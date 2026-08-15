#pragma once

// For use by EGSS applications.

#include "Egss/Application.h"
#include "Egss/Log.h"
#include "Egss/Layer.h"
#include "Egss/Timestep.h"
#include "Egss/Debug/Instrumentor.h"
#include "Egss/Debug/ScreenCapture.h"
#include "Egss/Debug/Replay.h"
#include "Egss/Debug/ReplayParams.h"
#include "Egss/ImGui/ImGuiLayer.h"

#include "Egss/Json.h"
#include "Egss/Input.h"
#include "Egss/KeyCodes.h"
#include "Egss/MouseButtonCodes.h"

#include "Egss/Events/Event.h"
#include "Egss/Events/ApplicationEvent.h"
#include "Egss/Events/KeyEvent.h"
#include "Egss/Events/MouseEvent.h"

// AUDIO ******************

#include "Egss/Audio/AudioClip.h"
#include "Egss/Audio/AudioEngine.h"
#include "Egss/Audio/Acoustics2D.h"
#include "Egss/Audio/Acoustics3D.h"

#include "Egss/Voxel/VoxelField3D.h"
#include "Egss/Voxel/MarchingCubes.h"
#include "Egss/Voxel/VoxelIslands.h"
#include "Egss/Voxel/VoxelStress.h"

// SCENE ******************

#include "Egss/Scene/Entity.h"
#include "Egss/Scene/Components.h"
#include "Egss/Scene/Scene.h"

// PHYSICS ****************

#include "Egss/Physics/RigidBody2D.h"
#include "Egss/Physics/PhysicsWorld2D.h"
#include "Egss/Physics/PhysicsWorld3D.h"
#include "Egss/Physics/Raycast3D.h"
#include "Egss/Physics/Sat2D.h"
#include "Egss/Physics/Sat3D.h"

// RENDERER ***************

#include "Egss/Renderer/Renderer.h"
#include "Egss/Renderer/Renderer2D.h"
#include "Egss/Renderer/RenderCommand.h"
#include "Egss/Renderer/Buffer.h"
#include "Egss/Renderer/VertexArray.h"
#include "Egss/Renderer/Shader.h"
#include "Egss/Renderer/ShaderLibrary.h"
#include "Egss/Renderer/Material.h"
#include "Egss/Renderer/Texture.h"
#include "Egss/Renderer/Framebuffer.h"
#include "Egss/Renderer/SubTexture2D.h"
#include "Egss/Renderer/Mesh.h"
#include "Egss/Renderer/ObjLoader.h"
#include "Egss/Renderer/MtlLoader.h"
#include "Egss/Renderer/GltfLoader.h"
#include "Egss/Renderer/Camera.h"
#include "Egss/Renderer/OrthographicCamera.h"
#include "Egss/Renderer/PerspectiveCamera.h"
#include "Egss/Renderer/Frustum.h"

// ENTRY POINT ************

#include "Egss/EntryPoint.h"
