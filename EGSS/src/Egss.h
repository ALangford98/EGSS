#pragma once

// For use by EGSS applications.

#include "Egss/Application.h"
#include "Egss/Log.h"
#include "Egss/Layer.h"
#include "Egss/Timestep.h"
#include "Egss/ImGui/ImGuiLayer.h"

#include "Egss/Input.h"
#include "Egss/KeyCodes.h"
#include "Egss/MouseButtonCodes.h"

#include "Egss/Events/Event.h"
#include "Egss/Events/ApplicationEvent.h"
#include "Egss/Events/KeyEvent.h"
#include "Egss/Events/MouseEvent.h"

// RENDERER ***************

#include "Egss/Renderer/Renderer.h"
#include "Egss/Renderer/Renderer2D.h"
#include "Egss/Renderer/RenderCommand.h"
#include "Egss/Renderer/Buffer.h"
#include "Egss/Renderer/VertexArray.h"
#include "Egss/Renderer/Shader.h"
#include "Egss/Renderer/Texture.h"
#include "Egss/Renderer/Framebuffer.h"
#include "Egss/Renderer/SubTexture2D.h"
#include "Egss/Renderer/Camera.h"
#include "Egss/Renderer/OrthographicCamera.h"
#include "Egss/Renderer/PerspectiveCamera.h"

// ENTRY POINT ************

#include "Egss/EntryPoint.h"
