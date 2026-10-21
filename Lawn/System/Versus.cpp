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
#include "../Widget/ZombieChooserScreen.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "../../Sexy.TodLib/TodCommon.h"
#include "../../Sexy.TodLib/TodDebug.h"
#include "ReanimationLawn.h"
#include "graphics/Graphics.h"
#include "graphics/Font.h"
#include "widget/WidgetManager.h"

#include <SDL.h>
#include <algorithm>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Read by GLInterface::Flush: during a match the game keeps running while its page is hidden, so the
// opponent isn't left playing against a frozen board and the connection keeps being serviced
EM_JS(void, SetKeepRunningHiddenJS, (int theKeepRunning), {
	Module.pvzKeepRunningHidden = !!theKeepRunning;
});
#else
static void SetKeepRunningHiddenJS(int) {}
#endif

enum VersusConnectionDialog
{
	VERSUS_CONNECTION_DIALOG_NONE,
	VERSUS_CONNECTION_DIALOG_RECONNECTING,
	VERSUS_CONNECTION_DIALOG_OPPONENT_AWAY
};

// Every zombie that can be sent, in the order the zombie chooser shows them. Prices are roughly the
// sun worth of plants it takes to stop each zombie, so sending isn't cheaper than defending.
const VersusZombieOffer gVersusZombieOffers[VERSUS_NUM_ZOMBIE_OFFERS] = {
	// Like the bloons you can send in Bloons TD Battles, tougher zombies unlock as the natural waves
	// go by, so nobody can rush a Football Zombie before the other player has any defences. Nothing
	// can be sent before the first natural wave, which gives both players time to set up.
	//  zombie							cost	cooldown	unlock wave
	{ SeedType::SEED_ZOMBIE_NORMAL,			100,	400,		1 },
	{ SeedType::SEED_ZOMBIE_TRAFFIC_CONE,	175,	600,		2 },
	{ SeedType::SEED_ZOMBIE_POLEVAULTER,	175,	600,		3 },
	{ SeedType::SEED_ZOMBIE_PAIL,			300,	900,		4 },
	{ SeedType::SEED_ZOMBIE_SCREEN_DOOR,	275,	900,		4 },
	{ SeedType::SEED_ZOMBIE_LADDER,			300,	1000,		5 },
	{ SeedType::SEED_ZOMBIE_BALLOON,		300,	1000,		5 },
	{ SeedType::SEED_ZOMBIE_IMP,			75,		400,		1 },
	{ SeedType::SEED_ZOMBIE_DIGGER,			300,	1200,		6 },
	{ SeedType::SEED_ZOMBIE_BUNGEE,			250,	1200,		6 },
	{ SeedType::SEED_ZOMBIE_POGO,			350,	1200,		7 },
	{ SeedType::SEED_ZOMBIE_FOOTBALL,		450,	1800,		7 },
	{ SeedType::SEED_ZOMBONI,				450,	1800,		8 },
	{ SeedType::SEED_ZOMBIE_DANCER,			600,	2500,		9 },
	{ SeedType::SEED_ZOMBIE_GARGANTUAR,		750,	3500,		11 },
};

const VersusMapInfo gVersusMaps[NUM_VERSUS_MAPS] = {
	{ "Day",	BackgroundType::BACKGROUND_1_DAY },
	{ "Night",	BackgroundType::BACKGROUND_2_NIGHT },
	{ "Pool",	BackgroundType::BACKGROUND_3_POOL },
	{ "Fog",	BackgroundType::BACKGROUND_4_FOG },
	{ "Roof",	BackgroundType::BACKGROUND_5_ROOF },
};

// Zombies the natural waves pick from: point value, first wave it can appear in, and pick weight
struct VersusNaturalZombie
{
	ZombieType				mZombieType;
	int						mValue;
	int						mFirstWave;
	int						mWeight;
};

static const VersusNaturalZombie gVersusNaturalZombies[] = {
	{ ZombieType::ZOMBIE_NORMAL,		1,	1,	4000 },
	{ ZombieType::ZOMBIE_TRAFFIC_CONE,	2,	3,	4000 },
	{ ZombieType::ZOMBIE_POLEVAULTER,	2,	6,	2000 },
	{ ZombieType::ZOMBIE_NEWSPAPER,		2,	7,	1000 },
	{ ZombieType::ZOMBIE_PAIL,			4,	9,	3000 },
	{ ZombieType::ZOMBIE_DOOR,			4,	12,	1000 },
	{ ZombieType::ZOMBIE_FOOTBALL,		7,	16,	1000 },
	{ ZombieType::ZOMBIE_DANCER,		5,	20,	500 },
	{ ZombieType::ZOMBIE_GARGANTUAR,	10,	28,	500 },
};

// Points a natural wave spends on zombies (doubled for huge waves)
static int NaturalWavePoints(int theWave)
{
	return 1 + theWave * 2 / 3;
}

// Small deterministic generator so both players roll the same waves from the shared seed
class VersusWaveRandom
{
public:
	uint32_t mState;

	VersusWaveRandom(uint32_t theSeed, int theWave) : mState(theSeed * 2654435761u + (uint32_t)theWave * 40503u + 1u) { if (mState == 0) mState = 1; }
	uint32_t Next() { mState ^= mState << 13; mState ^= mState >> 17; mState ^= mState << 5; return mState; }
	int Range(int theMax) { return (int)(Next() % (uint32_t)theMax); }
};

// Binary messages, relayed untouched by the server. Multi-byte values are little-endian.
//   zombie   [1][seq u16][zombie type u8][row u8]
//   snapshot [2][sun u16][income u16][mower bits u8][brains u8]
//            [plant count u8] { [x i16][y i16][seed type u8] }
//            [zombie count u8] { [id u16][zombie type u8][x i16][y i16][flags u8] }
//   lost     [3][seq u16]
//   resume   [4][last seq received u16]   sent after a reconnect; the other side resends what's missing
// Messages with a seq are reliable: numbered from 1, kept by the sender, and ignored if already seen.
enum VersusMessage
{
	VERSUS_MSG_ZOMBIE = 1,
	VERSUS_MSG_SNAPSHOT = 2,
	VERSUS_MSG_LOST = 3,
	VERSUS_MSG_RESUME = 4
};

enum VersusZombieFlags
{
	VERSUS_ZOMBIE_DYING = 1,
	VERSUS_ZOMBIE_CHILLED = 2
};

// The opponent view shows this part of the board (the lawn rows plus a little sky)
static const int OPPONENT_VIEW_SRC_Y = 60;
static const int OPPONENT_VIEW_SRC_WIDTH = BOARD_WIDTH;
static const int OPPONENT_VIEW_SRC_HEIGHT = 540;  // tall enough for the six lanes of the pool
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

	// A line of text under the opponent view, centred on it but kept on screen
	void DrawCaption(Graphics* g, const SexyString& theText, int theY, const Color& theColor)
	{
		Rect aRect = Versus::GetOpponentViewRect();
		int aWidth = FONT_BRIANNETOD12->StringWidth(theText);
		int aX = std::min(aRect.mX + (aRect.mWidth - aWidth) / 2, BOARD_WIDTH - 14 - aWidth);
		TodDrawString(g, theText, aX, theY, FONT_BRIANNETOD12, theColor, DrawStringJustification::DS_ALIGN_LEFT);
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

Versus::Versus(LawnApp* theApp) : mApp(theApp), mState(VERSUS_STATE_OFF), mResult(VERSUS_RESULT_NONE), mSeed(0), mMap(VERSUS_MAP_DAY), mZombieChooser(nullptr),
	mConnectionDialog(VERSUS_CONNECTION_DIALOG_NONE), mKeepRunningHidden(false)
{
	ResetMatch();
}

void Versus::ResetMatch()
{
	mResult = VERSUS_RESULT_NONE;
	mOpponentReady = false;
	mGo = false;
	mZombiePacketsAdded = false;
	mZombiesChosen = false;
	mShowingWaitAdvice = false;
	mIncome = VERSUS_STARTING_INCOME;
	mIncomeCounter = 0;
	mSnapshotCounter = 0;
	mPlayTime = 0;
	mWaveNumber = 0;
	mHasOpponentSnapshot = false;
	mOpponentSun = 0;
	mOpponentIncome = 0;
	mOpponentMowers = 0;
	mBrains = VERSUS_STARTING_BRAINS;
	mOpponentBrains = VERSUS_STARTING_BRAINS;
	mBounties.clear();
	mOpponentPlants.clear();
	mOpponentZombies.clear();
	mPendingZombies.clear();
	mFarmFirstSun.clear();
	mNextSendSeq = 1;
	mLastReceivedSeq = 0;
	mSentReliable.clear();
	mReconnecting = false;
	mOpponentDisconnected = false;
	mLastReceiveTime = mLastKeepaliveTime = SDL_GetTicks();
}

void Versus::StartMatchmaking(const std::string& theJoinCode, VersusMap theMap)
{
	if (mState != VERSUS_STATE_OFF)
		return;

	mMap = theMap;  // used when creating a room; joining takes the room's map

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
	RemoveZombieChooser();
	if (mConnection.IsOpen())
		mConnection.SendText("{\"t\":\"leave\"}");
	mConnection.Close();
	mState = VERSUS_STATE_OFF;
	mStatus.clear();
	mReconnecting = false;
	mOpponentDisconnected = false;
	UpdateConnectionDialog();
}

void Versus::Update()
{
	VersusConnection::Event anEvent;
	while (mState != VERSUS_STATE_OFF && mConnection.Poll(anEvent))
	{
		switch (anEvent.mType)
		{
		case VersusConnection::EVENT_OPEN:
			mLastReceiveTime = SDL_GetTicks();
			if (mReconnecting)
			{
				mConnection.SendText("{\"t\":\"rejoin\",\"code\":\"" + mRoomCode + "\",\"token\":\"" + mToken + "\"}");
				break;
			}
			if (mJoinCode.empty())
				mConnection.SendText(StrFormat("{\"t\":\"create\",\"map\":%d}", (int)mMap));
			else
				mConnection.SendText("{\"t\":\"join\",\"code\":\"" + mJoinCode + "\"}");
			mStatus = mJoinCode.empty() ? "Creating a room..." : "Joining room " + mJoinCode + "...";
			break;

		case VersusConnection::EVENT_CLOSE:
			HandleDisconnect();
			break;

		case VersusConnection::EVENT_TEXT:
			mLastReceiveTime = SDL_GetTicks();
			HandleControl(anEvent.mData);
			break;

		case VersusConnection::EVENT_BINARY:
			mLastReceiveTime = SDL_GetTicks();
			HandleGameData(anEvent.mData);
			break;
		}
	}

	UpdateConnection();

	if (mKeepRunningHidden != IsInMatch())
	{
		mKeepRunningHidden = IsInMatch();
		SetKeepRunningHiddenJS(mKeepRunningHidden);
	}

	// Waves, income and snapshots stop while the game is paused for a lost connection, so both
	// players' waves stay in step
	Board* aBoard = mApp->mBoard;
	if (mState != VERSUS_STATE_PLAYING || aBoard == nullptr || !mApp->IsVersusMode() || mApp->mGameScene != GameScenes::SCENE_PLAYING || aBoard->mPaused)
		return;

	for (const VersusPendingZombie& aZombie : mPendingZombies)
		SpawnZombie(aBoard, aZombie.mZombieType, aZombie.mRow);
	mPendingZombies.clear();

	mPlayTime++;
	UpdateNaturalWaves(aBoard);
	UpdateBounties(aBoard);

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

	if (aType == "pong")
	{
		return;
	}
	else if (aType == "created")
	{
		mRoomCode = JsonField(theMessage, "code");
		mToken = JsonField(theMessage, "token");
		mState = VERSUS_STATE_IN_LOBBY;
		mStatus = "Room code: " + mRoomCode + " (" + gVersusMaps[mMap].mName + ")\nWaiting for an opponent...";
		TodTrace("Versus: created room %s", mRoomCode.c_str());
	}
	else if (aType == "joined")
	{
		mRoomCode = JsonField(theMessage, "code");
		mToken = JsonField(theMessage, "token");
		mMap = (VersusMap)std::clamp(atoi(JsonField(theMessage, "map").c_str()), 0, NUM_VERSUS_MAPS - 1);
		mState = VERSUS_STATE_IN_LOBBY;
		mStatus = "Joined room " + mRoomCode + " (" + gVersusMaps[mMap].mName + ").";
	}
	else if (aType == "opponent_joined")
	{
		mStatus = "An opponent joined!";
	}
	else if (aType == "start")
	{
		mSeed = (uint32_t)strtoul(JsonField(theMessage, "seed").c_str(), nullptr, 10);
		mMap = (VersusMap)std::clamp(atoi(JsonField(theMessage, "map").c_str()), 0, NUM_VERSUS_MAPS - 1);
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
	else if (aType == "rejoined")
	{
		TodTrace("Versus: rejoined room %s", mRoomCode.c_str());
		mReconnecting = false;
		mOpponentDisconnected = JsonField(theMessage, "opponentConnected") != "true";
		if (JsonField(theMessage, "opponentReady") == "true")
			mOpponentReady = true;
		if (JsonField(theMessage, "go") == "true" && !mGo)
		{
			mGo = true;
			mState = VERSUS_STATE_PLAYING;
		}
		else if (mZombiesChosen && !mGo)
		{
			mConnection.SendText("{\"t\":\"ready\"}");  // it may have been lost with the old connection
		}
		SendResume();
		UpdateConnectionDialog();
	}
	else if (aType == "opponent_disconnected")
	{
		mOpponentDisconnected = true;
		UpdateConnectionDialog();
	}
	else if (aType == "opponent_reconnected")
	{
		mOpponentDisconnected = false;
		SendResume();
		UpdateConnectionDialog();
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
		else if (mReconnecting)
		{
			EndMatch(VERSUS_RESULT_DISCONNECTED);  // the server gave our spot away
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
		if (mReconnecting)
			mConnection.Close();  // this attempt failed; UpdateConnection tries again
		else
			BeginReconnect();
		break;

	default:
		break;
	}
}

void Versus::BeginReconnect()
{
	TodTrace("Versus: connection lost, reconnecting");
	mConnection.Close();
	mReconnecting = true;
	uint32_t aNow = SDL_GetTicks();
	mReconnectGiveUpTime = aNow + VERSUS_RECONNECT_GIVE_UP_MS;
	mNextConnectTime = aNow;
	UpdateConnectionDialog();
}

void Versus::UpdateConnection()
{
	if (!IsInMatch())
		return;

	uint32_t aNow = SDL_GetTicks();
	if (mReconnecting)
	{
		if ((int)(aNow - mReconnectGiveUpTime) >= 0)
		{
			mConnection.Close();
			EndMatch(VERSUS_RESULT_DISCONNECTED);
			return;
		}

		// An attempt that hasn't opened in time is abandoned and retried
		if (mConnection.IsConnecting() && (int)(aNow - mConnectStartTime) >= VERSUS_CONNECT_TIMEOUT_MS)
			mConnection.Close();

		if (!mConnection.IsConnecting() && !mConnection.IsOpen() && (int)(aNow - mNextConnectTime) >= 0)
		{
			mConnectStartTime = aNow;
			mNextConnectTime = aNow + VERSUS_RECONNECT_INTERVAL_MS;
			mConnection.Connect(VersusConnection::GetServerUrl());
		}
		return;
	}

	if (!mConnection.IsOpen())
		return;

	if ((int)(aNow - mLastKeepaliveTime) >= VERSUS_KEEPALIVE_MS)
	{
		mLastKeepaliveTime = aNow;
		mConnection.SendText("{\"t\":\"ping\"}");
	}

	// The server answers every keepalive, so a long silence means the connection is dead even if the
	// browser hasn't noticed yet
	if ((int)(aNow - mLastReceiveTime) >= VERSUS_SILENCE_TIMEOUT_MS)
		BeginReconnect();
}

void Versus::UpdateConnectionDialog()
{
	int aWanted = VERSUS_CONNECTION_DIALOG_NONE;
	if (IsInMatch() && mReconnecting)
		aWanted = VERSUS_CONNECTION_DIALOG_RECONNECTING;
	else if (IsInMatch() && mOpponentDisconnected)
		aWanted = VERSUS_CONNECTION_DIALOG_OPPONENT_AWAY;

	if (aWanted == mConnectionDialog)
		return;

	// Showing the dialog pauses the board (LawnApp::NeedPauseGame asks IsConnectionPaused), and
	// killing it resumes the board
	mConnectionDialog = aWanted;
	mApp->KillDialog(Dialogs::DIALOG_VERSUS_CONNECTION);
	if (aWanted == VERSUS_CONNECTION_DIALOG_RECONNECTING)
	{
		mApp->DoDialog(Dialogs::DIALOG_VERSUS_CONNECTION, true, __S("CONNECTION LOST"),
			__S("Trying to reconnect...\nThe game is paused for both players."), __S("Leave Match"), Dialog::BUTTONS_FOOTER);
	}
	else if (aWanted == VERSUS_CONNECTION_DIALOG_OPPONENT_AWAY)
	{
		mApp->DoDialog(Dialogs::DIALOG_VERSUS_CONNECTION, true, __S("PAUSED"),
			__S("Your opponent lost their connection.\nWaiting for them to come back..."), __S("Leave Match"), Dialog::BUTTONS_FOOTER);
	}

	if (mApp->mBoard)
		mApp->mBoard->Pause(aWanted != VERSUS_CONNECTION_DIALOG_NONE);
}

// The lane a zombie of this type would use on this board: theRow if it can go there, otherwise the
// closest lane it can (a Football Zombie sent down a pool lane walks on the nearest grass instead)
int Versus::PickLane(Board* theBoard, ZombieType theZombieType, int theRow)
{
	auto aLaneAccepts = [theBoard, theZombieType](int theLane)
	{
		if (!theBoard->RowCanHaveZombies(theLane))
			return false;
		bool aPoolLane = theBoard->mPlantRow[theLane] == PlantRowType::PLANTROW_POOL;
		if (aPoolLane && !Zombie::ZombieTypeCanGoInPool(theZombieType))
			return false;
		if (!aPoolLane && Board::IsZombieTypePoolOnly(theZombieType))
			return false;
		if (theBoard->mPlantRow[theLane] == PlantRowType::PLANTROW_HIGH_GROUND && !Zombie::ZombieTypeCanGoOnHighGround(theZombieType))
			return false;
		// Dancers summon backup dancers above and below them
		if (theZombieType == ZombieType::ZOMBIE_DANCER && !theBoard->StageHasPool())
			return theBoard->RowCanHaveZombies(theLane - 1) && theBoard->RowCanHaveZombies(theLane + 1);
		return true;
	};

	for (int aDistance = 0; aDistance < MAX_GRID_SIZE_Y; aDistance++)
	{
		if (aLaneAccepts(theRow - aDistance))
			return theRow - aDistance;
		if (aLaneAccepts(theRow + aDistance))
			return theRow + aDistance;
	}
	return -1;
}

bool Versus::IsZombieAllowedOnMap(ZombieType theZombieType) const
{
	// Nothing can dig through the roof, and dancers' backups rise out of the ground
	if (mMap == VERSUS_MAP_ROOF)
		return theZombieType != ZombieType::ZOMBIE_DIGGER && theZombieType != ZombieType::ZOMBIE_DANCER && theZombieType != ZombieType::ZOMBIE_ZAMBONI;
	return true;
}

void Versus::SpawnZombie(Board* theBoard, ZombieType theZombieType, int theRow)
{
	int aLane = PickLane(theBoard, theZombieType, theRow);
	TodTrace("Versus: received zombie %d for row %d, sent down row %d", theZombieType, theRow, aLane);
	if (aLane < 0)
		return;

	Zombie* aZombie = AddSpacedZombie(theBoard, theZombieType, aLane);
	for (const VersusZombieOffer& anOffer : gVersusZombieOffers)
	{
		if (aZombie && theZombieType != ZombieType::ZOMBIE_BUNGEE && Challenge::IZombieSeedTypeToZombieType(anOffer.mSeedType) == theZombieType)
			AddBounty(aZombie, anOffer.mCost / VERSUS_BOUNTY_DIVISOR);
	}
}

void Versus::AddBounty(Zombie* theZombie, int theSun)
{
	mBounties[mApp->mBoard->mZombies.DataArrayGetID(theZombie)] = theSun;
}

// Pays out a zombie's bounty as sun dropped where it dies
void Versus::UpdateBounties(Board* theBoard)
{
	for (auto anIt = mBounties.begin(); anIt != mBounties.end(); )
	{
		Zombie* aZombie = theBoard->mZombies.DataArrayTryToGet(anIt->first);
		if (aZombie && !aZombie->mDead && !aZombie->IsDeadOrDying())
		{
			++anIt;
			continue;
		}

		if (aZombie)
		{
			TodTrace("Versus: bounty of %d sun", anIt->second);
			int aSun = anIt->second;
			int aX = (int)aZombie->mPosX + 30, aY = (int)aZombie->mPosY + 30;
			for (; aSun >= 50; aSun -= 50)
				theBoard->AddCoin(aX, aY, CoinType::COIN_LARGESUN, CoinMotion::COIN_MOTION_FROM_PLANT);
			for (; aSun >= 25; aSun -= 25)
				theBoard->AddCoin(aX, aY, CoinType::COIN_SUN, CoinMotion::COIN_MOTION_FROM_PLANT);
			if (aSun >= 10)
				theBoard->AddCoin(aX, aY, CoinType::COIN_SMALLSUN, CoinMotion::COIN_MOTION_FROM_PLANT);
		}
		anIt = mBounties.erase(anIt);
	}
}

// A zombie got past the lawnmowers. Returns true if it only cost a brain, false if that was the last
// one and the match is lost.
bool Versus::OnZombieReachedHouse(Zombie* theZombie)
{
	Board* aBoard = mApp->mBoard;
	if (mState != VERSUS_STATE_PLAYING || theZombie == nullptr || aBoard == nullptr || mBrains <= 1)
	{
		mBrains = 0;
		return false;
	}

	mBrains--;
	mBounties.erase(aBoard->mZombies.DataArrayGetID(theZombie));
	theZombie->DieNoLoot();
	mApp->PlayFoley(FoleyType::FOLEY_SCREAM);
	aBoard->ClearAdviceImmediately();
	aBoard->DisplayAdvice(StrFormat(mBrains == 1 ? __S("A zombie ate a brain! %d brain left") : __S("A zombie ate a brain! %d brains left"), mBrains),
		MessageStyle::MESSAGE_STYLE_HINT_FAST, AdviceType::ADVICE_NONE);
	TodTrace("Versus: a zombie ate a brain, %d left", mBrains);
	return true;
}

// Adds a zombie at the right edge of a lane, pushed further right (to walk in behind) if it would
// otherwise bunch up with the zombies already waiting there
Zombie* Versus::AddSpacedZombie(Board* theBoard, ZombieType theZombieType, int theLane)
{
	float aRightmost = -1000.0f;
	Zombie* anOther = nullptr;
	while (theBoard->IterateZombies(anOther))
	{
		if (anOther->mRow == theLane && anOther->mZombieType != ZombieType::ZOMBIE_BUNGEE && !anOther->IsDeadOrDying())
			aRightmost = std::max(aRightmost, anOther->mPosX);
	}

	Zombie* aZombie = theBoard->AddZombieInRow(theZombieType, theLane, 0);
	if (aZombie == nullptr || theZombieType == ZombieType::ZOMBIE_BUNGEE)
		return aZombie;  // bungees drop in from above

	if (aZombie->mPosX < aRightmost + VERSUS_ZOMBIE_SPACING)
	{
		aZombie->mPosX = aRightmost + VERSUS_ZOMBIE_SPACING;
		aZombie->mX = (int)aZombie->mPosX;
	}
	return aZombie;
}

void Versus::HandleGameData(const std::string& theData)
{
	if (theData.empty() || !IsInMatch())
		return;

	Board* aBoard = mApp->mBoard;
	switch ((uint8_t)theData[0])
	{
	case VERSUS_MSG_ZOMBIE:
	{
		ByteReader aReader(theData);
		aReader.U8();
		int aSeq = aReader.U16();
		if (aSeq <= mLastReceivedSeq)
			break;
		mLastReceivedSeq = aSeq;
		ZombieType aZombieType = (ZombieType)aReader.U8();
		int aRow = aReader.U8();
		if (!aReader.mOk || aZombieType < 0 || aZombieType >= ZombieType::NUM_ZOMBIE_TYPES || aRow < 0 || aRow >= MAX_GRID_SIZE_Y)
			break;

		// Hold on to zombies that arrive before our level has started, or while we're paused
		if (aBoard && mState == VERSUS_STATE_PLAYING && mApp->mGameScene == GameScenes::SCENE_PLAYING && !aBoard->mPaused)
			SpawnZombie(aBoard, aZombieType, aRow);
		else
			mPendingZombies.push_back({ aZombieType, aRow });
		break;
	}

	case VERSUS_MSG_SNAPSHOT:
		ReadSnapshot(theData);
		break;

	case VERSUS_MSG_LOST:
	{
		ByteReader aReader(theData);
		aReader.U8();
		int aSeq = aReader.U16();
		if (aSeq <= mLastReceivedSeq)
			break;
		mLastReceivedSeq = aSeq;
		EndMatch(VERSUS_RESULT_WON);
		break;
	}

	case VERSUS_MSG_RESUME:
	{
		ByteReader aReader(theData);
		aReader.U8();
		int aLastSeen = aReader.U16();
		int aResent = 0;
		for (const std::vector<uint8_t>& aMessage : mSentReliable)
		{
			int aSeq = aMessage[1] | (aMessage[2] << 8);
			if (aSeq > aLastSeen)
			{
				mConnection.SendBinary(aMessage);
				aResent++;
			}
		}
		TodTrace("Versus: opponent has up to %d, resent %d", aLastSeen, aResent);
		break;
	}
	}
}

void Versus::SendZombie(SeedType theSeedType, int theRow)
{
	ZombieType aZombieType = Challenge::IZombieSeedTypeToZombieType(theSeedType);
	int aPackSize = GetPackSize();
	for (int i = 0; i < aPackSize; i++)
	{
		ByteWriter aWriter;
		aWriter.U8(VERSUS_MSG_ZOMBIE);
		aWriter.U16(0);  // seq, filled in by SendReliable
		aWriter.U8(aZombieType);
		aWriter.U8(theRow);
		SendReliable(aWriter.mData);
	}

	mIncome += GetIncomeGain(GetSendCost(theSeedType));
	TodTrace("Versus: sent %d x zombie %d to row %d, income now %d", aPackSize, aZombieType, theRow, mIncome);
}

void Versus::SendReliable(std::vector<uint8_t> theMessage)
{
	int aSeq = mNextSendSeq++;
	theMessage[1] = aSeq & 0xFF;
	theMessage[2] = (aSeq >> 8) & 0xFF;
	mSentReliable.push_back(theMessage);
	mConnection.SendBinary(theMessage);
}

void Versus::SendResume()
{
	ByteWriter aWriter;
	aWriter.U8(VERSUS_MSG_RESUME);
	aWriter.U16(mLastReceivedSeq);
	mConnection.SendBinary(aWriter.mData);
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
	aWriter.U8(mBrains);

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
	int aBrains = aReader.U8();

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
	mOpponentBrains = aBrains;
	mOpponentPlants.swap(aPlants);
	mOpponentZombies.swap(aZombies);
}

int Versus::GetNumSeedsInBank() const
{
	return VERSUS_NUM_PLANT_PACKETS + (mZombiePacketsAdded ? VERSUS_NUM_ZOMBIE_PACKETS : 0);
}

void Versus::OnSeedsChosen(Board* theBoard)
{
	// Make room for the zombie packets, which are picked next in the zombie chooser
	mZombiePacketsAdded = true;
	theBoard->mSeedBank->UpdateWidth();
	for (int i = 0; i < VERSUS_NUM_ZOMBIE_PACKETS; i++)
	{
		theBoard->mSeedBank->mSeedPackets[VERSUS_NUM_PLANT_PACKETS + i].mPacketType = SeedType::SEED_NONE;
	}

	// The seed chooser's Menu button lingers until the level starts, on top of the opponent view
	if (mApp->mSeedChooserScreen)
	{
		mApp->mSeedChooserScreen->mMenuButton->mBtnNoDraw = true;
		mApp->mSeedChooserScreen->mMenuButton->mDisabled = true;
	}
}

// Called every update while the intro holds before "Ready, Set, Plant!". Both games hold at the
// same point and only report ready from there, so the server's "go" starts them together.
void Versus::UpdateWaitingToStart(Board* theBoard)
{
	if (!mZombiesChosen)
	{
		if (mZombieChooser == nullptr && mState == VERSUS_STATE_CHOOSING_SEEDS)
		{
			mZombieChooser = new ZombieChooserScreen(mApp);
			mApp->mWidgetManager->AddWidget(mZombieChooser);
			mApp->mWidgetManager->BringToFront(mZombieChooser);
			mApp->mWidgetManager->SetFocus(mZombieChooser);
		}
	}
	else if (!mShowingWaitAdvice)
	{
		theBoard->DisplayAdvice(__S("Waiting for your opponent..."), MessageStyle::MESSAGE_STYLE_HINT_STAY, AdviceType::ADVICE_NONE);
		mShowingWaitAdvice = true;
	}
}

void Versus::OnZombiesChosen(const std::vector<SeedType>& theZombieSeeds)
{
	Board* aBoard = mApp->mBoard;
	if (aBoard == nullptr || theZombieSeeds.size() != VERSUS_NUM_ZOMBIE_PACKETS)
		return;

	for (int i = 0; i < VERSUS_NUM_ZOMBIE_PACKETS; i++)
	{
		aBoard->mSeedBank->mSeedPackets[VERSUS_NUM_PLANT_PACKETS + i].SetPacketType(theZombieSeeds[i], SeedType::SEED_NONE);
	}

	mZombiesChosen = true;
	RemoveZombieChooser();
	mApp->mWidgetManager->SetFocus(aBoard);
	mConnection.SendText("{\"t\":\"ready\"}");
}

void Versus::RemoveZombieChooser()
{
	if (mZombieChooser)
	{
		mApp->mWidgetManager->RemoveWidget(mZombieChooser);
		mApp->SafeDeleteWidget(mZombieChooser);
		mZombieChooser = nullptr;
	}
}

void Versus::OnZombiesWon()
{
	if (mState == VERSUS_STATE_OVER)
		return;

	ByteWriter aWriter;
	aWriter.U8(VERSUS_MSG_LOST);
	aWriter.U16(0);
	SendReliable(aWriter.mData);

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
	mReconnecting = false;
	mOpponentDisconnected = false;
	UpdateConnectionDialog();
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
	case VERSUS_RESULT_LOST:			aHeader = "YOU LOSE";	aBody = "The zombies ate all your brains.\nYour opponent wins.";	break;
	case VERSUS_RESULT_OPPONENT_LEFT:	aHeader = "YOU WIN!";	aBody = "Your opponent left the match.";						break;
	case VERSUS_RESULT_DISCONNECTED:	aHeader = "DISCONNECTED";	aBody = "Lost the connection to\nthe Versus server.";		break;
	default:																											break;
	}

	mApp->KillDialog(Dialogs::DIALOG_VERSUS_RESULT);
	mApp->DoDialog(Dialogs::DIALOG_VERSUS_RESULT, true, StringToSexyString(aHeader), StringToSexyString(aBody), __S("Main Menu"), Dialog::BUTTONS_FOOTER);
	if (mApp->mBoard)
		mApp->mBoard->Pause(true);
}

void Versus::UpdateNaturalWaves(Board* theBoard)
{
	int aNextWaveTime = VERSUS_FIRST_WAVE_TIME + mWaveNumber * VERSUS_WAVE_INTERVAL;
	bool aNextIsHuge = (mWaveNumber + 1) % VERSUS_HUGE_WAVE_EVERY == 0;

	if (aNextIsHuge && mPlayTime == aNextWaveTime - 500)
	{
		theBoard->ClearAdviceImmediately();
		theBoard->DisplayAdviceAgain(__S("[ADVICE_HUGE_WAVE]"), MessageStyle::MESSAGE_STYLE_HUGE_WAVE, AdviceType::ADVICE_HUGE_WAVE);
		mApp->PlaySample(Sexy::SOUND_HUGE_WAVE);
	}

	if (mPlayTime >= aNextWaveTime)
	{
		mWaveNumber++;
		SpawnNaturalWave(theBoard, mWaveNumber);
		if (mWaveNumber == 1)
			mApp->PlaySample(Sexy::SOUND_AWOOGA);
	}
}

void Versus::SpawnNaturalWave(Board* theBoard, int theWave)
{
	VersusWaveRandom aRandom(mSeed, theWave);
	bool aHuge = theWave % VERSUS_HUGE_WAVE_EVERY == 0;
	int aPoints = NaturalWavePoints(theWave) * (aHuge ? 2 : 1);
	int aCount = 0;

	int aNumLanes = theBoard->StageHas6Rows() ? 6 : 5;
	if (aHuge)
	{
		int aLane = PickLane(theBoard, ZombieType::ZOMBIE_FLAG, aRandom.Range(aNumLanes));
		if (aLane >= 0)
			if (Zombie* aFlag = AddSpacedZombie(theBoard, ZombieType::ZOMBIE_FLAG, aLane))
				AddBounty(aFlag, 15);
		aCount++;
	}

	while (aPoints > 0 && aCount < VERSUS_MAX_ZOMBIES_PER_WAVE)
	{
		int aTotalWeight = 0;
		for (const VersusNaturalZombie& aZombie : gVersusNaturalZombies)
			if (theWave >= aZombie.mFirstWave && aZombie.mValue <= aPoints && IsZombieAllowedOnMap(aZombie.mZombieType))
				aTotalWeight += aZombie.mWeight;
		if (aTotalWeight == 0)
			break;

		int aPick = aRandom.Range(aTotalWeight);
		for (const VersusNaturalZombie& aZombie : gVersusNaturalZombies)
		{
			if (theWave < aZombie.mFirstWave || aZombie.mValue > aPoints || !IsZombieAllowedOnMap(aZombie.mZombieType))
				continue;
			if (aPick < aZombie.mWeight)
			{
				int aLane = PickLane(theBoard, aZombie.mZombieType, aRandom.Range(aNumLanes));
				if (aLane >= 0)
					if (Zombie* aSpawned = AddSpacedZombie(theBoard, aZombie.mZombieType, aLane))
						AddBounty(aSpawned, 15);
				aPoints -= aZombie.mValue;
				aCount++;
				break;
			}
			aPick -= aZombie.mWeight;
		}
	}

	TodTrace("Versus: natural wave %d, %d zombies", theWave, aCount);
}

int Versus::GetFarmCost(SeedType theSeedType)
{
	switch (theSeedType)
	{
	case SeedType::SEED_SUNFLOWER:		return VERSUS_SUNFLOWER_COST;
	case SeedType::SEED_TWINSUNFLOWER:	return VERSUS_TWINSUNFLOWER_COST;
	case SeedType::SEED_SUNSHROOM:		return VERSUS_SUNSHROOM_COST;
	default:							return 0;
	}
}

// Drops a sun producer's sun, sized by how long it has been producing
void Versus::AddFarmSun(Plant* thePlant)
{
	Board* aBoard = thePlant->mBoard;
	unsigned int anId = aBoard->mPlants.DataArrayGetID(thePlant);
	auto anEntry = mFarmFirstSun.emplace(anId, mPlayTime).first;
	int anAge = mPlayTime - anEntry->second;

	// A small sun-shroom hasn't grown up yet and keeps dropping small suns
	if (thePlant->mSeedType == SeedType::SEED_SUNSHROOM && thePlant->mState == PlantState::STATE_SUNSHROOM_SMALL)
	{
		aBoard->AddCoin(thePlant->mX, thePlant->mY, CoinType::COIN_SMALLSUN, CoinMotion::COIN_MOTION_FROM_PLANT);
		return;
	}

	int aDrops = thePlant->mSeedType == SeedType::SEED_TWINSUNFLOWER ? 2 : 1;
	for (int i = 0; i < aDrops; i++)
	{
		if (anAge < VERSUS_FARM_RIPE_TIME)
		{
			aBoard->AddCoin(thePlant->mX, thePlant->mY, CoinType::COIN_SUN, CoinMotion::COIN_MOTION_FROM_PLANT);
		}
		else
		{
			aBoard->AddCoin(thePlant->mX, thePlant->mY, CoinType::COIN_LARGESUN, CoinMotion::COIN_MOTION_FROM_PLANT);
			if (anAge >= VERSUS_FARM_FULL_TIME)
				aBoard->AddCoin(thePlant->mX, thePlant->mY, CoinType::COIN_SUN, CoinMotion::COIN_MOTION_FROM_PLANT);
		}
	}
}

int Versus::GetPackSize() const
{
	int aSize = 1;
	for (const VersusPackStep& aStep : VERSUS_PACK_STEPS)
	{
		if (mWaveNumber >= aStep.mFromWave)
			aSize = aStep.mSize;
	}
	return aSize;
}

// Each send raises income by up to cost / VERSUS_INCOME_PER_SEND_DIVISOR, less the closer income is
// to VERSUS_MAX_INCOME
int Versus::GetIncomeGain(int theCost) const
{
	int aRoom = std::max(VERSUS_MAX_INCOME - mIncome, 0);
	int aGain = theCost * aRoom / (VERSUS_INCOME_PER_SEND_DIVISOR * VERSUS_MAX_INCOME);
	return std::min(std::max(aGain, aRoom > 0 ? 1 : 0), aRoom);
}

int Versus::GetUnlockWave(SeedType theSeedType)
{
	if (Challenge::IsZombieSeedType(theSeedType))
		return GetZombieUnlockWave(theSeedType);
	for (const VersusPlantUnlock& anUnlock : VERSUS_PLANT_UNLOCKS)
	{
		if (anUnlock.mSeedType == theSeedType)
			return anUnlock.mUnlockWave;
	}
	return 0;
}

int Versus::GetZombieUnlockWave(SeedType theSeedType)
{
	for (const VersusZombieOffer& anOffer : gVersusZombieOffers)
	{
		if (anOffer.mSeedType == theSeedType)
			return anOffer.mUnlockWave;
	}
	return 0;
}

// Darkens zombie packets that haven't unlocked yet and says which wave unlocks them, and marks
// sends that are packs of zombies
void Versus::DrawZombiePacketOverlays(Graphics* g)
{
	Board* aBoard = mApp->mBoard;
	if (aBoard == nullptr || !mZombiesChosen)
		return;

	SeedBank* aSeedBank = aBoard->mSeedBank;
	for (int i = 0; i < aSeedBank->mNumPackets; i++)
	{
		SeedPacket& aPacket = aSeedBank->mSeedPackets[i];
		if (aPacket.mPacketType == SeedType::SEED_NONE)
			continue;

		int aX = aSeedBank->mX + aPacket.mX, aY = aSeedBank->mY + aPacket.mY;
		if (IsSeedUnlocked(aPacket.mPacketType))
		{
			if (Challenge::IsZombieSeedType(aPacket.mPacketType) && GetPackSize() > 1)
			{
				TodDrawString(g, StrFormat(__S("x%d"), GetPackSize()), aX + SEED_PACKET_WIDTH - 4, aY + 20,
					FONT_CONTINUUMBOLD14, Color(255, 230, 90), DrawStringJustification::DS_ALIGN_RIGHT);
			}
			continue;
		}

		g->SetColor(Color(0, 0, 0, 150));
		g->FillRect(aX, aY, SEED_PACKET_WIDTH, SEED_PACKET_HEIGHT);
		TodDrawString(g, StrFormat(__S("Wave %d"), GetUnlockWave(aPacket.mPacketType)), aX + SEED_PACKET_WIDTH / 2, aY + 40,
			FONT_BRIANNETOD12, Color(255, 220, 120), DrawStringJustification::DS_ALIGN_CENTER);
	}
}

int Versus::GetZombieCost(SeedType theSeedType)
{
	for (const VersusZombieOffer& anOffer : gVersusZombieOffers)
	{
		if (anOffer.mSeedType == theSeedType)
			return anOffer.mCost;
	}
	return 0;
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

	// Both lawns are the same map, so our own board's lane layout (rows, pool, roof slope) applies
	Board* aBoard = gLawnApp->mBoard;
	if (aBoard == nullptr)
		return -1;
	int aBoardX = (int)((theX - aRect.mX) / OPPONENT_VIEW_SCALE);
	int aBoardY = (int)(OPPONENT_VIEW_SRC_Y + (theY - aRect.mY) / OPPONENT_VIEW_SCALE);
	int aRow = aBoard->PixelToGridY(aBoardX, aBoardY);
	return aRow >= 0 && aBoard->RowCanHaveZombies(aRow) ? aRow : -1;
}

void Versus::DrawOpponentView(Graphics* g)
{
	Rect aRect = GetOpponentViewRect();

	g->SetColor(Color(0, 0, 0, 200));
	g->FillRect(aRect.mX - 2, aRect.mY - 2, aRect.mWidth + 4, aRect.mHeight + 4);

	Graphics aView(*g);
	aView.ClipRect(aRect);
	Board* aBoard = mApp->mBoard;
	Image* aBackgrounds[NUM_VERSUS_MAPS] = { IMAGE_BACKGROUND1, IMAGE_BACKGROUND2, IMAGE_BACKGROUND3, IMAGE_BACKGROUND4, IMAGE_BACKGROUND5 };
	if (aBackgrounds[mMap])
		aView.DrawImage(aBackgrounds[mMap], aRect, Rect(BOARD_OFFSET, OPPONENT_VIEW_SRC_Y, OPPONENT_VIEW_SRC_WIDTH, OPPONENT_VIEW_SRC_HEIGHT));

	// The background image leaves the pool blank (the board draws the water on top), so paint it in
	for (int aRow = 0; aBoard && aBoard->StageHasPool() && aRow < MAX_GRID_SIZE_Y; aRow++)
	{
		if (aBoard->mPlantRow[aRow] != PlantRowType::PLANTROW_POOL)
			continue;
		int aTop = (int)OpponentViewY(aBoard->GridToPixelY(0, aRow));
		int aBottom = (int)OpponentViewY(aBoard->GridToPixelY(0, aRow) + 85);
		aView.SetColor(mMap == VERSUS_MAP_FOG ? Color(28, 58, 92) : Color(60, 150, 215));
		aView.FillRect((int)OpponentViewX(LAWN_XMIN - 5), aTop, (int)(735 * OPPONENT_VIEW_SCALE), aBottom - aTop);
	}

	aView.mScaleX = OPPONENT_VIEW_SCALE;
	aView.mScaleY = OPPONENT_VIEW_SCALE;
	for (int aRow = 0; aBoard && aRow < MAX_GRID_SIZE_Y; aRow++)
	{
		if (mOpponentMowers & (1 << aRow))
		{
			LawnMowerType aMowerType = aBoard->StageHasRoof() ? LawnMowerType::LAWNMOWER_ROOF :
				aBoard->mPlantRow[aRow] == PlantRowType::PLANTROW_POOL ? LawnMowerType::LAWNMOWER_POOL : LawnMowerType::LAWNMOWER_LAWN;
			// DrawCachedMower shifts left by 20 unscaled pixels
			mApp->mReanimatorCache->DrawCachedMower(&aView, OpponentViewX(0) + 20, OpponentViewY(aBoard->GridToPixelY(0, aRow) + 23), aMowerType);
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
	if (aBoard && Challenge::IsZombieSeedType(aBoard->GetSeedTypeInCursor()))
	{
		int aRow = OpponentViewRowAt(mApp->mWidgetManager->mLastMouseX, mApp->mWidgetManager->mLastMouseY);
		if (aRow >= 0)
		{
			int aRowHeight = aBoard->StageHas6Rows() ? 85 : 100;
			g->SetColor(Color(255, 255, 255, 70));
			g->FillRect(aRect.mX, (int)OpponentViewY(aBoard->GridToPixelY(8, aRow)), aRect.mWidth, (int)(aRowHeight * OPPONENT_VIEW_SCALE));
		}
	}

	SexyString aCaption = mHasOpponentSnapshot ? StrFormat(__S("Opponent: %d (+%d)"), mOpponentSun, mOpponentIncome) : __S("Opponent");
	DrawCaption(g, aCaption, aRect.mY + aRect.mHeight + 12, Color(255, 255, 255));
	if (mHasOpponentSnapshot)
		TodDrawString(g, StrFormat(mOpponentBrains == 1 ? __S("%d brain") : __S("%d brains"), mOpponentBrains), aRect.mX + 4, aRect.mY + 12,
			FONT_BRIANNETOD12, Color(255, 200, 200), DrawStringJustification::DS_ALIGN_LEFT);
}

void Versus::DrawWaveInfo(Graphics* g)
{
	int aSecondsToNext = (VERSUS_FIRST_WAVE_TIME + mWaveNumber * VERSUS_WAVE_INTERVAL - mPlayTime + 99) / 100;
	SexyString aText = mWaveNumber == 0 ?
		StrFormat(__S("Zombies in %ds"), aSecondsToNext) :
		StrFormat(__S("Wave %d - next %ds"), mWaveNumber, aSecondsToNext);
	Rect aRect = GetOpponentViewRect();
	DrawCaption(g, aText, aRect.mY + aRect.mHeight + 28, Color(255, 255, 160));
	if (GetPackSize() > 1)
		DrawCaption(g, StrFormat(__S("Sends: packs of %d"), GetPackSize()), aRect.mY + aRect.mHeight + 44, Color(255, 230, 90));
}

void Versus::DrawIncome(Graphics* g)
{
	TodDrawString(g, StrFormat(mIncome >= VERSUS_MAX_INCOME ? __S("+%d max") : __S("+%d"), mIncome), 34, 100, FONT_CONTINUUMBOLD14, Color(255, 255, 160), DrawStringJustification::DS_ALIGN_CENTER);
	TodDrawString(g, StrFormat(mBrains == 1 ? __S("%d brain") : __S("%d brains"), mBrains), 6, 118, FONT_BRIANNETOD12,
		mBrains == 1 ? Color(255, 90, 90) : Color(255, 200, 200), DrawStringJustification::DS_ALIGN_LEFT);
}
