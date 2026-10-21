#ifndef __VERSUS_H__
#define __VERSUS_H__

#include <cstdint>
#include <string>
#include <vector>

#include "../../ConstEnums.h"
#include "misc/Rect.h"
#include "VersusConnection.h"

class LawnApp;
class Board;
class ZombieChooserScreen;
namespace Sexy
{
	class Graphics;
}

// Versus mode: two players defend their own lawns and send zombies at each other, like Bloons TD
// Battles. Each game simulates only its own board; the relay server (server/relay.js) passes along
// zombies sent, a 10/s snapshot of each board for the opponent view, and the result.

// ---- Tuning -------------------------------------------------------------------------------------
constexpr int VERSUS_NUM_PLANT_PACKETS = 6;
constexpr int VERSUS_NUM_ZOMBIE_PACKETS = 4;			// picked from gVersusZombieOffers before the match
constexpr int VERSUS_NUM_ZOMBIE_OFFERS = 15;
constexpr int VERSUS_STARTING_SUN = 250;			// a sunflower, a peashooter and a little spare
constexpr int VERSUS_STARTING_INCOME = 25;			// sun paid out every VERSUS_INCOME_INTERVAL
constexpr int VERSUS_INCOME_INTERVAL = 600;			// updates (100 per second)
constexpr int VERSUS_MAX_INCOME = 250;				// income tapers off towards this, so late-game sun doesn't run away
constexpr int VERSUS_INCOME_PER_SEND_DIVISOR = 8;	// sending a zombie raises income by up to cost / this, which pays back
													// about as fast as a sunflower (50 sun for 25 every ~24 s)
constexpr int VERSUS_SNAPSHOT_INTERVAL = 10;		// updates between board snapshots

// Natural waves: both lawns get the same zombies at the same time, generated from the seed the
// server hands out at the start of the match (like the rounds in Bloons TD Battles)
constexpr int VERSUS_FIRST_WAVE_TIME = 2500;		// updates after "Ready, Set, Plant!"
constexpr int VERSUS_WAVE_INTERVAL = 2500;			// updates between waves
constexpr int VERSUS_HUGE_WAVE_EVERY = 5;			// every Nth wave is twice the size
constexpr int VERSUS_MAX_ZOMBIES_PER_WAVE = 30;

// Later in the match, each send is a pack of zombies at a multiple of the price: a sun sink for
// the late game, and it keeps the pressure rising on both sides
struct VersusPackStep
{
	int						mFromWave;
	int						mSize;
};
constexpr VersusPackStep VERSUS_PACK_STEPS[] = { { 10, 2 }, { 18, 3 }, { 26, 4 } };

// Connection: if nothing arrives from the server for a while (it answers our keepalive pings), the
// connection is treated as lost. Both games then pause while this one tries to rejoin; the server
// holds our spot for 60 s.
constexpr int VERSUS_KEEPALIVE_MS = 2000;
constexpr int VERSUS_SILENCE_TIMEOUT_MS = 6000;
constexpr int VERSUS_RECONNECT_INTERVAL_MS = 2000;
constexpr int VERSUS_CONNECT_TIMEOUT_MS = 5000;
constexpr int VERSUS_RECONNECT_GIVE_UP_MS = 55000;

// Maps the room's creator can pick. The order is part of the relay protocol and matches the
// survival thumbnails (IMAGE_SURVIVAL_THUMBNAILS).
enum VersusMap
{
	VERSUS_MAP_DAY,
	VERSUS_MAP_NIGHT,
	VERSUS_MAP_POOL,
	VERSUS_MAP_FOG,
	VERSUS_MAP_ROOF,
	NUM_VERSUS_MAPS
};

struct VersusMapInfo
{
	const char*				mName;
	BackgroundType			mBackground;
};

extern const VersusMapInfo gVersusMaps[NUM_VERSUS_MAPS];

struct VersusZombieOffer
{
	SeedType				mSeedType;
	int						mCost;					// sun, replacing the I, Zombie puzzle prices
	int						mRefreshTime;			// cooldown in updates, like Plant::GetRefreshTime
	int						mUnlockWave;			// can only be sent once this many natural waves have come
};

extern const VersusZombieOffer gVersusZombieOffers[VERSUS_NUM_ZOMBIE_OFFERS];

// ---- Session ------------------------------------------------------------------------------------
enum VersusState
{
	VERSUS_STATE_OFF,
	VERSUS_STATE_CONNECTING,
	VERSUS_STATE_IN_LOBBY,							// waiting for an opponent to join
	VERSUS_STATE_CHOOSING_SEEDS,
	VERSUS_STATE_PLAYING,
	VERSUS_STATE_OVER
};

enum VersusResult
{
	VERSUS_RESULT_NONE,
	VERSUS_RESULT_WON,
	VERSUS_RESULT_LOST,
	VERSUS_RESULT_OPPONENT_LEFT,
	VERSUS_RESULT_DISCONNECTED
};

struct VersusRemotePlant
{
	int16_t					mX;
	int16_t					mY;
	SeedType				mSeedType;
};

struct VersusPendingZombie
{
	ZombieType				mZombieType;
	int						mRow;
};

struct VersusRemoteZombie
{
	uint16_t				mId;
	ZombieType				mZombieType;
	uint8_t					mFlags;
	float					mX, mY;					// drawn position, eased towards the target
	float					mTargetX, mTargetY;
};

class Versus
{
public:
	LawnApp*				mApp;
	VersusConnection		mConnection;
	VersusState				mState;
	VersusResult			mResult;
	std::string				mStatus;				// shown in the lobby dialog
	std::string				mJoinCode;				// empty when creating a room
	std::string				mRoomCode;
	std::string				mToken;					// lets us rejoin the room after a dropped connection
	uint32_t				mSeed;					// shared by both players, for the natural waves and gravestones
	VersusMap				mMap;					// picked by the room's creator
	int						mPlayTime;				// updates since the match started
	int						mWaveNumber;			// natural waves spawned so far
	bool					mOpponentReady;
	bool					mGo;					// both players have picked seeds
	bool					mZombiePacketsAdded;	// the seed bank has grown to hold the zombie packets
	bool					mZombiesChosen;
	bool					mShowingWaitAdvice;
	ZombieChooserScreen*	mZombieChooser;
	int						mIncome;
	int						mIncomeCounter;
	int						mSnapshotCounter;

	bool					mHasOpponentSnapshot;
	int						mOpponentSun;
	int						mOpponentIncome;
	uint8_t					mOpponentMowers;		// bit per row with a lawnmower left
	std::vector<VersusRemotePlant>	mOpponentPlants;
	std::vector<VersusRemoteZombie>	mOpponentZombies;
	std::vector<VersusPendingZombie>	mPendingZombies;	// sent before our level started

	// Zombies sent and "I lost" are numbered, kept, and resent after a reconnect if the opponent
	// says it missed them
	int						mNextSendSeq;
	int						mLastReceivedSeq;
	std::vector<std::vector<uint8_t>>	mSentReliable;

	bool					mReconnecting;
	bool					mOpponentDisconnected;
	uint32_t				mReconnectGiveUpTime;
	uint32_t				mNextConnectTime;
	uint32_t				mConnectStartTime;
	uint32_t				mLastReceiveTime;
	uint32_t				mLastKeepaliveTime;
	int						mConnectionDialog;		// which connection message is showing, if any
	bool					mKeepRunningHidden;

public:
	Versus(LawnApp* theApp);

	void					StartMatchmaking(const std::string& theJoinCode, VersusMap theMap);
	void					Cancel();				// leave the room and close the connection
	void					Update();
	bool					IsMatchmaking() const { return mState == VERSUS_STATE_CONNECTING || mState == VERSUS_STATE_IN_LOBBY; }
	bool					IsInMatch() const { return mState == VERSUS_STATE_CHOOSING_SEEDS || mState == VERSUS_STATE_PLAYING; }
	bool					IsConnectionPaused() const { return IsInMatch() && (mReconnecting || mOpponentDisconnected); }

	// Hooks from the board and seed chooser
	int						GetNumSeedsInBank() const;
	void					OnSeedsChosen(Board* theBoard);
	void					UpdateWaitingToStart(Board* theBoard);
	void					OnZombiesChosen(const std::vector<SeedType>& theZombieSeeds);
	bool					CanStartLevel() const { return mGo; }
	void					SendZombie(SeedType theSeedType, int theRow);
	void					OnZombiesWon();
	void					ShowResult();
	static int				GetZombieRefreshTime(SeedType theSeedType);
	static int				GetZombieCost(SeedType theSeedType);	// for one zombie
	int						GetPackSize() const;
	int						GetSendCost(SeedType theSeedType) const { return GetZombieCost(theSeedType) * GetPackSize(); }
	int						GetIncomeGain(int theCost) const;
	static int				GetZombieUnlockWave(SeedType theSeedType);
	bool					IsZombieUnlocked(SeedType theSeedType) const { return mWaveNumber >= GetZombieUnlockWave(theSeedType); }
	void					DrawZombiePacketOverlays(Sexy::Graphics* g);
	bool					IsZombieAllowedOnMap(ZombieType theZombieType) const;
	static int				PickLane(Board* theBoard, ZombieType theZombieType, int theRow);

	// Opponent view, drawn in the top bar
	static Sexy::Rect		GetOpponentViewRect();
	static int				OpponentViewRowAt(int theX, int theY);
	void					DrawOpponentView(Sexy::Graphics* g);
	void					DrawIncome(Sexy::Graphics* g);
	void					DrawWaveInfo(Sexy::Graphics* g);

private:
	void					ResetMatch();
	void					RemoveZombieChooser();
	void					HandleControl(const std::string& theMessage);
	void					HandleGameData(const std::string& theData);
	void					HandleDisconnect();
	void					BeginReconnect();
	void					UpdateConnection();
	void					UpdateConnectionDialog();
	void					SpawnZombie(Board* theBoard, ZombieType theZombieType, int theRow);
	void					SendReliable(std::vector<uint8_t> theMessage);
	void					SendResume();
	void					EndMatch(VersusResult theResult);
	void					SendSnapshot(Board* theBoard);
	void					ReadSnapshot(const std::string& theData);
	void					UpdateNaturalWaves(Board* theBoard);
	void					SpawnNaturalWave(Board* theBoard, int theWave);
};

#endif
