#include "VersusDialog.h"
#include "GameButton.h"
#include "../System/Versus.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "../../Sexy.TodLib/TodCommon.h"
#include "../../Sexy.TodLib/TodStringFile.h"
#include "widget/EditWidget.h"
#include "widget/WidgetManager.h"

static const int ROOM_CODE_LENGTH = 4;
static const int STATUS_HEIGHT = 50;
static const int MAP_HEIGHT = 118;							// map thumbnail and its name
static const int MAP_THUMBNAIL_HEIGHT = 80;
static const int ARROW_SIZE = 46;
static const int EXTRA_HEIGHT = 40 + STATUS_HEIGHT + 40 + MAP_HEIGHT;	// edit box, status, the second line of text, map
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
	// The map thumbnails are the survival ones from the mini-games screen
	mLoadedResourceNames.push_back("DelayLoad_ChallengeScreen");
	TodLoadResources("DelayLoad_ChallengeScreen");

	mMap = theApp->mVersus->mMap;  // the last map picked
	mVerticalCenterText = false;
	mLawnYesButton->mLabel = __S("Play");
	mCodeEditWidget = CreateEditWidget(0, this, this);
	mCodeEditWidget->mMaxChars = ROOM_CODE_LENGTH;
	mPrevMapButton = MakeButton(VersusDialog_PrevMap, this, __S("<"));
	mNextMapButton = MakeButton(VersusDialog_NextMap, this, __S(">"));
	CalcSize(110, EXTRA_HEIGHT);
}

VersusDialog::~VersusDialog()
{
	delete mCodeEditWidget;
	delete mPrevMapButton;
	delete mNextMapButton;
}

void VersusDialog::AddedToManager(WidgetManager* theWidgetManager)
{
	LawnDialog::AddedToManager(theWidgetManager);
	AddWidget(mCodeEditWidget);
	AddWidget(mPrevMapButton);
	AddWidget(mNextMapButton);
	theWidgetManager->SetFocus(mCodeEditWidget);
}

void VersusDialog::RemovedFromManager(WidgetManager* theWidgetManager)
{
	LawnDialog::RemovedFromManager(theWidgetManager);
	RemoveWidget(mCodeEditWidget);
	RemoveWidget(mPrevMapButton);
	RemoveWidget(mNextMapButton);
}

int VersusDialog::GetPreferredHeight(int theWidth)
{
	return LawnDialog::GetPreferredHeight(theWidth) + EXTRA_HEIGHT;
}

Rect VersusDialog::GetThumbnailRect()
{
	Image* aThumbnails = Sexy::IMAGE_SURVIVAL_THUMBNAILS;
	int aHeight = MAP_THUMBNAIL_HEIGHT;
	int aWidth = aThumbnails ? aThumbnails->GetCelWidth() * aHeight / std::max(aThumbnails->GetCelHeight(), 1) : aHeight * 4 / 3;
	int aTop = mHeight - BUTTONS_TOP - STATUS_HEIGHT - 40 - MAP_HEIGHT;
	return Rect((mWidth - aWidth) / 2, aTop, aWidth, aHeight);
}

void VersusDialog::Resize(int theX, int theY, int theWidth, int theHeight)
{
	LawnDialog::Resize(theX, theY, theWidth, theHeight);
	mCodeEditWidget->Resize(mContentInsets.mLeft + 12, mHeight - BUTTONS_TOP - STATUS_HEIGHT - 40, mWidth - mContentInsets.mLeft - mContentInsets.mRight - 24, 28);

	Rect aThumbnail = GetThumbnailRect();
	int anArrowY = aThumbnail.mY + (aThumbnail.mHeight - ARROW_SIZE) / 2;
	mPrevMapButton->Resize(aThumbnail.mX - ARROW_SIZE - 14, anArrowY, ARROW_SIZE, ARROW_SIZE);
	mNextMapButton->Resize(aThumbnail.mX + aThumbnail.mWidth + 14, anArrowY, ARROW_SIZE, ARROW_SIZE);
}

bool VersusDialog::IsJoining()
{
	return !mCodeEditWidget->mString.empty();
}

void VersusDialog::Update()
{
	LawnDialog::Update();

	// While connecting or waiting for an opponent, Cancel stays available to leave the room
	bool aBusy = mApp->mVersus->IsMatchmaking();
	mLawnYesButton->SetDisabled(aBusy);
	mCodeEditWidget->mVisible = !aBusy;

	// Joining a room plays on the map its creator picked
	bool aCanPickMap = !aBusy && !IsJoining();
	mPrevMapButton->mVisible = aCanPickMap;
	mNextMapButton->mVisible = aCanPickMap;
	MarkDirty();
}

void VersusDialog::ButtonDepress(int theId)
{
	if (theId == VersusDialog_PrevMap || theId == VersusDialog_NextMap)
	{
		int aStep = theId == VersusDialog_NextMap ? 1 : NUM_VERSUS_MAPS - 1;
		mMap = (VersusMap)((mMap + aStep) % NUM_VERSUS_MAPS);
		return;
	}

	LawnDialog::ButtonDepress(theId);
}

void VersusDialog::Draw(Graphics* g)
{
	LawnDialog::Draw(g);

	bool aBusy = mApp->mVersus->IsMatchmaking();
	VersusMap aShownMap = aBusy ? mApp->mVersus->mMap : mMap;
	bool aDimmed = !aBusy && IsJoining();
	Rect aThumbnail = GetThumbnailRect();
	if (Sexy::IMAGE_SURVIVAL_THUMBNAILS)
	{
		if (aDimmed)
		{
			g->SetColorizeImages(true);
			g->SetColor(Color(110, 110, 110));
		}
		g->DrawImageCel(Sexy::IMAGE_SURVIVAL_THUMBNAILS, aThumbnail, (int)aShownMap);
		g->SetColorizeImages(false);
	}
	SexyString aMapLabel = aDimmed ? __S("Map: picked by the room's creator") : StrFormat(__S("Map: %s"), gVersusMaps[aShownMap].mName);
	TodDrawString(g, aMapLabel, mWidth / 2, aThumbnail.mY + aThumbnail.mHeight + 24, FONT_BRIANNETOD16, Color(224, 187, 98), DrawStringJustification::DS_ALIGN_CENTER);

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
