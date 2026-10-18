#include <SDL.h>
#include <emscripten/html5.h>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

void SexyAppBase::MakeWindow()
{
	if (mWindow)
	{
		// Browsers only grant fullscreen from a user gesture; SDL defers the request until the next one
		SDL_SetWindowFullscreen((SDL_Window*)mWindow, (!mIsWindowed ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	}
	else
	{
		SDL_Init(SDL_INIT_VIDEO);

		// SDL_WINDOW_RESIZABLE makes SDL track the canvas' CSS size, so the page decides how big the game is
		mWindow = (void*)SDL_CreateWindow(
			SexyStringToStringFast(mTitle).c_str(),
			SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			mWidth * IMG_DOWNSCALE, mHeight * IMG_DOWNSCALE,
			SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (!mIsWindowed ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0)
		);

		// The context is created here rather than with SDL_GL_CreateContext because SDL can't request
		// preserveDrawingBuffer, and only dirty widgets are redrawn each frame
		EmscriptenWebGLContextAttributes attribs;
		emscripten_webgl_init_context_attributes(&attribs);
		attribs.alpha = false;
		attribs.depth = false;
		attribs.stencil = false;
		attribs.antialias = false;
		attribs.preserveDrawingBuffer = true;
		attribs.majorVersion = 2;

		EMSCRIPTEN_WEBGL_CONTEXT_HANDLE aContext = emscripten_webgl_create_context("#canvas", &attribs);
		if (aContext <= 0)
		{
			attribs.majorVersion = 1;
			aContext = emscripten_webgl_create_context("#canvas", &attribs);
		}

		if (aContext <= 0)
		{
			Popup("Couldn't create a WebGL context. Your browser or GPU may not support WebGL.");
			DoExit(1);
		}

		emscripten_webgl_make_context_current(aContext);
		mContext = (void*)aContext;
	}

	if (mGLInterface == NULL)
	{
		mGLInterface = new GLInterface(this);
		InitGLInterface();
	}

	bool isActive = mActive;
	mActive = true;

	mPhysMinimized = false;
	if (mMinimized)
	{
		if (mMuteOnLostFocus)
			Unmute(true);

		mMinimized = false;
		isActive = mActive; // set this here so we don't call RehupFocus again.
		RehupFocus();
	}

	if (isActive != mActive)
		RehupFocus();

	ReInitImages();

	mWidgetManager->mImage = mGLInterface->GetScreenImage();
	mWidgetManager->MarkAllDirty();

	mGLInterface->UpdateViewport();
	mWidgetManager->Resize(mScreenBounds, mGLInterface->mPresentationRect);
}
