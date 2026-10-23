#ifndef __VERSUS_H__
#define __VERSUS_H__

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../ConstEnums.h"
#include "misc/Rect.h"
#include "VersusConnection.h"

class LawnApp;
class Board;
class Plant;
class Zombie;
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
constexpr int VERSUS_NUM_ZOMBIE_OFFERS = 20;
constexpr int VERSUS_STARTING_SUN = 250;			// a sunflower, a peashooter and a little spare
constexpr int VERSUS_STARTING_INCOME = 25;			// sun paid out every VERSUS_INCOME_INTERVAL
constexpr int VERSUS_INCOME_INTERVAL = 600;			// updates (100 per second)
constexpr int VERSUS_MAX_INCOME = 400;				// income tapers off towards this, so late-game sun doesn't run away
constexpr int VERSUS_INCOME_PER_SEND_DIVISOR = 6;	// sending a zombie raises income by up to cost / this, which pays back
													// about as fast as a sunflower (50 sun for 25 every ~24 s)
constexpr int VERSUS_SNAPSHOT_INTERVAL = 10;		// updates between board snapshots

// Natural waves: both lawns get the same zombies at the same time, generated from the seed the
// server hands out at the start of the match (like the rounds in Bloons TD Battles)
constexpr int VERSUS_FIRST_WAVE_TIME = 2500;		// updates after "Ready, Set, Plant!"
constexpr int VERSUS_WAVE_INTERVAL = 2500;			// updates between waves
constexpr int VERSUS_HUGE_WAVE_EVERY = 5;			// every Nth wave is twice the size
constexpr int VERSUS_MAX_ZOMBIES_PER_WAVE = 30;
constexpr int VERSUS_ZOMBIE_SPACING = 150;			// px between zombies spawned in the same lane, so one
													// squash or cherry bomb can't take out a whole pack (zombies
													// walk at slightly different speeds, so the gaps shrink a bit)

// Lawn events: from VERSUS_EVENT_FIRST_WAVE, every VERSUS_EVENT_EVERY waves a random event (rolled from
// the shared seed, so both players get the same one) changes the rules for both lawns for a while
enum VersusEvent
{
	VERSUS_EVENT_NONE = -1,
	VERSUS_EVENT_SUN_SHOWER,
	VERSUS_EVENT_GOLD_RUSH,
	VERSUS_EVENT_ECO_BOOM,
	VERSUS_EVENT_ZOMBIE_SALE,
	VERSUS_EVENT_PLANT_SALE,
	VERSUS_EVENT_RAPID_RECHARGE,
	VERSUS_EVENT_ZOMBIE_RAGE,
	NUM_VERSUS_EVENTS
};
constexpr int VERSUS_EVENT_FIRST_WAVE = 3;
constexpr int VERSUS_EVENT_EVERY = 3;
constexpr int VERSUS_EVENT_DURATION = 4000;			// updates (40 s)
constexpr int VERSUS_SUN_SHOWER_INTERVAL = 200;		// a sun falls this often during a sun shower
constexpr int VERSUS_SALE_PERCENT = 30;				// discount during zombie and garden sales
constexpr int VERSUS_RAGE_PERCENT = 50;				// extra health of sent zombies during zombie rage

// Brains work like lives: a zombie that gets past the lawnmowers eats one instead of ending the match
constexpr int VERSUS_STARTING_BRAINS = 3;

// Bounties: killing a zombie the opponent sent drops sun worth a share of what they paid for it, so
// spamming zombies into a strong defence feeds the defender. Natural-wave zombies drop a small sun.
constexpr int VERSUS_BOUNTY_DIVISOR = 4;

// Upgrade plants unlock as the natural waves go by, like the zombies do
struct VersusPlantUnlock
{
	SeedType				mSeedType;
	int						mUnlockWave;
};
constexpr VersusPlantUnlock VERSUS_PLANT_UNLOCKS[] = {
	{ SeedType::SEED_TWINSUNFLOWER,	4 },
	{ SeedType::SEED_GATLINGPEA,	6 },
	{ SeedType::SEED_GOLD_MAGNET,	6 },
	{ SeedType::SEED_GLOOMSHROOM,	8 },
	{ SeedType::SEED_CATTAIL,		8 },
	{ SeedType::SEED_SPIKEROCK,		8 },
	{ SeedType::SEED_WINTERMELON,	10 },
	{ SeedType::SEED_COBCANNON,		12 },
};

// Sun producers work like farms in Bloons TD Battles: pricier than usual, and their sun drops grow as
// they ripen, so they lose to eco early but out-earn it late (if they survive)
constexpr int VERSUS_SUNFLOWER_COST = 125;
constexpr int VERSUS_TWINSUNFLOWER_COST = 250;
constexpr int VERSUS_SUNSHROOM_COST = 75;
constexpr int VERSUS_SUN_PRODUCTION_PERCENT = 70;	// sun producers drop sun this much of the normal time apart
constexpr int VERSUS_SKY_SUN_INTERVAL = 500;		// updates between suns falling from the sky (day maps),
constexpr int VERSUS_SKY_SUN_RANDOM = 250;			// plus up to this; the normal game starts near here and slows down
constexpr int VERSUS_FARM_RIPE_TIME = 12000;		// updates (2 min): drops become big suns (50)
constexpr int VERSUS_FARM_FULL_TIME = 24000;		// updates (4 min): drops become a big sun and a sun (75)

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
constexpr int VERSUS_SILENCE_TIMEOUT_MS = 15000;
// The server owns the match clock and sends it 10 times a second. A game never runs its lawn past the
// clock (plus this many updates of extrapolation between messages), and one that has fallen behind
// fast-forwards, so both lawns stay on the same match time whatever happens to either connection.
constexpr int VERSUS_CLOCK_LEAD = 30;
constexpr int VERSUS_CATCH_UP_THRESHOLD = 20;		// updates behind before fast-forwarding
constexpr int VERSUS_MAX_UPDATES_PER_FRAME = 8;		// fast-forward speed
constexpr int VERSUS_WAITING_NOTICE_MS = 1000;		// held this long, the board says it's waiting		// long enough to ride out a stalled tab or a wifi hiccup
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
	int						mIncomeLoss;			// if set, sending lowers income by this instead of raising it,
													// like MOAB-class sends in Bloons TD Battles
};

// Cooldowns of plants (and the zombie table's) are this fraction of the normal game's, so the match is
// about what you buy and when rather than waiting on recharges
constexpr int VERSUS_PLANT_COOLDOWN_PERCENT = 50;

// Sends get more dangerous as the match goes on: from VERSUS_TOUGHEN_FROM_WAVE every sent zombie has
// VERSUS_TOUGHEN_PERCENT_PER_WAVE more health per wave (up to double), and from
// VERSUS_GIGA_FROM_WAVE a sent Gargantuar arrives as a Giga-Gargantuar
constexpr int VERSUS_TOUGHEN_FROM_WAVE = 10;
constexpr int VERSUS_TOUGHEN_PERCENT_PER_WAVE = 8;
constexpr int VERSUS_TOUGHEN_MAX_PERCENT = 100;
constexpr int VERSUS_GIGA_FROM_WAVE = 14;

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
	int						mTime;					// match time it was sent at; it arrives then on our lawn too
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
	VersusEvent				mEvent;					// the lawn event in effect, if any
	VersusEvent				mLastEvent;
	int						mEventEndTime;			// mPlayTime it ends at
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
	std::unordered_map<unsigned int, int>	mFarmFirstSun;	// sun producer id -> mPlayTime of its first drop
	std::unordered_map<unsigned int, int>	mBounties;		// zombie id -> sun dropped when it's killed
	int						mBrains;
	int						mOpponentBrains;

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
	int						mClockAt;				// last match time from the server, in updates
	bool					mClockRunning;
	uint32_t				mClockReceivedTick;
	bool					mHold;					// our lawn has caught up with the clock and waits for it
	uint32_t				mHoldStartTick;
	bool					mSentPlaying;
	bool					mCatchingUp;
	uint32_t				mLastUpdateTick;		// for spotting a stalled game loop
	uint32_t				mMaxUpdateGap;			// longest gap between updates since the last (re)join, in ms
	const char*				mReconnectReason;		// reported to the server on rejoin, for diagnosing drops
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
	bool					IsHoldingBoard() const { return mHold; }
	int						GetUpdatesThisFrame() const;	// more than 1 while catching up with the clock
	int						GetAllowedTime() const;
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
	static int				GetFarmCost(SeedType theSeedType);		// 0 if it's not a sun producer
	void					AddFarmSun(Plant* thePlant);
	int						GetPackSize() const;
	int						GetSendCost(SeedType theSeedType) const { return ApplySale(GetZombieCost(theSeedType) * GetPackSize(), VERSUS_EVENT_ZOMBIE_SALE); }
	bool					IsEventActive(VersusEvent theEvent) const { return mEvent == theEvent; }
	int						ApplySale(int theCost, VersusEvent theSale) const;
	int						GetCooldownPercent() const { return IsEventActive(VERSUS_EVENT_RAPID_RECHARGE) ? 50 : 100; }
	int						GetIncomeGain(int theCost) const;
	int						GetIncomeChange(SeedType theSeedType) const;	// what sending one pack does to income
	int						GetToughnessPercent() const;	// extra health of sent zombies
	static int				GetZombieUnlockWave(SeedType theSeedType);
	static int				GetUnlockWave(SeedType theSeedType);	// plants or zombies; 0 if always available
	bool					IsZombieUnlocked(SeedType theSeedType) const { return mWaveNumber >= GetZombieUnlockWave(theSeedType); }
	bool					IsSeedUnlocked(SeedType theSeedType) const { return mWaveNumber >= GetUnlockWave(theSeedType); }
	bool					OnZombieReachedHouse(Zombie* theZombie);
	void					DrawZombiePacketOverlays(Sexy::Graphics* g);
	bool					IsZombieAllowedOnMap(ZombieType theZombieType) const;
	static int				PickLane(Board* theBoard, ZombieType theZombieType, int theRow);
	static Zombie*			AddSpacedZombie(Board* theBoard, ZombieType theZombieType, int theLane);

	// Opponent view, drawn in the top bar
	static Sexy::Rect		GetOpponentViewRect();
	static int				OpponentViewRowAt(int theX, int theY);
	void					DrawOpponentView(Sexy::Graphics* g);
	void					DrawIncome(Sexy::Graphics* g);
	void					DrawWaveInfo(Sexy::Graphics* g);
	void					StartEvent(Board* theBoard);
	void					UpdateEvent(Board* theBoard);
	void					UpdateMatch(Board* theBoard);

private:
	void					ResetMatch();
	void					RemoveZombieChooser();
	void					HandleControl(const std::string& theMessage);
	void					HandleGameData(const std::string& theData);
	void					HandleDisconnect();
	void					BeginReconnect(const char* theReason);
	void					UpdateConnection();
	void					UpdateConnectionDialog();
	void					SpawnZombie(Board* theBoard, ZombieType theZombieType, int theRow);
	void					SendReliable(std::vector<uint8_t> theMessage);
	void					SendResume();
	void					EndMatch(VersusResult theResult);
	void					SendSnapshot(Board* theBoard);
	void					ReadSnapshot(const std::string& theData);
	void					UpdateNaturalWaves(Board* theBoard);
	void					UpdateBounties(Board* theBoard);
	void					AddBounty(Zombie* theZombie, int theSun);
	void					SpawnNaturalWave(Board* theBoard, int theWave);
};

#endif
