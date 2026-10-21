#include "VersusConnection.h"

#include <cstdlib>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/websocket.h>
#endif

VersusConnection::VersusConnection() : mSocket(0), mOpen(false), mConnecting(false)
{
}

VersusConnection::~VersusConnection()
{
	Close();
}

void VersusConnection::PushEvent(EventType theType, const char* theData, size_t theLength)
{
	Event anEvent;
	anEvent.mType = theType;
	if (theData != nullptr)
		anEvent.mData.assign(theData, theLength);
	mEvents.push_back(anEvent);
}

bool VersusConnection::Poll(Event& theEvent)
{
	if (mEvents.empty())
		return false;

	theEvent = mEvents.front();
	mEvents.pop_front();
	return true;
}

#ifdef __EMSCRIPTEN__

// web/shell.html sets Module.pvzServerUrl: the page's ?server= parameter, or else /versus on the server
// the page came from (a reverse proxy in front of server/relay.js, or the relay itself with --static)
EM_JS(char*, GetServerUrlJS, (), {
	var url = Module.pvzServerUrl || ((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/versus');
	return stringToNewUTF8(url);
});

static EM_BOOL OnSocketOpen(int, const EmscriptenWebSocketOpenEvent*, void* theUserData)
{
	VersusConnection* aConnection = (VersusConnection*)theUserData;
	aConnection->SetOpen(true);
	aConnection->PushEvent(VersusConnection::EVENT_OPEN, nullptr, 0);
	return EM_TRUE;
}

static EM_BOOL OnSocketMessage(int, const EmscriptenWebSocketMessageEvent* theEvent, void* theUserData)
{
	VersusConnection* aConnection = (VersusConnection*)theUserData;
	if (theEvent->isText)
	{
		// numBytes counts the null terminator for text messages
		const char* aText = (const char*)theEvent->data;
		aConnection->PushEvent(VersusConnection::EVENT_TEXT, aText, strnlen(aText, theEvent->numBytes));
	}
	else
	{
		aConnection->PushEvent(VersusConnection::EVENT_BINARY, (const char*)theEvent->data, theEvent->numBytes);
	}
	return EM_TRUE;
}

static EM_BOOL OnSocketClose(int, const EmscriptenWebSocketCloseEvent*, void* theUserData)
{
	// Also fires when the connection could not be made at all
	VersusConnection* aConnection = (VersusConnection*)theUserData;
	aConnection->SetOpen(false);
	aConnection->PushEvent(VersusConnection::EVENT_CLOSE, nullptr, 0);
	return EM_TRUE;
}

bool VersusConnection::IsSupported()
{
	return emscripten_websocket_is_supported();
}

std::string VersusConnection::GetServerUrl()
{
	char* aUrl = GetServerUrlJS();
	std::string aResult = aUrl;
	free(aUrl);
	return aResult;
}

bool VersusConnection::Connect(const std::string& theUrl)
{
	Close();

	EmscriptenWebSocketCreateAttributes anAttributes;
	emscripten_websocket_init_create_attributes(&anAttributes);
	anAttributes.url = theUrl.c_str();

	EMSCRIPTEN_WEBSOCKET_T aSocket = emscripten_websocket_new(&anAttributes);
	if (aSocket <= 0)
		return false;

	mSocket = aSocket;
	mConnecting = true;
	emscripten_websocket_set_onopen_callback(aSocket, this, OnSocketOpen);
	emscripten_websocket_set_onmessage_callback(aSocket, this, OnSocketMessage);
	emscripten_websocket_set_onclose_callback(aSocket, this, OnSocketClose);
	return true;
}

void VersusConnection::Close()
{
	if (mSocket > 0)
	{
		// Unhook first so a late close event can't reach a connection that has moved on
		emscripten_websocket_set_onopen_callback(mSocket, nullptr, nullptr);
		emscripten_websocket_set_onmessage_callback(mSocket, nullptr, nullptr);
		emscripten_websocket_set_onclose_callback(mSocket, nullptr, nullptr);
		emscripten_websocket_close(mSocket, 1000, "");
		emscripten_websocket_delete(mSocket);
		mSocket = 0;
	}
	mOpen = false;
	mConnecting = false;
	mEvents.clear();
}

void VersusConnection::SendText(const std::string& theText)
{
	if (mOpen)
		emscripten_websocket_send_utf8_text(mSocket, theText.c_str());
}

void VersusConnection::SendBinary(const std::vector<uint8_t>& theData)
{
	if (mOpen && !theData.empty())
		emscripten_websocket_send_binary(mSocket, (void*)theData.data(), theData.size());
}

#else // !__EMSCRIPTEN__

bool VersusConnection::IsSupported()
{
	return false;
}

std::string VersusConnection::GetServerUrl()
{
	return "";
}

bool VersusConnection::Connect(const std::string&)
{
	return false;
}

void VersusConnection::Close()
{
	mOpen = false;
	mConnecting = false;
	mEvents.clear();
}

void VersusConnection::SendText(const std::string&)
{
}

void VersusConnection::SendBinary(const std::vector<uint8_t>&)
{
}

#endif
