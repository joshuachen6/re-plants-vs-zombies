#include <emscripten.h>

#include "SexyAppBase.h"

using namespace Sexy;

// Called by web/pre.js when the page is hidden or closed. A browser tab never gets to shut the game
// down, which is when it would normally save its settings and the current profile.
extern "C" EMSCRIPTEN_KEEPALIVE void SexyWebSaveState()
{
	if ((gSexyAppBase != NULL) && (gSexyAppBase->mRunning) && (!gSexyAppBase->mShutdown))
		gSexyAppBase->WriteToRegistry();
}
