#include "VersusDialog.h"
#include "GameButton.h"
#include "../System/Versus.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "../../Sexy.TodLib/TodStringFile.h"
#include "widget/EditWidget.h"
#include "widget/WidgetManager.h"

static const int ROOM_CODE_LENGTH = 4;
static const int STATUS_HEIGHT = 50;
static const int EXTRA_HEIGHT = 40 + STATUS_HEIGHT + 40;	// edit box, status, and the second line of text
static const int BUTTONS_TOP = 115;							// distance of the buttons from the bottom

VersusDialog::VersusDialog(LawnApp* theApp) : LawnDialog(
	theApp,
	Dialogs::DIALOG_VERSUS,
	true,
	__S("VERSUS"),
	__S("Enter a friend's room code to join,\nor leave it empty to make a room."),
	__S("[DIALOG_BUTTON_OK]"),
	Dialog::BUTTONS_OK_CANCEL)
{
	mVerticalCenterText = false;
	mLawnYesButton->mLabel = __S("Play");
	mCodeEditWidget = CreateEditWidget(0, this, this);
	mCodeEditWidget->mMaxChars = ROOM_CODE_LENGTH;
	CalcSize(110, EXTRA_HEIGHT);
}

VersusDialog::~VersusDialog()
{
	delete mCodeEditWidget;
}

void VersusDialog::AddedToManager(WidgetManager* theWidgetManager)
{
	LawnDialog::AddedToManager(theWidgetManager);
	AddWidget(mCodeEditWidget);
	theWidgetManager->SetFocus(mCodeEditWidget);
}

void VersusDialog::RemovedFromManager(WidgetManager* theWidgetManager)
{
	LawnDialog::RemovedFromManager(theWidgetManager);
	RemoveWidget(mCodeEditWidget);
}

int VersusDialog::GetPreferredHeight(int theWidth)
{
	return LawnDialog::GetPreferredHeight(theWidth) + EXTRA_HEIGHT;
}

void VersusDialog::Resize(int theX, int theY, int theWidth, int theHeight)
{
	LawnDialog::Resize(theX, theY, theWidth, theHeight);
	mCodeEditWidget->Resize(mContentInsets.mLeft + 12, mHeight - BUTTONS_TOP - STATUS_HEIGHT - 40, mWidth - mContentInsets.mLeft - mContentInsets.mRight - 24, 28);
}

void VersusDialog::Update()
{
	LawnDialog::Update();

	// While connecting or waiting for an opponent, Cancel stays available to leave the room
	bool aBusy = mApp->mVersus->IsMatchmaking();
	mLawnYesButton->SetDisabled(aBusy);
	mCodeEditWidget->mVisible = !aBusy;
	MarkDirty();
}

void VersusDialog::Draw(Graphics* g)
{
	LawnDialog::Draw(g);
	if (mCodeEditWidget->mVisible)
		DrawEditBox(g, mCodeEditWidget);

	const std::string& aStatus = mApp->mVersus->mStatus;
	if (!aStatus.empty())
	{
		Rect aStatusRect(mContentInsets.mLeft + 12, mHeight - BUTTONS_TOP - STATUS_HEIGHT, mWidth - mContentInsets.mLeft - mContentInsets.mRight - 24, STATUS_HEIGHT);
		if (!mCodeEditWidget->mVisible)
			aStatusRect.mY -= 20;
		TodDrawStringWrapped(g, StringToSexyString(aStatus), aStatusRect, FONT_BRIANNETOD16, Color(224, 187, 98), DrawStringJustification::DS_ALIGN_CENTER);
	}
}

void VersusDialog::EditWidgetText(int, const SexyString&)
{
	if (!mApp->mVersus->IsMatchmaking())
		mApp->ButtonDepress(mId + 2000);
}

bool VersusDialog::AllowChar(int, SexyChar theChar)
{
	return isalpha((int)theChar) != 0;
}

std::string VersusDialog::GetRoomCode()
{
	std::string aCode;
	for (SexyChar aChar : mCodeEditWidget->mString)
		aCode += (char)toupper((int)aChar);
	return aCode;
}
