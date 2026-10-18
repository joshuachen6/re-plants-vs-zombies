#include "ZombieChooserScreen.h"
#include "GameButton.h"
#include "../Board.h"
#include "../Plant.h"
#include "../SeedPacket.h"
#include "../Zombie.h"
#include "../Challenge.h"
#include "../System/Versus.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "../../Sexy.TodLib/TodCommon.h"
#include "../../Sexy.TodLib/TodStringFile.h"
#include "graphics/Graphics.h"
#include "widget/WidgetManager.h"

#include <algorithm>

static const Rect INFO_RECT(22, 290, 414, 230);

ZombieChooserScreen::ZombieChooserScreen(LawnApp* theApp)
{
	mApp = theApp;
	mMouseVisible = true;
	Resize(0, 0, BOARD_WIDTH, BOARD_HEIGHT);

	mStartButton = new GameButton(0);
	mStartButton->SetLabel(__S("[LETS_ROCK_BUTTON]"));
	mStartButton->mButtonImage = Sexy::IMAGE_SEEDCHOOSER_BUTTON;
	mStartButton->mOverImage = nullptr;
	mStartButton->mDownImage = nullptr;
	mStartButton->mDisabledImage = Sexy::IMAGE_SEEDCHOOSER_BUTTON_DISABLED;
	mStartButton->mOverOverlayImage = Sexy::IMAGE_SEEDCHOOSER_BUTTON_GLOW;
	mStartButton->SetFont(Sexy::FONT_DWARVENTODCRAFT18YELLOW);
	mStartButton->Resize(154, 545, 156, 42);
	mStartButton->mTextOffsetY = -1;
	mStartButton->mParentWidget = this;
}

ZombieChooserScreen::~ZombieChooserScreen()
{
	delete mStartButton;
}

void ZombieChooserScreen::GetOfferPosition(int theIndex, int& x, int& y)
{
	// Same grid as the seed chooser
	x = (theIndex % 8) * 53 + 22;
	y = (theIndex / 8) * 70 + 123;
}

int ZombieChooserScreen::OfferHitTest(int x, int y)
{
	for (int i = 0; i < VERSUS_NUM_ZOMBIE_OFFERS; i++)
	{
		int aPosX, aPosY;
		GetOfferPosition(i, aPosX, aPosY);
		if (Rect(aPosX, aPosY, SEED_PACKET_WIDTH, SEED_PACKET_HEIGHT).Contains(x, y))
			return i;
	}
	return -1;
}

int ZombieChooserScreen::BankHitTest(int x, int y)
{
	Board* aBoard = mApp->mBoard;
	if (aBoard == nullptr)
		return -1;

	SeedBank* aSeedBank = aBoard->mSeedBank;
	for (int i = 0; i < (int)mChosen.size(); i++)
	{
		SeedPacket& aPacket = aSeedBank->mSeedPackets[VERSUS_NUM_PLANT_PACKETS + i];
		if (Rect(aSeedBank->mX + aPacket.mX, aSeedBank->mY + aPacket.mY, SEED_PACKET_WIDTH, SEED_PACKET_HEIGHT).Contains(x, y))
			return i;
	}
	return -1;
}

bool ZombieChooserScreen::IsChosen(SeedType theSeedType)
{
	return std::find(mChosen.begin(), mChosen.end(), theSeedType) != mChosen.end();
}

void ZombieChooserScreen::SyncSeedBank()
{
	Board* aBoard = mApp->mBoard;
	if (aBoard == nullptr)
		return;

	for (int i = 0; i < VERSUS_NUM_ZOMBIE_PACKETS; i++)
	{
		SeedPacket& aPacket = aBoard->mSeedBank->mSeedPackets[VERSUS_NUM_PLANT_PACKETS + i];
		if (i < (int)mChosen.size())
			aPacket.SetPacketType(mChosen[i], SeedType::SEED_NONE);
		else
			aPacket.mPacketType = SeedType::SEED_NONE;
	}
}

void ZombieChooserScreen::Update()
{
	Widget::Update();

	bool aFull = mChosen.size() == VERSUS_NUM_ZOMBIE_PACKETS;
	mStartButton->SetDisabled(!aFull);
	mStartButton->mColors[GameButton::COLOR_LABEL] = aFull ? Color::White : Color(64, 64, 64);
	mStartButton->Update();

	int aMouseX = mApp->mWidgetManager->mLastMouseX - mX, aMouseY = mApp->mWidgetManager->mLastMouseY - mY;
	bool aOverPacket = OfferHitTest(aMouseX, aMouseY) >= 0 || BankHitTest(aMouseX, aMouseY) >= 0;
	mApp->SetCursor(aOverPacket || mStartButton->IsMouseOver() ? CURSOR_HAND : CURSOR_POINTER);

	MarkDirty();
}

void ZombieChooserScreen::Draw(Graphics* g)
{
	g->SetLinearBlend(true);
	g->DrawImage(Sexy::IMAGE_SEEDCHOOSER_BACKGROUND, 0, 87);
	TodDrawString(g, __S("CHOOSE YOUR ZOMBIES!"), 229, 110, Sexy::FONT_DWARVENTODCRAFT18YELLOW, Color::White, DS_ALIGN_CENTER);

	for (int i = 0; i < VERSUS_NUM_ZOMBIE_OFFERS; i++)
	{
		int x, y;
		GetOfferPosition(i, x, y);
		SeedType aSeedType = gVersusZombieOffers[i].mSeedType;
		DrawSeedPacket(g, x, y, aSeedType, SeedType::SEED_NONE, 0, IsChosen(aSeedType) ? 55 : 255, true, false);
	}

	g->SetColor(Color(0, 0, 0, 160));
	g->FillRect(INFO_RECT);

	int aMouseX = mApp->mWidgetManager->mLastMouseX - mX, aMouseY = mApp->mWidgetManager->mLastMouseY - mY;
	int anOffer = OfferHitTest(aMouseX, aMouseY);
	SexyString anInfo;
	if (anOffer >= 0)
	{
		const VersusZombieOffer& aOffer = gVersusZombieOffers[anOffer];
		ZombieType aZombieType = Challenge::IZombieSeedTypeToZombieType(aOffer.mSeedType);
		int aCost = Plant::GetCost(aOffer.mSeedType, SeedType::SEED_NONE);
		anInfo = StrFormat(__S("%s\n\nCosts %d sun\nRaises your income by %d\nCooldown %d seconds"),
			TodStringTranslate(StrFormat(__S("[%s]"), GetZombieDefinition(aZombieType).mZombieName)).c_str(),
			aCost, aCost / VERSUS_INCOME_PER_SEND_DIVISOR, aOffer.mRefreshTime / 100);
	}
	else
	{
		anInfo = StrFormat(__S("Pick %d zombies to send at your opponent.\nSending a zombie also raises your income.\n\nClick a zombie in your seed bank to put it back."), VERSUS_NUM_ZOMBIE_PACKETS);
	}
	Rect aTextRect(INFO_RECT.mX + 12, INFO_RECT.mY + 12, INFO_RECT.mWidth - 24, INFO_RECT.mHeight - 60);
	TodDrawStringWrapped(g, anInfo, aTextRect, Sexy::FONT_BRIANNETOD16, Color(255, 255, 255), DS_ALIGN_CENTER);

	const SexyChar* aOpponentStatus = mApp->mVersus->mOpponentReady ? __S("Your opponent is ready!") : __S("Your opponent is still choosing...");
	TodDrawString(g, aOpponentStatus, INFO_RECT.mX + INFO_RECT.mWidth / 2, INFO_RECT.mY + INFO_RECT.mHeight - 18, Sexy::FONT_BRIANNETOD16,
		mApp->mVersus->mOpponentReady ? Color(120, 255, 120) : Color(224, 187, 98), DS_ALIGN_CENTER);

	mStartButton->Draw(g);
}

void ZombieChooserScreen::MouseDown(int x, int y, int theClickCount)
{
	(void)theClickCount;

	if (mStartButton->IsMouseOver())
	{
		mApp->PlaySample(Sexy::SOUND_TAP);
		mApp->mVersus->OnZombiesChosen(mChosen);  // removes this screen
		return;
	}

	int anOffer = OfferHitTest(x, y);
	if (anOffer >= 0)
	{
		SeedType aSeedType = gVersusZombieOffers[anOffer].mSeedType;
		if (!IsChosen(aSeedType) && mChosen.size() < VERSUS_NUM_ZOMBIE_PACKETS)
		{
			mChosen.push_back(aSeedType);
			SyncSeedBank();
			mApp->PlaySample(Sexy::SOUND_SEEDLIFT);
		}
		return;
	}

	int aBankIndex = BankHitTest(x, y);
	if (aBankIndex >= 0)
	{
		mChosen.erase(mChosen.begin() + aBankIndex);
		SyncSeedBank();
		mApp->PlaySample(Sexy::SOUND_TAP);
	}
}
