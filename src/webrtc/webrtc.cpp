/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-MODULE-
WebRTC: Establishes native peer connections over ICE and DTLS.

The WebRTC module is a host for the @PeerConnection and @WebChannel classes.

-END-

*********************************************************************************************************************/

// Windows uses native Schannel DTLS and CNG by default; Linux uses OpenSSL.

#include <kotuku/main.h>
#include <kotuku/modules/network.h>
#include <kotuku/modules/webrtc.h>
#include <kotuku/modules/module.h>
#include <kotuku/modules/script.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "peerconnection/peer_engine.h"

#ifdef _WIN32
uint32_t webrtc_windows_build();
#endif

class extWebChannel;

class extPeerConnection : public objPeerConnection {
public:
   std::unique_ptr<rtc::PeerConnection> Connection; // Object-independent protocol engine
   RTCS ReportedState = RTCS::NEW;                  // Last State reported to Feedback
   RTCICE ReportedIceState = RTCICE::NEW;           // Last IceState reported to Feedback
   FUNCTION Feedback;
   FUNCTION OnIceCandidate;
   FUNCTION OnWebChannel;
   FUNCTION OnRestartRequired;
   uint64_t ReportedRestartGeneration = 0;
   uint64_t DescriptionRevision = 0;                // Engine description revision copied to the public fields
   std::map<uint64_t, extWebChannel *> Channels;    // Child channels, keyed by engine handle
   std::string ServerConfiguration;                 // IceServers JSON, held until Init
   bool Dispatching = false;                        // True while channel events are being delivered
   APTR TimerHandle = nullptr;
   std::deque<std::pair<uint64_t, std::string>> PendingCandidates; // Candidate generation and line

   extPeerConnection(objMetaClass *ClassPtr, OBJECTID ObjectID) : objPeerConnection(ClassPtr, ObjectID) {
      State = RTCS::NEW;
      SignalingState = RTCSIG::STABLE;
      IceState = RTCICE::NEW;
      GatheringState = RTCGATHER::NEW;
      Error = ERR::Okay;
      ClientData = nullptr;
      ReceiveLimit = 262144;
      Flags = RTCF::NIL;
   }

   ~extPeerConnection();
};

//********************************************************************************************************************

class extWebChannel : public objWebChannel {
public:
   extPeerConnection *Parent = nullptr; // Cleared if the parent is destroyed first
   uint64_t Handle = 0;                 // Engine channel handle
   int StreamID = -1;
   int64_t BufferedAmount = 0, MaxMessageSize = 0;
   FUNCTION OnOpen, OnMessage, OnClose, OnError, OnBufferedAmountLow;
   bool OpenReported = false, CloseReported = false, ErrorReported = false;

   extWebChannel(objMetaClass *ClassPtr, OBJECTID ObjectID) : objWebChannel(ClassPtr, ObjectID) {
      StreamID = -1;
      State = WCS::CONNECTING;
      Error = ERR::Okay;
      BufferedAmount = 0;
      BufferedAmountLowThreshold = 0;
      MaxMessageSize = 0;
      Negotiated = 0;
      Ordered = 1;
      MaxRetransmits = MaxPacketLifeTime = -1;
   }

   ~extWebChannel();
};

//********************************************************************************************************************

#include "module_def.c"

JUMPTABLE_CORE
JUMPTABLE_NETWORK

//********************************************************************************************************************

static OBJECTPTR modNetwork = nullptr;
static OBJECTPTR clPeerConnection = nullptr;
static OBJECTPTR clWebChannel = nullptr;
static MSGID glRtcFreeMsgID = MSGID::NIL;
static MsgHandler *glRtcFreeHandle = nullptr;

static ERR add_peer_connection_class();
static ERR add_web_channel_class();

//********************************************************************************************************************
// Objects that are marked for collection while pinned by a callback are freed from a deferred message, so that the
// release never occurs inside the caller's stack frame.

static ERR rtc_free_message(APTR, int, MSGID, std::span<std::byte> Message)
{
   if (Message.size() IS sizeof(OBJECTID)) {
      OBJECTID id;
      std::memcpy(&id, Message.data(), sizeof(id));
      FreeResource(id);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Releases a pin taken for a callback.  If the object was marked for collection while pinned and this was the last
// pin, the free is deferred to rtc_free_message().

static void rtc_release_pin(Object *Target)
{
   bool collect = Target->collecting() and not Target->defined(NF::FREE);
   Target->unpin();
   if (collect and not Target->isPinned()) {
      auto id = Target->UID;
      (void)SendMessage(glRtcFreeMsgID, MSF::NIL, std::span((const int8_t *)&id, sizeof(id)));
   }
}

//********************************************************************************************************************
// Callback field bookkeeping.  A callback field holds a weak pin on its target while it is set, so that stale
// callbacks can be detected and discarded before use.

static void rtc_clear_callback(FUNCTION &Field)
{
   if (not Field.defined()) return;
   Field.unpin();
   Field.clear();
}

// Replaces a callback field, releasing the pin held on the previous value.  A null Value clears the field.

static ERR rtc_set_callback(FUNCTION &Field, const FUNCTION *Value)
{
   rtc_clear_callback(Field);

   if (Value) {
      Field = *Value;
      Field.pin();
   }

   return ERR::Okay;
}

// Discards a callback whose target has been destroyed.  Returns true if the field holds a usable callback.

static bool rtc_live_callback(FUNCTION &Field)
{
   if (Field.stale()) {
      Field.unpin();
      Field.clear();
   }

   return Field.defined();
}

// Pins a copy of a callback for the duration of a call, so that the client can replace or clear the field from within
// the callback.  The owning object's pin is managed separately with rtc_release_pin().

class RetainedCallback {
public:
   FUNCTION function;

   explicit RetainedCallback(const FUNCTION &Field) : function(Field) { function.pin(); }
   ~RetainedCallback() { function.unpin(); }
   RetainedCallback(const RetainedCallback &) = delete;
   RetainedCallback & operator=(const RetainedCallback &) = delete;
};

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR, struct CoreBase *Core)
{
   CoreBase = Core;

#ifdef _WIN32
   auto windows_build = webrtc_windows_build();
   if (windows_build < 17763) {
      kt::Log log("Init");
      log.warning(
         "WebRTC requires Windows 10 1809 / Server 2019 (build 17763) or newer; detected build %u.", windows_build);
      return ERR::NoSupport;
   }
#endif

   if (objModule::load("network", &modNetwork, &NetworkBase) != ERR::Okay) return ERR::InitModule;

   glRtcFreeMsgID = MSGID(AllocateID(IDTYPE::MESSAGE));
   auto handler = C_FUNCTION(rtc_free_message);
   if (AddMsgHandler(glRtcFreeMsgID, &handler, &glRtcFreeHandle) != ERR::Okay) return ERR::InitModule;

   auto error = add_web_channel_class();
   return error IS ERR::Okay ? add_peer_connection_class() : error;
}

//********************************************************************************************************************

static ERR MODExpunge()
{
   if (clPeerConnection) { FreeResource(clPeerConnection); clPeerConnection = nullptr; }
   if (clWebChannel)     { FreeResource(clWebChannel); clWebChannel = nullptr; }
   if (glRtcFreeHandle)  { FreeResource(glRtcFreeHandle); glRtcFreeHandle = nullptr; }
   if (modNetwork)       { FreeResource(modNetwork); modNetwork = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

#include "webchannel/webchannel.cpp"
#include "peerconnection/peerconnection.cpp"

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_webrtc_module() { return &ModHeader; }
