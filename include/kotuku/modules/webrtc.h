#pragma once

// Name:      webrtc.h
// Copyright: Paul Manias © 2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_WEBRTC (1)

class objWebChannel;
class objPeerConnection;

// PeerConnection transport states.

enum class RTCS : int {
   NIL = 0,
   NEW = 0,
   CONNECTING = 1,
   CONNECTED = 2,
   DISCONNECTED = 3,
   FAILED = 4,
   CLOSED = 5,
};

// PeerConnection signalling states.

enum class RTCSIG : int {
   NIL = 0,
   STABLE = 0,
   HAVE_LOCAL_OFFER = 1,
   HAVE_REMOTE_OFFER = 2,
   CLOSED = 3,
};

// ICE agent states.

enum class RTCICE : int {
   NIL = 0,
   NEW = 0,
   CHECKING = 1,
   CONNECTED = 2,
   COMPLETED = 3,
   DISCONNECTED = 4,
   FAILED = 5,
};

// ICE gathering states.

enum class RTCGATHER : int {
   NIL = 0,
   NEW = 0,
   GATHERING = 1,
   COMPLETE = 2,
};

// PeerConnection ICE policy.

enum class RTCF : uint32_t {
   NIL = 0,
   RELAY_ONLY = 0x00000001,
   NO_HOST = 0x00000002,
   NO_MDNS = 0x00000004,
   DISABLE_IPV6 = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(RTCF)

// WebChannel lifecycle states.

enum class WCS : int {
   NIL = 0,
   CONNECTING = 0,
   OPEN = 1,
   CLOSING = 2,
   CLOSED = 3,
};

// WebChannel complete-message types.

enum class WCM : int {
   NIL = 0,
   TEXT = 1,
   BINARY = 2,
};

// WebChannel class definition

#define VER_WEBCHANNEL (1.000000)

// WebChannel methods

namespace wch {
struct Send { std::span<const int8_t> Data; WCM Type; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Close { static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objWebChannel : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::WEBCHANNEL;
   static constexpr CSTRING CLASS_NAME = "WebChannel";

   using create = kt::Create<objWebChannel>;
   objWebChannel(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   std::string Label;                     // The name of the channel.
   std::string Protocol;                  // The name of the sub-protocol used by the channel.
   int64_t BufferedAmountLowThreshold;    // The BufferedAmount level, in bytes, at which OnBufferedAmountLow is called.
   int     Negotiated;                    // True if the channel was negotiated by the client rather than announced in-band.
   int     Ordered;                       // True if messages are delivered in the order that they were sent.
   int     MaxRetransmits;                // The maximum number of times an outgoing message is retransmitted.
   int     MaxPacketLifeTime;             // The maximum lifetime of an outgoing message, in milliseconds.
   WCS     State;                         // The current state of the channel.
   ERR     Error;                         // The error that caused the channel to fail.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR send(std::span<const int8_t> Data, WCM Type) noexcept {
      struct wch::Send args = { Data, Type };
      return Action(AC(-1), this, &args);
   }
   inline ERR close() noexcept {
      return Action(AC(-2), this, nullptr);
   }

   // Customised field getting

   inline ERR getLabel(std::string_view &Value) noexcept {
      Value = this->Label;
      return ERR::Okay;
   }

   inline ERR getProtocol(std::string_view &Value) noexcept {
      Value = this->Protocol;
      return ERR::Okay;
   }

   inline ERR getBufferedAmountLowThreshold(int64_t &Value) noexcept {
      Value = this->BufferedAmountLowThreshold;
      return ERR::Okay;
   }

   inline ERR getNegotiated(int &Value) noexcept {
      Value = this->Negotiated;
      return ERR::Okay;
   }

   inline ERR getOrdered(int &Value) noexcept {
      Value = this->Ordered;
      return ERR::Okay;
   }

   inline ERR getMaxRetransmits(int &Value) noexcept {
      Value = this->MaxRetransmits;
      return ERR::Okay;
   }

   inline ERR getMaxPacketLifeTime(int &Value) noexcept {
      Value = this->MaxPacketLifeTime;
      return ERR::Okay;
   }

   inline ERR getState(WCS &Value) noexcept {
      Value = this->State;
      return ERR::Okay;
   }

   inline ERR getError(ERR &Value) noexcept {
      Value = this->Error;
      return ERR::Okay;
   }

   inline ERR getStream(int &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getMaxMessageSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getBufferedAmount(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOnOpen(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOnClose(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOnError(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOnMessage(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOnBufferedAmountLow(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setBufferedAmountLowThreshold(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setOnOpen(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOnClose(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOnError(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOnMessage(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOnBufferedAmountLow(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};

// PeerConnection class definition

#define VER_PEERCONNECTION (1.000000)

// PeerConnection methods

namespace rtc {
struct CreateOffer { static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CreateAnswer { static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetLocalDescription { std::string_view Description; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetRemoteDescription { std::string_view Description; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddIceCandidate { std::string_view Candidate; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Close { static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RestartIce { static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CreateWebChannel { std::string_view Label; std::string_view Protocol; int Ordered; int Negotiated; int ID; int MaxRetransmits; int MaxPacketLifeTime; objWebChannel *Channel; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetStats { STRING Stats; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objPeerConnection : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::PEERCONNECTION;
   static constexpr CSTRING CLASS_NAME = "PeerConnection";

   using create = kt::Create<objPeerConnection>;
   objPeerConnection(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   std::string LocalDescription;          // The local session description.
   std::string RemoteDescription;         // The session description received from the remote peer.
   std::string CertificateFingerprint;    // The SHA-256 fingerprint of the local DTLS certificate.
   std::string StunServer;                // The address of a STUN server for discovering the public address of the host.
   APTR      ClientData;                  // A custom pointer available for userspace.
   RTCS      State;                       // The overall state of the connection.
   RTCSIG    SignalingState;              // The state of the offer and answer exchange.
   RTCICE    IceState;                    // The state of the ICE agent.
   RTCGATHER GatheringState;              // The state of local candidate gathering.
   ERR       Error;                       // The most recent error reported by the transport.
   int       ReceiveLimit;                // The largest message, in bytes, that the connection will accept from the remote peer.
   RTCF      Flags;                       // Optional flags that control how candidates are gathered and signalled.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR createOffer() noexcept {
      return Action(AC(-1), this, nullptr);
   }
   inline ERR createAnswer() noexcept {
      return Action(AC(-2), this, nullptr);
   }
   inline ERR setLocalDescription(const std::string_view &Description) noexcept {
      struct rtc::SetLocalDescription args = { Description };
      return Action(AC(-3), this, &args);
   }
   inline ERR setRemoteDescription(const std::string_view &Description) noexcept {
      struct rtc::SetRemoteDescription args = { Description };
      return Action(AC(-4), this, &args);
   }
   inline ERR addIceCandidate(const std::string_view &Candidate) noexcept {
      struct rtc::AddIceCandidate args = { Candidate };
      return Action(AC(-5), this, &args);
   }
   inline ERR close() noexcept {
      return Action(AC(-6), this, nullptr);
   }
   inline ERR restartIce() noexcept {
      return Action(AC(-7), this, nullptr);
   }
   inline ERR createWebChannel(const std::string_view &Label, const std::string_view &Protocol, int Ordered, int Negotiated, int ID, int MaxRetransmits, int MaxPacketLifeTime, objWebChannel ** Channel) noexcept {
      struct rtc::CreateWebChannel args = { Label, Protocol, Ordered, Negotiated, ID, MaxRetransmits, MaxPacketLifeTime, (objWebChannel *)0 };
      ERR error = Action(AC(-8), this, &args);
      if (Channel) *Channel = args.Channel;
      return error;
   }
   inline ERR getStats(STRING * Stats) noexcept {
      struct rtc::GetStats args = { (STRING)0 };
      ERR error = Action(AC(-9), this, &args);
      if (Stats) *Stats = args.Stats;
      return error;
   }

   // Customised field getting

   inline ERR getLocalDescription(std::string_view &Value) noexcept {
      Value = this->LocalDescription;
      return ERR::Okay;
   }

   inline ERR getRemoteDescription(std::string_view &Value) noexcept {
      Value = this->RemoteDescription;
      return ERR::Okay;
   }

   inline ERR getCertificateFingerprint(std::string_view &Value) noexcept {
      Value = this->CertificateFingerprint;
      return ERR::Okay;
   }

   inline ERR getStunServer(std::string_view &Value) noexcept {
      Value = this->StunServer;
      return ERR::Okay;
   }

   inline ERR getClientData(APTR &Value) noexcept {
      Value = this->ClientData;
      return ERR::Okay;
   }

   inline ERR getState(RTCS &Value) noexcept {
      Value = this->State;
      return ERR::Okay;
   }

   inline ERR getSignalingState(RTCSIG &Value) noexcept {
      Value = this->SignalingState;
      return ERR::Okay;
   }

   inline ERR getIceState(RTCICE &Value) noexcept {
      Value = this->IceState;
      return ERR::Okay;
   }

   inline ERR getGatheringState(RTCGATHER &Value) noexcept {
      Value = this->GatheringState;
      return ERR::Okay;
   }

   inline ERR getRestartRequired(int &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOnRestartRequired(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getError(ERR &Value) noexcept {
      Value = this->Error;
      return ERR::Okay;
   }

   inline ERR getReceiveLimit(int &Value) noexcept {
      Value = this->ReceiveLimit;
      return ERR::Okay;
   }

   inline ERR getFlags(RTCF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getFeedback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOnIceCandidate(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOnWebChannel(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setStunServer(const std::string_view &Value) noexcept {
      this->StunServer = Value;
      return ERR::Okay;
   }

   inline ERR setClientData(APTR Value) noexcept {
      this->ClientData = Value;
      return ERR::Okay;
   }

   inline ERR setOnRestartRequired(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setReceiveLimit(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->ReceiveLimit = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const RTCF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setIceServers(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(this, field, 0x00804408, &Value);
   }

   inline ERR setFeedback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOnIceCandidate(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOnWebChannel(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};