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
namespace Sexy
{
	class Graphics;
}

// Versus mode: two players defend their own lawns and send zombies at each other, like Bloons TD
// Battles. Each game simulates only its own board; the relay server (server/relay.js) passes along
// zombies sent, a 10/s snapshot of each board for the opponent view, and the result.

// ---- Tuning -------------------------------------------------------------------------------------
constexpr int VERSUS_NUM_PLANT_PACKETS = 6;
constexpr int VERSUS_NUM_ZOMBIE_PACKETS = 4;
constexpr int VERSUS_STARTING_SUN = 150;
constexpr int VERSUS_STARTING_INCOME = 25;			// sun paid out every VERSUS_INCOME_INTERVAL
constexpr int VERSUS_INCOME_INTERVAL = 600;			// updates (100 per second)
constexpr int VERSUS_INCOME_PER_SEND_DIVISOR = 10;	// sending a zombie raises income by cost / this
constexpr int VERSUS_SNAPSHOT_INTERVAL = 10;		// updates between board snapshots

struct VersusZombieOffer
{
	SeedType				mSeedType;
	int						mRefreshTime;			// cooldown in updates, like Plant::GetRefreshTime
};

extern const VersusZombieOffer gVersusZombieOffers[VERSUS_NUM_ZOMBIE_PACKETS];

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
	bool					mOpponentReady;
	bool					mGo;					// both players have picked seeds
	bool					mZombiePacketsAdded;
	bool					mShowingWaitAdvice;
	int						mIncome;
	int						mIncomeCounter;
	int						mSnapshotCounter;

	bool					mHasOpponentSnapshot;
	int						mOpponentSun;
	int						mOpponentIncome;
	uint8_t					mOpponentMowers;		// bit per row with a lawnmower left
	std::vector<VersusRemotePlant>	mOpponentPlants;
	std::vector<VersusRemoteZombie>	mOpponentZombies;

public:
	Versus(LawnApp* theApp);

	void					StartMatchmaking(const std::string& theJoinCode);
	void					Cancel();				// leave the room and close the connection
	void					Update();
	bool					IsMatchmaking() const { return mState == VERSUS_STATE_CONNECTING || mState == VERSUS_STATE_IN_LOBBY; }

	// Hooks from the board and seed chooser
	int						GetNumSeedsInBank() const;
	void					OnSeedsChosen(Board* theBoard);
	bool					CanStartLevel() const { return mGo; }
	void					SendZombie(SeedType theSeedType, int theRow);
	void					OnZombiesWon();
	void					ShowResult();
	static int				GetZombieRefreshTime(SeedType theSeedType);

	// Opponent view, drawn in the top bar
	static Sexy::Rect		GetOpponentViewRect();
	static int				OpponentViewRowAt(int theX, int theY);
	void					DrawOpponentView(Sexy::Graphics* g);
	void					DrawIncome(Sexy::Graphics* g);

private:
	void					ResetMatch();
	void					HandleControl(const std::string& theMessage);
	void					HandleGameData(const std::string& theData);
	void					HandleDisconnect();
	void					EndMatch(VersusResult theResult);
	void					SendSnapshot(Board* theBoard);
	void					ReadSnapshot(const std::string& theData);
};

#endif
