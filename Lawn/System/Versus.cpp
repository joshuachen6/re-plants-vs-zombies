#include "Versus.h"
#include "../Board.h"
#include "../Challenge.h"
#include "../LawnMower.h"
#include "../Plant.h"
#include "../SeedPacket.h"
#include "../Zombie.h"
#include "../Widget/LawnDialog.h"
#include "../Widget/GameButton.h"
#include "../Widget/SeedChooserScreen.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "../../Sexy.TodLib/TodCommon.h"
#include "../../Sexy.TodLib/TodDebug.h"
#include "ReanimationLawn.h"
#include "graphics/Graphics.h"
#include "widget/WidgetManager.h"

#include <algorithm>
#include <cstdlib>

const VersusZombieOffer gVersusZombieOffers[VERSUS_NUM_ZOMBIE_PACKETS] = {
	{ SeedType::SEED_ZOMBIE_NORMAL,			300 },
	{ SeedType::SEED_ZOMBIE_TRAFFIC_CONE,	500 },
	{ SeedType::SEED_ZOMBIE_PAIL,			750 },
	{ SeedType::SEED_ZOMBIE_FOOTBALL,		1500 },
};

// Binary messages, relayed untouched by the server. Multi-byte values are little-endian.
//   zombie   [1][zombie type u8][row u8]
//   snapshot [2][sun u16][income u16][mower bits u8]
//            [plant count u8] { [x i16][y i16][seed type u8] }
//            [zombie count u8] { [id u16][zombie type u8][x i16][y i16][flags u8] }
//   lost     [3]
enum VersusMessage
{
	VERSUS_MSG_ZOMBIE = 1,
	VERSUS_MSG_SNAPSHOT = 2,
	VERSUS_MSG_LOST = 3
};

enum VersusZombieFlags
{
	VERSUS_ZOMBIE_DYING = 1,
	VERSUS_ZOMBIE_CHILLED = 2
};

// The opponent view shows this part of the board (the lawn rows plus a little sky)
static const int OPPONENT_VIEW_SRC_Y = 60;
static const int OPPONENT_VIEW_SRC_WIDTH = BOARD_WIDTH;
static const int OPPONENT_VIEW_SRC_HEIGHT = 520;
static const float OPPONENT_VIEW_SCALE = 0.19f;

namespace
{
	class ByteWriter
	{
	public:
		std::vector<uint8_t> mData;

		void U8(int theValue) { mData.push_back((uint8_t)theValue); }
		void U16(int theValue) { U8(theValue & 0xFF); U8((theValue >> 8) & 0xFF); }
	};

	class ByteReader
	{
	public:
		const std::string& mData;
		size_t mPos = 0;
		bool mOk = true;

		ByteReader(const std::string& theData) : mData(theData) {}

		int U8()
		{
			if (mPos >= mData.size())
			{
				mOk = false;
				return 0;
			}
			return (uint8_t)mData[mPos++];
		}
		int U16() { int aLow = U8(); return aLow | (U8() << 8); }
		int S16() { return (int16_t)U16(); }
	};

	// The relay's lobby messages are flat JSON objects produced by JSON.stringify; this is all the
	// parsing they need
	std::string JsonField(const std::string& theJson, const char* theKey)
	{
		std::string aNeedle = std::string("\"") + theKey + "\":";
		size_t aPos = theJson.find(aNeedle);
		if (aPos == std::string::npos)
			return "";

		aPos += aNeedle.size();
		if (aPos < theJson.size() && theJson[aPos] == '"')
		{
			size_t anEnd = theJson.find('"', aPos + 1);
			return anEnd == std::string::npos ? "" : theJson.substr(aPos + 1, anEnd - aPos - 1);
		}

		size_t anEnd = theJson.find_first_of(",}", aPos);
		return theJson.substr(aPos, anEnd == std::string::npos ? std::string::npos : anEnd - aPos);
	}

	float OpponentViewX(float theBoardX)
	{
		return Versus::GetOpponentViewRect().mX + theBoardX * OPPONENT_VIEW_SCALE;
	}

	float OpponentViewY(float theBoardY)
	{
		return Versus::GetOpponentViewRect().mY + (theBoardY - OPPONENT_VIEW_SRC_Y) * OPPONENT_VIEW_SCALE;
	}
}

Versus::Versus(LawnApp* theApp) : mApp(theApp), mState(VERSUS_STATE_OFF), mResult(VERSUS_RESULT_NONE)
{
	ResetMatch();
}

void Versus::ResetMatch()
{
	mResult = VERSUS_RESULT_NONE;
	mOpponentReady = false;
	mGo = false;
	mZombiePacketsAdded = false;
	mShowingWaitAdvice = false;
	mIncome = VERSUS_STARTING_INCOME;
	mIncomeCounter = 0;
	mSnapshotCounter = 0;
	mHasOpponentSnapshot = false;
	mOpponentSun = 0;
	mOpponentIncome = 0;
	mOpponentMowers = 0;
	mOpponentPlants.clear();
	mOpponentZombies.clear();
}

void Versus::StartMatchmaking(const std::string& theJoinCode)
{
	if (mState != VERSUS_STATE_OFF)
		return;

	std::string aUrl = VersusConnection::GetServerUrl();
	ResetMatch();
	mJoinCode = theJoinCode;
	mRoomCode.clear();

	if (!VersusConnection::IsSupported() || !mConnection.Connect(aUrl))
	{
		mStatus = "Versus needs the web build of the game.";
		return;
	}

	mState = VERSUS_STATE_CONNECTING;
	mStatus = "Connecting to " + aUrl + "...";
}

void Versus::Cancel()
{
	if (mConnection.IsOpen())
		mConnection.SendText("{\"t\":\"leave\"}");
	mConnection.Close();
	mState = VERSUS_STATE_OFF;
	mStatus.clear();
}

void Versus::Update()
{
	VersusConnection::Event anEvent;
	while (mState != VERSUS_STATE_OFF && mConnection.Poll(anEvent))
	{
		switch (anEvent.mType)
		{
		case VersusConnection::EVENT_OPEN:
			if (mJoinCode.empty())
				mConnection.SendText("{\"t\":\"create\"}");
			else
				mConnection.SendText("{\"t\":\"join\",\"code\":\"" + mJoinCode + "\"}");
			mStatus = mJoinCode.empty() ? "Creating a room..." : "Joining room " + mJoinCode + "...";
			break;

		case VersusConnection::EVENT_CLOSE:
			HandleDisconnect();
			break;

		case VersusConnection::EVENT_TEXT:
			HandleControl(anEvent.mData);
			break;

		case VersusConnection::EVENT_BINARY:
			HandleGameData(anEvent.mData);
			break;
		}
	}

	Board* aBoard = mApp->mBoard;
	if (mState != VERSUS_STATE_PLAYING || aBoard == nullptr || !mApp->IsVersusMode() || mApp->mGameScene != GameScenes::SCENE_PLAYING)
		return;

	if (++mIncomeCounter >= VERSUS_INCOME_INTERVAL)
	{
		mIncomeCounter = 0;
		aBoard->AddSunMoney(mIncome);
	}

	if (++mSnapshotCounter >= VERSUS_SNAPSHOT_INTERVAL)
	{
		mSnapshotCounter = 0;
		SendSnapshot(aBoard);
	}

	for (VersusRemoteZombie& aZombie : mOpponentZombies)
	{
		aZombie.mX += (aZombie.mTargetX - aZombie.mX) * 0.2f;
		aZombie.mY += (aZombie.mTargetY - aZombie.mY) * 0.2f;
	}
}

void Versus::HandleControl(const std::string& theMessage)
{
	std::string aType = JsonField(theMessage, "t");

	if (aType == "created")
	{
		mRoomCode = JsonField(theMessage, "code");
		mState = VERSUS_STATE_IN_LOBBY;
		mStatus = "Room code: " + mRoomCode + "\nWaiting for an opponent...";
		TodTrace("Versus: created room %s", mRoomCode.c_str());
	}
	else if (aType == "joined")
	{
		mRoomCode = JsonField(theMessage, "code");
		mState = VERSUS_STATE_IN_LOBBY;
		mStatus = "Joined room " + mRoomCode + ".";
	}
	else if (aType == "opponent_joined")
	{
		mStatus = "An opponent joined!";
	}
	else if (aType == "start")
	{
		mState = VERSUS_STATE_CHOOSING_SEEDS;
		mApp->KillDialog(Dialogs::DIALOG_VERSUS);
		mApp->KillGameSelector();
		mApp->PreNewGame(GameMode::GAMEMODE_MULTIPLAYER_VERSUS, false);
	}
	else if (aType == "opponent_ready")
	{
		mOpponentReady = true;
	}
	else if (aType == "go")
	{
		TodTrace("Versus: match started");
		mGo = true;
		mState = VERSUS_STATE_PLAYING;
	}
	else if (aType == "opponent_left")
	{
		if (mState == VERSUS_STATE_CHOOSING_SEEDS || mState == VERSUS_STATE_PLAYING)
			EndMatch(VERSUS_RESULT_OPPONENT_LEFT);
	}
	else if (aType == "error")
	{
		std::string anError = JsonField(theMessage, "msg");
		if (IsMatchmaking())
		{
			Cancel();
			mStatus = anError + ".";
		}
		else
		{
			TodTrace("Versus server error: %s", anError.c_str());
		}
	}
}

void Versus::HandleDisconnect()
{
	switch (mState)
	{
	case VERSUS_STATE_CONNECTING:
		mConnection.Close();
		mState = VERSUS_STATE_OFF;
		mStatus = "Couldn't reach the Versus server at " + VersusConnection::GetServerUrl() + ".";
		break;

	case VERSUS_STATE_IN_LOBBY:
		mConnection.Close();
		mState = VERSUS_STATE_OFF;
		mStatus = "Lost the connection to the Versus server.";
		break;

	case VERSUS_STATE_CHOOSING_SEEDS:
	case VERSUS_STATE_PLAYING:
		EndMatch(VERSUS_RESULT_DISCONNECTED);
		break;

	default:
		break;
	}
}

void Versus::HandleGameData(const std::string& theData)
{
	if (theData.empty() || mState != VERSUS_STATE_PLAYING)
		return;

	Board* aBoard = mApp->mBoard;
	switch ((uint8_t)theData[0])
	{
	case VERSUS_MSG_ZOMBIE:
	{
		ByteReader aReader(theData);
		aReader.U8();
		ZombieType aZombieType = (ZombieType)aReader.U8();
		int aRow = aReader.U8();
		if (aReader.mOk && aBoard && mApp->mGameScene == GameScenes::SCENE_PLAYING &&
			aZombieType >= 0 && aZombieType < ZombieType::NUM_ZOMBIE_TYPES && aBoard->RowCanHaveZombies(aRow))
		{
			TodTrace("Versus: received zombie %d in row %d", aZombieType, aRow);
			aBoard->AddZombieInRow(aZombieType, aRow, 0);
		}
		break;
	}

	case VERSUS_MSG_SNAPSHOT:
		ReadSnapshot(theData);
		break;

	case VERSUS_MSG_LOST:
		EndMatch(VERSUS_RESULT_WON);
		break;
	}
}

void Versus::SendZombie(SeedType theSeedType, int theRow)
{
	ZombieType aZombieType = Challenge::IZombieSeedTypeToZombieType(theSeedType);
	ByteWriter aWriter;
	aWriter.U8(VERSUS_MSG_ZOMBIE);
	aWriter.U8(aZombieType);
	aWriter.U8(theRow);
	mConnection.SendBinary(aWriter.mData);

	mIncome += Plant::GetCost(theSeedType, SeedType::SEED_NONE) / VERSUS_INCOME_PER_SEND_DIVISOR;
	TodTrace("Versus: sent zombie %d to row %d, income now %d", aZombieType, theRow, mIncome);
}

void Versus::SendSnapshot(Board* theBoard)
{
	ByteWriter aWriter;
	aWriter.U8(VERSUS_MSG_SNAPSHOT);
	aWriter.U16(std::clamp(theBoard->mSunMoney, 0, 0xFFFF));
	aWriter.U16(std::clamp(mIncome, 0, 0xFFFF));

	int aMowers = 0;
	LawnMower* aLawnMower = nullptr;
	while (theBoard->IterateLawnMowers(aLawnMower))
	{
		if (aLawnMower->mMowerState != LawnMowerState::MOWER_TRIGGERED && aLawnMower->mMowerState != LawnMowerState::MOWER_SQUISHED)
			aMowers |= 1 << aLawnMower->mRow;
	}
	aWriter.U8(aMowers);

	std::vector<Plant*> aPlants;
	Plant* aPlant = nullptr;
	while (theBoard->IteratePlants(aPlant) && aPlants.size() < 255)
	{
		aPlants.push_back(aPlant);
	}
	aWriter.U8((int)aPlants.size());
	for (Plant* p : aPlants)
	{
		SeedType aSeedType = p->mSeedType == SeedType::SEED_IMITATER && p->mImitaterType != SeedType::SEED_NONE ? p->mImitaterType : p->mSeedType;
		aWriter.U16(p->mX);
		aWriter.U16(p->mY);
		aWriter.U8(aSeedType);
	}

	std::vector<Zombie*> aZombies;
	Zombie* aZombie = nullptr;
	while (theBoard->IterateZombies(aZombie) && aZombies.size() < 255)
	{
		if (aZombie->mZombieType >= 0 && aZombie->mZombieType < ZombieType::NUM_CACHED_ZOMBIE_TYPES && aZombie->mPosX < BOARD_WIDTH + 40)
			aZombies.push_back(aZombie);
	}
	aWriter.U8((int)aZombies.size());
	for (Zombie* z : aZombies)
	{
		int aFlags = 0;
		if (z->IsDeadOrDying())
			aFlags |= VERSUS_ZOMBIE_DYING;
		if (z->mChilledCounter > 0 || z->mIceTrapCounter > 0)
			aFlags |= VERSUS_ZOMBIE_CHILLED;

		aWriter.U16(theBoard->mZombies.DataArrayGetID(z) & 0xFFFF);
		aWriter.U8(z->mZombieType);
		aWriter.U16((int)z->mPosX);
		aWriter.U16((int)z->mPosY);
		aWriter.U8(aFlags);
	}

	mConnection.SendBinary(aWriter.mData);
}

void Versus::ReadSnapshot(const std::string& theData)
{
	ByteReader aReader(theData);
	aReader.U8();
	int aSun = aReader.U16();
	int anIncome = aReader.U16();
	int aMowers = aReader.U8();

	std::vector<VersusRemotePlant> aPlants(aReader.U8());
	for (VersusRemotePlant& aPlant : aPlants)
	{
		aPlant.mX = aReader.S16();
		aPlant.mY = aReader.S16();
		aPlant.mSeedType = (SeedType)aReader.U8();
	}

	std::vector<VersusRemoteZombie> aZombies(aReader.U8());
	for (VersusRemoteZombie& aZombie : aZombies)
	{
		aZombie.mId = aReader.U16();
		aZombie.mZombieType = (ZombieType)aReader.U8();
		aZombie.mTargetX = aReader.S16();
		aZombie.mTargetY = aReader.S16();
		aZombie.mFlags = aReader.U8();
		aZombie.mX = aZombie.mTargetX;
		aZombie.mY = aZombie.mTargetY;

		// Keep easing from where an already-known zombie is drawn
		for (const VersusRemoteZombie& anOld : mOpponentZombies)
		{
			if (anOld.mId == aZombie.mId)
			{
				aZombie.mX = anOld.mX;
				aZombie.mY = anOld.mY;
				break;
			}
		}
	}

	if (!aReader.mOk)
		return;

	for (const VersusRemotePlant& aPlant : aPlants)
		if (aPlant.mSeedType < 0 || aPlant.mSeedType >= SeedType::NUM_SEED_TYPES)
			return;
	for (const VersusRemoteZombie& aZombie : aZombies)
		if (aZombie.mZombieType < 0 || aZombie.mZombieType >= ZombieType::NUM_CACHED_ZOMBIE_TYPES)
			return;

	mHasOpponentSnapshot = true;
	mOpponentSun = aSun;
	mOpponentIncome = anIncome;
	mOpponentMowers = (uint8_t)aMowers;
	mOpponentPlants.swap(aPlants);
	mOpponentZombies.swap(aZombies);
}

int Versus::GetNumSeedsInBank() const
{
	return VERSUS_NUM_PLANT_PACKETS + (mZombiePacketsAdded ? VERSUS_NUM_ZOMBIE_PACKETS : 0);
}

void Versus::OnSeedsChosen(Board* theBoard)
{
	mZombiePacketsAdded = true;
	theBoard->mSeedBank->UpdateWidth();
	for (int i = 0; i < VERSUS_NUM_ZOMBIE_PACKETS; i++)
	{
		theBoard->mSeedBank->mSeedPackets[VERSUS_NUM_PLANT_PACKETS + i].SetPacketType(gVersusZombieOffers[i].mSeedType, SeedType::SEED_NONE);
	}

	// The seed chooser's Menu button lingers until the level starts, on top of the opponent view
	if (mApp->mSeedChooserScreen)
	{
		mApp->mSeedChooserScreen->mMenuButton->mBtnNoDraw = true;
		mApp->mSeedChooserScreen->mMenuButton->mDisabled = true;
	}

	mConnection.SendText("{\"t\":\"ready\"}");
}

void Versus::OnZombiesWon()
{
	if (mState == VERSUS_STATE_OVER)
		return;

	ByteWriter aWriter;
	aWriter.U8(VERSUS_MSG_LOST);
	mConnection.SendBinary(aWriter.mData);

	// The board plays its "zombies ate your brains" cutscene first, then calls ShowResult()
	mState = VERSUS_STATE_OVER;
	mResult = VERSUS_RESULT_LOST;
}

void Versus::EndMatch(VersusResult theResult)
{
	if (mState == VERSUS_STATE_OVER)
		return;

	mState = VERSUS_STATE_OVER;
	mResult = theResult;
	ShowResult();
}

void Versus::ShowResult()
{
	TodTrace("Versus: result %d", mResult);
	const char* aHeader = "MATCH OVER";
	const char* aBody = "";
	switch (mResult)
	{
	case VERSUS_RESULT_WON:				aHeader = "YOU WIN!";	aBody = "The zombies got through\nyour opponent's lawn!";		break;
	case VERSUS_RESULT_LOST:			aHeader = "YOU LOSE";	aBody = "The zombies ate your brains.\nYour opponent wins.";	break;
	case VERSUS_RESULT_OPPONENT_LEFT:	aHeader = "YOU WIN!";	aBody = "Your opponent left the match.";						break;
	case VERSUS_RESULT_DISCONNECTED:	aHeader = "DISCONNECTED";	aBody = "Lost the connection to\nthe Versus server.";		break;
	default:																											break;
	}

	mApp->KillDialog(Dialogs::DIALOG_VERSUS_RESULT);
	mApp->DoDialog(Dialogs::DIALOG_VERSUS_RESULT, true, StringToSexyString(aHeader), StringToSexyString(aBody), __S("Main Menu"), Dialog::BUTTONS_FOOTER);
	if (mApp->mBoard)
		mApp->mBoard->Pause(true);
}

int Versus::GetZombieRefreshTime(SeedType theSeedType)
{
	for (const VersusZombieOffer& anOffer : gVersusZombieOffers)
	{
		if (anOffer.mSeedType == theSeedType)
			return anOffer.mRefreshTime;
	}
	return 0;
}

Rect Versus::GetOpponentViewRect()
{
	int aWidth = (int)(OPPONENT_VIEW_SRC_WIDTH * OPPONENT_VIEW_SCALE);
	int aHeight = (int)(OPPONENT_VIEW_SRC_HEIGHT * OPPONENT_VIEW_SCALE);
	return Rect(BOARD_WIDTH - aWidth - 4, 4, aWidth, aHeight);
}

int Versus::OpponentViewRowAt(int theX, int theY)
{
	Rect aRect = GetOpponentViewRect();
	if (!aRect.Contains(theX, theY))
		return -1;

	float aBoardY = OPPONENT_VIEW_SRC_Y + (theY - aRect.mY) / OPPONENT_VIEW_SCALE;
	int aRow = (int)((aBoardY - LAWN_YMIN) / 100.0f);
	return std::clamp(aRow, 0, 4);
}

void Versus::DrawOpponentView(Graphics* g)
{
	Rect aRect = GetOpponentViewRect();

	g->SetColor(Color(0, 0, 0, 200));
	g->FillRect(aRect.mX - 2, aRect.mY - 2, aRect.mWidth + 4, aRect.mHeight + 4);

	Graphics aView(*g);
	aView.ClipRect(aRect);
	aView.DrawImage(IMAGE_BACKGROUND1, aRect, Rect(BOARD_OFFSET, OPPONENT_VIEW_SRC_Y, OPPONENT_VIEW_SRC_WIDTH, OPPONENT_VIEW_SRC_HEIGHT));

	aView.mScaleX = OPPONENT_VIEW_SCALE;
	aView.mScaleY = OPPONENT_VIEW_SCALE;
	for (int aRow = 0; aRow < 5; aRow++)
	{
		if (mOpponentMowers & (1 << aRow))
		{
			// DrawCachedMower shifts left by 20 unscaled pixels
			mApp->mReanimatorCache->DrawCachedMower(&aView, OpponentViewX(0) + 20, OpponentViewY(LAWN_YMIN + aRow * 100 + 23), LawnMowerType::LAWNMOWER_LAWN);
		}
	}
	for (const VersusRemotePlant& aPlant : mOpponentPlants)
	{
		mApp->mReanimatorCache->DrawCachedPlant(&aView, OpponentViewX(aPlant.mX), OpponentViewY(aPlant.mY), aPlant.mSeedType, DrawVariation::VARIATION_NORMAL);
	}
	for (const VersusRemoteZombie& aZombie : mOpponentZombies)
	{
		if (aZombie.mFlags & VERSUS_ZOMBIE_CHILLED)
		{
			aView.SetColorizeImages(true);
			aView.SetColor(Color(75, 75, 255));
		}
		else if (aZombie.mFlags & VERSUS_ZOMBIE_DYING)
		{
			aView.SetColorizeImages(true);
			aView.SetColor(Color(255, 255, 255, 128));
		}
		mApp->mReanimatorCache->DrawCachedZombie(&aView, OpponentViewX(aZombie.mX - 40), OpponentViewY(aZombie.mY - 40), aZombie.mZombieType);
		aView.SetColorizeImages(false);
	}

	// Highlight the lane a held zombie packet would go to
	Board* aBoard = mApp->mBoard;
	if (aBoard && Challenge::IsZombieSeedType(aBoard->GetSeedTypeInCursor()))
	{
		int aRow = OpponentViewRowAt(mApp->mWidgetManager->mLastMouseX, mApp->mWidgetManager->mLastMouseY);
		if (aRow >= 0)
		{
			g->SetColor(Color(255, 255, 255, 70));
			g->FillRect(aRect.mX, (int)OpponentViewY(LAWN_YMIN + aRow * 100), aRect.mWidth, (int)(100 * OPPONENT_VIEW_SCALE));
		}
	}

	SexyString aCaption = mHasOpponentSnapshot ? StrFormat(__S("Opponent: %d (+%d)"), mOpponentSun, mOpponentIncome) : __S("Opponent");
	TodDrawString(g, aCaption, aRect.mX + aRect.mWidth / 2, aRect.mY + aRect.mHeight + 12, FONT_BRIANNETOD12, Color(255, 255, 255), DrawStringJustification::DS_ALIGN_CENTER);
}

void Versus::DrawIncome(Graphics* g)
{
	TodDrawString(g, StrFormat(__S("+%d"), mIncome), 34, 100, FONT_CONTINUUMBOLD14, Color(255, 255, 160), DrawStringJustification::DS_ALIGN_CENTER);
}
