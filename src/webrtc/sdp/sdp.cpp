// SDP offer/answer for a single bundled data-channel media section (RFC 8866, RFC 8841, RFC 8829).  Parsing is strict
// and bounded: any media section other than one UDP/DTLS/SCTP application section is rejected.

#include "sdp.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <utility>

namespace rtc::sdp {
namespace {

//********************************************************************************************************************
// Parses an unsigned decimal number that occupies the whole of Text.

bool number(std::string_view Text, uint64_t &Result)
{
   if (Text.empty()) return false;

   uint64_t value = 0;
   auto [end, error] = std::from_chars(Text.data(), Text.data() + Text.size(), value);
   if ((error != std::errc()) or (end != Text.data() + Text.size())) return false;
   Result = value;
   return true;
}

//********************************************************************************************************************
// Splits Text into space-separated words.  Empty words are not returned.

std::vector<std::string_view> words(std::string_view Text)
{
   std::vector<std::string_view> result;
   while (not Text.empty()) {
      auto start = Text.find_first_not_of(' ');
      if (start IS std::string_view::npos) break;
      Text.remove_prefix(start);
      auto end = Text.find(' ');
      result.push_back(Text.substr(0, end));
      if (end IS std::string_view::npos) break;
      Text.remove_prefix(end);
   }

   return result;
}

//********************************************************************************************************************
// Validates an ice-char token (RFC 8839 §5.4) of Min to Max characters, as used by ice-ufrag and ice-pwd.

bool token(std::string_view Text, size_t Min, size_t Max)
{
   if ((Text.size() < Min) or (Text.size() > Max)) return false;
   for (char ch : Text) {
      if (not ((ch >= 'A' and ch <= 'Z') or (ch >= 'a' and ch <= 'z') or
          (ch >= '0' and ch <= '9') or ch IS '+' or ch IS '/')) return false;
   }

   return true;
}

//********************************************************************************************************************
// Validates a SHA-256 fingerprint of 32 colon-separated hexadecimal bytes.

bool fingerprint(std::string_view Text)
{
   if (Text.size() != 95) return false;

   for (size_t i = 0; i < Text.size(); ++i) {
      if ((i % 3) IS 2) {
         if (Text[i] != ':') return false;
      }
      else if (not std::isxdigit((unsigned char)Text[i])) return false;
   }

   return true;
}

//********************************************************************************************************************
// Validates the syntax of a candidate attribute.  Only component 1 is permitted, because RTCP is always multiplexed.

bool valid_candidate(std::string_view Text, bool *Udp = nullptr)
{
   if (not Text.starts_with("candidate:") or Text.size() > 2048 or
       Text.find_first_of("\r\n") != std::string_view::npos) return false;

   // candidate:<foundation> <component> <transport> <priority> <address> <port> typ <type> ...

   auto fields = words(Text.substr(10));
   if (fields.size() < 8 or fields[0].empty() or fields[0].size() > 32 or
       fields[6] != "typ" or fields[4].empty()) return false;

   uint64_t component = 0, priority = 0, port = 0;
   if (not number(fields[1], component) or component != 1 or
       (fields[2] != "udp" and fields[2] != "UDP" and
        fields[2] != "tcp" and fields[2] != "TCP") or
       not number(fields[3], priority) or priority > UINT32_MAX or
       not number(fields[5], port) or port IS 0 or port > 65535) return false;

   if (fields[7] != "host" and fields[7] != "srflx" and fields[7] != "prflx" and
       fields[7] != "relay") return false;

   if (Udp) *Udp = fields[2] IS "udp" or fields[2] IS "UDP";
   return true;
}

//********************************************************************************************************************

bool setup(std::string_view Text, Setup &Result)
{
   if (Text IS "actpass") Result = Setup::ACTPASS;
   else if (Text IS "active") Result = Setup::ACTIVE;
   else if (Text IS "passive") Result = Setup::PASSIVE;
   else return false;
   return true;
}

//********************************************************************************************************************

const char * setup_name(Setup Value)
{
   switch (Value) {
      case Setup::ACTPASS: return "actpass";
      case Setup::ACTIVE: return "active";
      case Setup::PASSIVE: return "passive";
   }
   return "";
}

//********************************************************************************************************************
// Assigns a single-valued attribute, rejecting empty, oversized or repeated values.

bool set_once(std::string &Target, std::string_view Value)
{
   if (not Target.empty() or Value.empty() or Value.size() > 4096) return false;
   Target = Value;
   return true;
}

//********************************************************************************************************************
// Attributes collected at session or media level.  Media-level values take precedence when resolved.

struct Attributes {
   std::string ufrag;
   std::string pwd;
   std::string fingerprint;
   std::string setup;
   std::string mid;
   std::string sctp_port;
   std::string max_message_size;
   bool rtcp_mux = false;
   bool trickle = false;
   bool end_of_candidates = false;
   std::vector<std::string> candidates;
};

//********************************************************************************************************************
// Records a recognised attribute line.  Unrecognised attributes are ignored.

bool attribute(std::string_view Text, Attributes &Result)
{
   if (Text.starts_with("ice-ufrag:")) return set_once(Result.ufrag, Text.substr(10));
   if (Text.starts_with("ice-pwd:")) return set_once(Result.pwd, Text.substr(8));
   if (Text.starts_with("fingerprint:")) return set_once(Result.fingerprint, Text.substr(12));
   if (Text.starts_with("setup:")) return set_once(Result.setup, Text.substr(6));
   if (Text.starts_with("mid:")) return set_once(Result.mid, Text.substr(4));
   if (Text.starts_with("sctp-port:")) return set_once(Result.sctp_port, Text.substr(10));
   if (Text.starts_with("max-message-size:")) return set_once(Result.max_message_size, Text.substr(17));

   if (Text IS "rtcp-mux") {
      Result.rtcp_mux = true;
      return true;
   }

   if (Text IS "end-of-candidates") {
      Result.end_of_candidates = true;
      return true;
   }

   if (Text.starts_with("ice-options:")) {
      for (auto option : words(Text.substr(12))) if (option IS "trickle") Result.trickle = true;
      return true;
   }

   if (Text.starts_with("candidate:")) {
      if (Result.candidates.size() >= 256 or not valid_candidate(Text)) return false;
      Result.candidates.emplace_back(Text);
   }

   return true;
}

//********************************************************************************************************************
// Combines session-level and media-level attributes into the data channel description, and validates the result.

bool resolve(const Attributes &Session, const Attributes &Media, DataChannel &Result)
{
   auto ufrag = Media.ufrag.empty() ? Session.ufrag : Media.ufrag;
   auto pwd = Media.pwd.empty() ? Session.pwd : Media.pwd;
   auto fp = Media.fingerprint.empty() ? Session.fingerprint : Media.fingerprint;
   auto role = Media.setup.empty() ? Session.setup : Media.setup;

   if (not token(ufrag, 4, 256) or not token(pwd, 22, 256)) return false;

   auto fp_words = words(fp);
   if (fp_words.size() != 2 or fp_words[0] != "sha-256" or not fingerprint(fp_words[1])) return false;
   if (Media.mid.empty() or Media.mid.size() > 64 or not setup(role, Result.setup)) return false;

   uint64_t port = 0;
   if (not number(Media.sctp_port, port) or port IS 0 or port > 65535) return false;
   Result.sctp_port = uint16_t(port);
   Result.max_message_size = 65536; // RFC 8841 default when the attribute is absent.
   if (not Media.max_message_size.empty() and not number(Media.max_message_size, Result.max_message_size)) return false;

   Result.mid = Media.mid;
   Result.ice_ufrag = ufrag;
   Result.ice_pwd = pwd;
   Result.fingerprint.reserve(fp_words[1].size());
   for (char ch : fp_words[1]) Result.fingerprint.push_back(char(std::toupper((unsigned char)ch)));
   Result.candidates = Media.candidates;
   Result.rtcp_mux = Session.rtcp_mux or Media.rtcp_mux;
   Result.end_of_candidates = Media.end_of_candidates;
   return true;
}

//********************************************************************************************************************
// Validates a Description before it is accepted by parse() or serialised.

bool valid(const Description &Value)
{
   const auto &data = Value.data;
   if (Value.session_id.empty() or Value.session_id.size() > 20) return false;

   uint64_t session_id = 0;
   if (not number(Value.session_id, session_id)) return false;

   if (Value.origin_address.empty() or Value.origin_address.find_first_of("\r\n ") != std::string::npos) return false;
   if (data.mid.empty() or data.mid.size() > 64 or data.mid.find_first_of("\r\n ") != std::string::npos) return false;
   if (not token(data.ice_ufrag, 4, 256) or not token(data.ice_pwd, 22, 256)) return false;
   if (not fingerprint(data.fingerprint) or data.sctp_port IS 0) return false;

   for (const auto &candidate : data.candidates) {
      if (not valid_candidate(candidate)) return false;
   }

   return true;
}

} // namespace

//********************************************************************************************************************
// Parses a session description.  Result is only updated if the description is valid.

bool parse(std::string_view Text, Description &Result)
{
   if (Text.empty() or Text.size() > 65536 or Text.find('\0') != std::string_view::npos) return false;

   Description parsed;
   Attributes session;
   Attributes media;
   std::vector<std::string_view> bundle;
   bool version = false, origin = false, name = false, timing = false, application = false, other_media = false;
   bool connection = false;

   while (not Text.empty()) {
      auto end = Text.find('\n');
      auto line = Text.substr(0, end);
      if (end IS std::string_view::npos) Text = {};
      else Text.remove_prefix(end + 1);
      if (line.ends_with('\r')) line.remove_suffix(1);

      if (line.empty()) continue;
      if (line.size() > 4096 or line.size() < 2 or line[1] != '=') return false;

      auto value = line.substr(2);
      switch (line[0]) {
         case 'v':
            if (version or value != "0") return false;
            version = true;
            break;

         case 'o': {
            if (origin or application or other_media) return false;
            auto fields = words(value);
            uint64_t ignored = 0;
            if (fields.size() != 6 or not number(fields[1], ignored) or
                not number(fields[2], parsed.session_version) or fields[3] != "IN" or
                (fields[4] != "IP4" and fields[4] != "IP6")) return false;
            parsed.session_id = fields[1];
            parsed.origin_address = fields[5];
            parsed.origin_ipv6 = fields[4] IS "IP6";
            origin = true;
            break;
         }

         case 's': name = true; break;

         case 't': timing = true; break;

         case 'm': {
            auto fields = words(value);
            if (fields.size() < 4) return false;
            if (fields[0] IS "application") {
               if (application or fields[2] != "UDP/DTLS/SCTP" or fields[3] != "webrtc-datachannel") return false;
               uint64_t port = 0;
               // Rejected (port-zero) application sections are unsupported.
               if (not number(fields[1], port) or port IS 0 or port > 65535) return false;
               application = true;
               other_media = false;
            }
            else return false; // Media sections other than data channels cannot be answered.
            break;
         }

         case 'c': if (application and not other_media) connection = true; break;

         case 'a':
            if (other_media) break;
            if (not application and value.starts_with("group:BUNDLE ")) {
               if (not bundle.empty()) return false;
               bundle = words(value.substr(13));
            }
            else if (not application and value IS "ice-lite") parsed.ice_lite = true;
            else if (not attribute(value, application ? media : session)) return false;
            break;

         default: break;
      }
   }

   if (not version or not origin or not name or not timing or not application or not connection) return false;
   if (not resolve(session, media, parsed.data)) return false;
   if (std::find(bundle.begin(), bundle.end(), parsed.data.mid) IS bundle.end()) return false;
   parsed.trickle = session.trickle or media.trickle;
   if (not valid(parsed)) return false;

   Result = std::move(parsed);
   return true;
}

//********************************************************************************************************************
// Validates a candidate attribute that has been received through trickle ICE.

bool valid_candidate_line(std::string_view Text, bool &Udp)
{
   return valid_candidate(Text, &Udp);
}

//********************************************************************************************************************
// Serialises a Description, returning an empty string if it is invalid.

std::string serialize(const Description &Value)
{
   if (not valid(Value)) return {};

   const auto &data = Value.data;
   std::string result;
   result.reserve(512);
   result += "v=0\r\no=- " + Value.session_id + " " + std::to_string(Value.session_version) +
      (Value.origin_ipv6 ? " IN IP6 " : " IN IP4 ") + Value.origin_address +
      "\r\ns=-\r\nt=0 0\r\na=group:BUNDLE " + data.mid + "\r\n";
   if (Value.ice_lite) result += "a=ice-lite\r\n";
   result += "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\n";
   result += "a=ice-ufrag:" + data.ice_ufrag + "\r\na=ice-pwd:" + data.ice_pwd + "\r\n";
   if (Value.trickle) result += "a=ice-options:trickle\r\n";
   result += "a=fingerprint:sha-256 " + data.fingerprint + "\r\na=setup:" + setup_name(data.setup) +
      "\r\na=mid:" + data.mid + "\r\na=sctp-port:" + std::to_string(data.sctp_port) +
      "\r\na=max-message-size:" + std::to_string(data.max_message_size) + "\r\n";
   if (data.rtcp_mux) result += "a=rtcp-mux\r\n";
   for (const auto &candidate : data.candidates) result += "a=" + candidate + "\r\n";
   if (data.end_of_candidates) result += "a=end-of-candidates\r\n";

   return result;
}

//********************************************************************************************************************
// Checks that an answer is compatible with the offer.  The offerer is always actpass, and the answer must select a DTLS
// role.

bool validate_answer(const Description &Offer, const Description &Answer)
{
   if (not valid(Offer) or not valid(Answer)) return false;
   if (Offer.data.setup != Setup::ACTPASS or Answer.data.setup IS Setup::ACTPASS) return false;
   if (Offer.data.mid != Answer.data.mid) return false;
   return true;
}

} // namespace rtc::sdp
