#pragma once
// Android: the process is not started by main(). A NativeActivity loads
// libgsplayer.so and native_app_glue calls android_main(android_app*) on a
// thread of its own (EntryPoint.h); everything the engine needs from that --
// the native window, the touch stream, the APK's assets -- hangs off the one
// android_app, which is kept here.
//
// Export piece 6: docs/superpowers/specs/2026-10-12-android-export-design.md.
#include <string>

struct android_app;

namespace GS::Android {

	// What EntryPoint.h's android_main does: wait for a window, unpack the
	// game, run the application, and end the process.
	void Main(android_app* app);

	android_app* GetApp();

	// Handles whatever the activity has queued -- lifecycle commands and
	// input -- and returns. With block, waits for at least one first: how a
	// backgrounded game sleeps until its window comes back.
	void PumpEvents(bool block);

	// Copies the game out of the APK into destination, file by file, as
	// game.files lists it (an APK's asset directory cannot be walked for
	// subfolders, so the packager writes the list). Skipped when the copy
	// already there came from this same build. False with a reason logged
	// if anything could not be written.
	bool ExtractAssets(const std::string& destination);
}
