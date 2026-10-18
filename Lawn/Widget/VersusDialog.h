#ifndef __VERSUSDIALOG_H__
#define __VERSUSDIALOG_H__

#include "LawnDialog.h"
#include "widget/EditListener.h"

// Lobby for Versus mode: enter a friend's room code to join, or leave it empty to create a room.
// Shows the matchmaking status from LawnApp::mVersus until the match starts.
class VersusDialog : public LawnDialog, public EditListener
{
public:
	EditWidget*			mCodeEditWidget;

public:
	VersusDialog(LawnApp* theApp);
	virtual ~VersusDialog();

	virtual int			GetPreferredHeight(int theWidth);
	virtual void		Resize(int theX, int theY, int theWidth, int theHeight);
	virtual void		AddedToManager(WidgetManager* theWidgetManager);
	virtual void		RemovedFromManager(WidgetManager* theWidgetManager);
	virtual void		Update();
	virtual void		Draw(Graphics* g);
	virtual void		EditWidgetText(int theId, const SexyString& theString);
	virtual bool		AllowChar(int theId, SexyChar theChar);
	std::string			GetRoomCode();
};

#endif
