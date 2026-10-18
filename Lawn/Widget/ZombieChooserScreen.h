#ifndef __ZOMBIECHOOSERSCREEN_H__
#define __ZOMBIECHOOSERSCREEN_H__

#include <vector>

#include "../../ConstEnums.h"
#include "widget/Widget.h"

class LawnApp;
class GameButton;

using namespace Sexy;

// Versus: after picking plants, each player picks which zombies they can send, laid out like the
// seed chooser. The picks fill the zombie slots of the seed bank; clicking one there removes it.
class ZombieChooserScreen : public Widget
{
public:
	LawnApp*				mApp;
	GameButton*				mStartButton;
	std::vector<SeedType>	mChosen;

public:
	ZombieChooserScreen(LawnApp* theApp);
	virtual ~ZombieChooserScreen();

	virtual void			Update();
	virtual void			Draw(Graphics* g);
	virtual void			MouseDown(int x, int y, int theClickCount);

private:
	static void				GetOfferPosition(int theIndex, int& x, int& y);
	int						OfferHitTest(int x, int y);
	int						BankHitTest(int x, int y);
	bool					IsChosen(SeedType theSeedType);
	void					SyncSeedBank();
};

#endif
