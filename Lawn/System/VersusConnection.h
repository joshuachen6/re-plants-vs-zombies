#ifndef __VERSUSCONNECTION_H__
#define __VERSUSCONNECTION_H__

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// WebSocket connection to the Versus relay server (server/relay.js). Only the web build has one;
// elsewhere IsSupported() is false and Connect() fails.
//
// Socket callbacks arrive from the browser while the game is suspended between frames, so they only
// queue events. The game drains them with Poll() from its own update.
class VersusConnection
{
public:
	enum EventType
	{
		EVENT_OPEN,
		EVENT_CLOSE,
		EVENT_TEXT,
		EVENT_BINARY
	};

	struct Event
	{
		EventType				mType;
		std::string				mData;
	};

public:
	VersusConnection();
	~VersusConnection();

	static bool					IsSupported();
	static std::string			GetServerUrl();

	bool						Connect(const std::string& theUrl);
	void						Close();
	bool						IsOpen() const { return mOpen; }
	bool						IsConnecting() const { return mConnecting; }
	void						SendText(const std::string& theText);
	void						SendBinary(const std::vector<uint8_t>& theData);
	bool						Poll(Event& theEvent);

	// Called from the socket callbacks
	void						PushEvent(EventType theType, const char* theData, size_t theLength);
	void						SetOpen(bool theOpen) { mOpen = theOpen; mConnecting = false; }

private:
	int							mSocket;
	bool						mOpen;
	bool						mConnecting;
	std::deque<Event>			mEvents;
};

#endif
