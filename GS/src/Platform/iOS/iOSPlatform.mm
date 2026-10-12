// iOS (export piece 7): the window, input and run loop GLFW provides on the
// desktop, in UIKit's terms. Compiled only by the Xcode project the iOS
// export writes (gs.py xcode) -- never by premake, and never on this
// machine: there is no iOS SDK on Linux. Written against the documented
// UIKit/OpenGL ES API and checked here only for syntax and types against
// stand-in headers, so the first build in Xcode is its first real compile.
// See docs/superpowers/specs/2026-10-12-ios-export-design.md.
//
// The shape:
//   main -> UIApplicationMain -> GSSceneDelegate connects a window scene ->
//   a GSView (a CAEAGLLayer with a GLES 3 context) becomes the window ->
//   the application is created (its Window::Create returns an iOSWindow
//   wrapping that view) -> a CADisplayLink runs Application::RunFrame once
//   per refresh, paused while the scene is not active (GL in the
//   background gets an app killed).
//
// Two things differ from every other platform:
//   - There is no default framebuffer. The view's framebuffer is made from
//     its layer and registered with Framebuffer::SetScreenRendererID, so
//     the engine's "unbind to the screen" lands in it.
//   - The frame is presented with presentRenderbuffer, not a swap.
#include "gspch.h"

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>

#include <glad/glad.h>
#include <unistd.h>

#include "GS/Application.h"
#include "GS/Input.h"
#include "GS/Log.h"
#include "GS/Window.h"
#include "GS/Events/ApplicationEvent.h"
#include "GS/Events/MouseEvent.h"
#include "GS/Renderer/Framebuffer.h"

namespace GS {
	// Defined by the game (GSPlayer), as on every platform.
	Application* CreateApplication();
	class iOSWindow;
}

@interface GSView : UIView
@property (nonatomic, readonly) EAGLContext* context;
@property (nonatomic, readonly) GLuint framebuffer;
@property (nonatomic, readonly) GLint pixelWidth;
@property (nonatomic, readonly) GLint pixelHeight;
- (void)present;
@end

namespace GS {

	// The Window the engine sees. The view does the UIKit work and reports
	// size changes and touches here, where they become the engine's events.
	class iOSWindow : public Window
	{
	public:
		explicit iOSWindow(GSView* view) : m_View(view)
		{
			s_Instance = this;
			m_Width = (unsigned int)view.pixelWidth;
			m_Height = (unsigned int)view.pixelHeight;
			GS_CORE_INFO("Window: iOS {0}x{1}, GLES {2}", m_Width, m_Height,
				(const char*)glGetString(GL_VERSION));
		}
		~iOSWindow() override { s_Instance = nullptr; }

		void OnUpdate() override { [m_View present]; }

		unsigned int GetWidth() const override { return m_Width; }
		unsigned int GetHeight() const override { return m_Height; }

		void SetEventCallback(const EventCallbackFn& callback) override { m_Callback = callback; }
		// The display link is the frame clock, so frames are always in step
		// with the display; there is no unsynchronised mode to turn on.
		void SetVSync(bool) override {}
		bool IsVSync() const override { return true; }
		void SetTitle(const std::string&) override {}
		void SetCursorCaptured(bool) override {}
		bool IsCursorCaptured() const override { return false; }
		void* GetNativeWindow() const override { return (__bridge void*)m_View; }

		static iOSWindow* Get() { return s_Instance; }

		void OnResize(unsigned int width, unsigned int height)
		{
			if (width == m_Width && height == m_Height)
				return;
			m_Width = width;
			m_Height = height;
			WindowResizeEvent event(width, height);
			Emit(event);
		}

		// One finger is the pointer: button 0 at its position (in pixels,
		// as the desktop reports the mouse) -- the view admits one touch.
		void OnTouch(bool down, bool up, float x, float y)
		{
			m_PointerX = x;
			m_PointerY = y;
			MouseMovedEvent moved(x, y);
			Emit(moved);
			if (down && !m_PointerDown)
			{
				m_PointerDown = true;
				MouseButtonPressedEvent pressed(0);
				Emit(pressed);
			}
			if (up && m_PointerDown)
			{
				m_PointerDown = false;
				MouseButtonReleasedEvent released(0);
				Emit(released);
			}
		}

		bool PointerDown() const { return m_PointerDown; }
		float PointerX() const { return m_PointerX; }
		float PointerY() const { return m_PointerY; }

	private:
		void Emit(Event& e) { if (m_Callback) m_Callback(e); }

		static iOSWindow* s_Instance;
		GSView* m_View;
		unsigned int m_Width = 0, m_Height = 0;
		EventCallbackFn m_Callback;
		bool m_PointerDown = false;
		float m_PointerX = 0.0f, m_PointerY = 0.0f;
	};

	iOSWindow* iOSWindow::s_Instance = nullptr;

	// Set before the application exists; Window::Create wraps it.
	static GSView* s_View = nil;
	static Application* s_Application = nullptr;

	Window* Window::Create(const WindowProps&)
	{
		return new iOSWindow(s_View);
	}

	std::vector<MonitorInfo> Window::GetMonitors()
	{
		std::vector<MonitorInfo> monitors;
		if (iOSWindow* window = iOSWindow::Get())
		{
			MonitorInfo info;
			info.Width = window->GetWidth();
			info.Height = window->GetHeight();
			info.Primary = true;
			info.Name = "iOS";
			monitors.push_back(info);
		}
		return monitors;
	}

	// No keyboard to poll -- a phone game reads the pointer (Input.h).
	class iOSInput : public Input
	{
	protected:
		bool IsKeyPressedImpl(int) override { return false; }
		bool IsMouseButtonPressedImpl(int button) override
		{
			iOSWindow* window = iOSWindow::Get();
			return button == 0 && window && window->PointerDown();
		}
		std::pair<float, float> GetMousePositionImpl() override
		{
			iOSWindow* window = iOSWindow::Get();
			return window ? std::pair<float, float>{ window->PointerX(), window->PointerY() }
						  : std::pair<float, float>{ 0.0f, 0.0f };
		}
		float GetMouseXImpl() override { return GetMousePositionImpl().first; }
		float GetMouseYImpl() override { return GetMousePositionImpl().second; }
	};

	Input* Input::s_Instance = new iOSInput();
}

@implementation GSView
{
	GLuint _colorBuffer;
	GLuint _depthBuffer;
	UITouch* _touch;
}

+ (Class)layerClass
{
	return [CAEAGLLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame
{
	if ((self = [super initWithFrame:frame]))
	{
		// One touch: the pointer is one finger, and a second would only
		// have to be ignored.
		self.multipleTouchEnabled = NO;

		CAEAGLLayer* layer = (CAEAGLLayer*)self.layer;
		layer.opaque = YES;
		layer.drawableProperties = @{
			kEAGLDrawablePropertyRetainedBacking: @NO,
			kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8
		};

		_context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
		[EAGLContext setCurrentContext:_context];
		glGenFramebuffers(1, &_framebuffer);
		glGenRenderbuffers(1, &_colorBuffer);
		glGenRenderbuffers(1, &_depthBuffer);
	}
	return self;
}

// The layer's storage is sized here, in pixels: points times the screen's
// scale (3 on most current iPhones). Rotation lands here too.
- (void)layoutSubviews
{
	[super layoutSubviews];
	self.contentScaleFactor = self.traitCollection.displayScale;
	[EAGLContext setCurrentContext:_context];

	glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, _colorBuffer);
	[_context renderbufferStorage:GL_RENDERBUFFER fromDrawable:(CAEAGLLayer*)self.layer];
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _colorBuffer);
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &_pixelWidth);
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &_pixelHeight);

	glBindRenderbuffer(GL_RENDERBUFFER, _depthBuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, _pixelWidth, _pixelHeight);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _depthBuffer);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		GS_CORE_ERROR("iOS: the view's framebuffer is incomplete at {0}x{1}", _pixelWidth, _pixelHeight);

	GS::Framebuffer::SetScreenRendererID(_framebuffer);
	if (GS::iOSWindow* window = GS::iOSWindow::Get())
		window->OnResize((unsigned int)_pixelWidth, (unsigned int)_pixelHeight);
}

- (void)present
{
	glBindRenderbuffer(GL_RENDERBUFFER, _colorBuffer);
	[_context presentRenderbuffer:GL_RENDERBUFFER];
	// Back to the screen framebuffer for whatever draws first next frame.
	glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
}

- (void)sendTouch:(UITouch*)touch down:(BOOL)down up:(BOOL)up
{
	CGPoint point = [touch locationInView:self];
	CGFloat scale = self.contentScaleFactor;
	if (GS::iOSWindow* window = GS::iOSWindow::Get())
		window->OnTouch(down, up, (float)(point.x * scale), (float)(point.y * scale));
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
	if (!_touch)
	{
		_touch = touches.anyObject;
		[self sendTouch:_touch down:YES up:NO];
	}
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
	if (_touch && [touches containsObject:_touch])
		[self sendTouch:_touch down:NO up:NO];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
	if (_touch && [touches containsObject:_touch])
	{
		[self sendTouch:_touch down:NO up:YES];
		_touch = nil;
	}
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
	[self touchesEnded:touches withEvent:event];
}

@end

@interface GSViewController : UIViewController
@end

@implementation GSViewController

- (void)loadView
{
	self.view = [[GSView alloc] initWithFrame:CGRectZero];
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }

// Landscape, either way up -- what the Android export does too.
- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
	return UIInterfaceOrientationMaskLandscape;
}

@end

@interface GSSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow* window;
@property (nonatomic, strong) CADisplayLink* displayLink;
@end

@implementation GSSceneDelegate

- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session
	options:(UISceneConnectionOptions*)connectionOptions
{
	UIWindowScene* windowScene = (UIWindowScene*)scene;
	self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
	GSViewController* controller = [[GSViewController alloc] init];
	self.window.rootViewController = controller;
	[self.window makeKeyAndVisible];

	GSView* view = (GSView*)controller.view;
	[view layoutIfNeeded];
	GS::s_View = view;

	// Only once per process. A second scene (iPad multi-window) would need
	// a second application, which the engine's singleton does not allow;
	// the Info.plist turns multiple scenes off.
	if (!GS::s_Application)
	{
		GS::Log::Init();

		// The game was copied into the bundle as a folder; the player
		// starts here and runs game/ beside it, as a desktop package does.
		NSString* resources = [[NSBundle mainBundle] resourcePath];
		chdir(resources.fileSystemRepresentation);

		static char name[] = "GSPlayer";
		static char* argv[] = { name, nullptr };
		GS::Application::SetCommandLine(1, argv);
		GS::s_Application = GS::CreateApplication();
	}

	self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(step:)];
	[self.displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)step:(CADisplayLink*)link
{
	if (!GS::s_Application)
		return;
	[EAGLContext setCurrentContext:GS::s_View.context];
	glBindFramebuffer(GL_FRAMEBUFFER, GS::s_View.framebuffer);
	GS::s_Application->RunFrame();
}

// No GL while not active: iOS terminates an app that issues GL commands in
// the background. The simulation simply waits, as minimised does elsewhere.
- (void)sceneWillResignActive:(UIScene*)scene
{
	self.displayLink.paused = YES;
	glFinish();
}

- (void)sceneDidBecomeActive:(UIScene*)scene
{
	self.displayLink.paused = NO;
}

@end

@interface GSAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation GSAppDelegate
@end

namespace GS::iOS {
	int Main(int argc, char** argv)
	{
		@autoreleasepool
		{
			return UIApplicationMain(argc, argv, nil, NSStringFromClass([GSAppDelegate class]));
		}
	}
}
