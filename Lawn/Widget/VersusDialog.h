#ifndef __VERSUSDIALOG_H__
#define __VERSUSDIALOG_H__

#include "LawnDialog.h"
#include "widget/EditListener.h"
#include "../System/Versus.h"

// Lobby for Versus mode: pick a map and leave the code empty to create a room, or enter a friend's
// room code to join theirs. Shows the matchmaking status from LawnApp::mVersus until the match starts.
class VersusDialog : public LawnDialog, public EditListener
{
public:
	enum
	{
		VersusDialog_PrevMap = 100,
		VersusDialog_NextMap = 101
	};

	EditWidget*			mCodeEditWidget;
	LawnStoneButton*	mPrevMapButton;
	LawnStoneButton*	mNextMapButton;
	VersusMap			mMap;

public:
	VersusDialog(LawnApp* theApp);
	virtual ~VersusDialog();

	virtual int			GetPreferredHeight(int theWidth);
	virtual void		Resize(int theX, int theY, int theWidth, int theHeight);
	virtual void		AddedToManager(WidgetManager* theWidgetManager);
	virtual void		RemovedFromManager(WidgetManager* theWidgetManager);
	virtual void		Update();
	virtual void		Draw(Graphics* g);
	virtual void		ButtonDepress(int theId);
	virtual void		EditWidgetText(int theId, const SexyString& theString);
	virtual bool		AllowChar(int theId, SexyChar theChar);
	std::string			GetRoomCode();

private:
	Rect				GetThumbnailRect();
	bool				IsJoining();
};

#endif
