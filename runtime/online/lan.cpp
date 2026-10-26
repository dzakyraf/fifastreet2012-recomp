// Players on the same network, over plain UDP: the smallest real service, and
// the one that tests the kernel side without an account anywhere. Two copies on
// one machine find each other too, which the title's own sockets cannot do --
// both would want port 1000.
//
// Each copy sends from a port of its own, and a peer is that port's address:
// the IPv4 address in the top bits, the port in the low 16. Broadcasts go to a
// port every copy shares (PES17_LAN_PORT, 3074 by default), which the kernel
// delivers to all of them; each copy's own port is the first free one after it.
// Who the player is, to the title, is the profile at the first controller
// (signin.h), plus this copy's port slot, so a second copy on one machine is a
// second player. PES17_NAME is another name for him than the profile's.
// PES17_LAN_IFACE pins the announced adapter by name or address substring;
// without it the first connection up is announced.
//
// Xbox LIVE lobbies: every copy says who it is and what can be joined, once a
// second, to everyone on the shared port. PES17_LAN_JOIN=<name> joins that
// player's party or lobby as soon as it is seen, as choosing a friend and
// "Join Session" does on the console. The lobby's "Invite friends" invites
// every player seen; a copy started with PES17_LAN_ACCEPT=1 accepts.
#include "service.h"
#include "../env.h"
#include "../log.h"
#include "../signin.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// The few places the two socket APIs differ.
namespace
{
#ifdef _WIN32
    using Socket = SOCKET;
    const Socket kNoSocket = INVALID_SOCKET;
    void CloseSocket(Socket s) { closesocket(s); }
    void MakeNonBlocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
    const char* UserName() { return std::getenv("USERNAME"); }
#else
    using Socket = int;
    const Socket kNoSocket = -1;
    void CloseSocket(Socket s) { ::close(s); }
    void MakeNonBlocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
    const char* UserName() { return std::getenv("USER"); }
#endif

    bool SendDatagram(Socket s, const void* data, size_t size, const sockaddr_in& to)
    {
        return ::sendto(s, static_cast<const char*>(data), int(size), 0,
                        reinterpret_cast<const sockaddr*>(&to), sizeof to) >= 0;
    }

    // -1 when nothing is waiting.
    long ReceiveDatagram(Socket s, void* data, size_t size, sockaddr_in& from)
    {
        socklen_t length = sizeof from;
        return long(::recvfrom(s, static_cast<char*>(data), int(size), 0,
                               reinterpret_cast<sockaddr*>(&from), &length));
    }

    uint64_t Fnv(const std::string& text)
    {
        uint64_t hash = 0xCBF29CE484222325ull;   // FNV-1a
        for (char c : text) hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
        return hash;
    }

    // The account travels big-endian; every host this runs on is little-endian.
    uint64_t BigEndian64(uint64_t value) { return __builtin_bswap64(value); }
}

namespace
{
    // Magic, the sender's instance, its port and the port it is for.
    struct Frame
    {
        uint32_t magic;
        uint32_t instance;
        uint16_t fromPort, toPort;
    };
    constexpr uint32_t kMagic = 0x4D57324C;   // "MW2L"

    // Who a copy is and what can be joined: to everyone as presence, or to one
    // player as an invitation. Slots and nonce travel too, so a system-link
    // invitation carries everything the title's own `connect` line needs
    // (XNADDR, key, id, slots, nonce) and can join without the Live path.
    //
    // `tu` is the game version this copy runs (kPresenceTu0/Tu6 below): TU0
    // plays only with TU0, TU6 only with TU6. It rides in what was padding,
    // so the struct keeps its size and older builds -- which send zero --
    // still parse it; zero reads as "unknown, predates versions".
    struct Presence
    {
        uint32_t magic;
        uint32_t instance;
        uint8_t kind;          // kAnnounce or kInvite
        uint8_t joinable;      // `session` holds something
        uint8_t tu;            // kPresenceTu0, kPresenceTu6, or 0 unknown
        uint8_t pad;
        uint64_t account;      // big-endian
        char name[32];
        uint8_t session[online::kSessionInfoSize];
        uint8_t publicSlots;
        uint8_t privateSlots;
        uint8_t nonce[8];
    };
    constexpr uint32_t kPresenceMagic = 0x4D573250;   // "MW2P"
    constexpr uint8_t kAnnounce = 0, kInvite = 1;
    constexpr uint8_t kPresenceTu0 = 1, kPresenceTu6 = 2;
#ifdef PES17_VERSION_TU0
    constexpr uint8_t kOwnTu = kPresenceTu0;
    constexpr const char* kOwnTuName = "TU0";
#else
    constexpr uint8_t kOwnTu = kPresenceTu6;
    constexpr const char* kOwnTuName = "TU6";
#endif
    constexpr const char* TuName(uint8_t tu)
    {
        return tu == kPresenceTu6 ? "TU6" : tu == kPresenceTu0 ? "TU0" : "an older build";
    }
    constexpr auto kAnnounceEvery = std::chrono::seconds(1);

    struct Peer
    {
        uint64_t id = 0, account = 0;
        bool joinable = false;
        uint8_t tu = 0;        // the game version they run, 0 when unknown
        bool warnedTu = false; // told once that their version differs
        uint8_t session[online::kSessionInfoSize]{};
        uint8_t publicSlots = 0, privateSlots = 0;
        uint8_t nonce[8]{};
    };
    constexpr size_t kLargest = 64 * 1024;
    constexpr size_t kQueued = 512;

    uint64_t IdOf(const sockaddr_in& address)
    {
        return (uint64_t(ntohl(address.sin_addr.s_addr)) << 16) | ntohs(address.sin_port);
    }

    // The title's own `connect` line for a heard system-link session, in the
    // exact form its server browser issues (xam.cpp's AdvertiseHosted): the
    // guest then joins the way picking the server would, with no Live
    // sign-in or accept flag needed.
    std::string HexBytes(const uint8_t* p, size_t n)
    {
        static const char* digits = "0123456789abcdef";
        std::string text;
        text.reserve(2 * n);
        for (size_t i = 0; i < n; i++)
        {
            text += digits[p[i] >> 4];
            text += digits[p[i] & 15];
        }
        return text;
    }

    std::string ConnectLine(const Peer& peer)
    {
        return "connect " + HexBytes(peer.session + 8, 36) + " " +
               HexBytes(peer.session + 44, 16) + " " + HexBytes(peer.session, 8) + " " +
               std::to_string(peer.publicSlots) + " " + std::to_string(peer.privateSlots) + " " +
               HexBytes(peer.nonce, 8);
    }

    // A Live session id carries 0x80 in its top bits; a system-link one does
    // not. Only the wire format both ends of this build speak is trusted.
    bool IsLiveSession(const Peer& peer) { return (peer.session[0] & 0x80) != 0; }

    sockaddr_in AddressOf(uint64_t id)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(uint32_t(id >> 16));
        address.sin_port = htons(uint16_t(id));
        return address;
    }

    // The network connections that are up, the loopback aside, in the order
    // the system lists them. On Android the WiFi interfaces come first: the
    // system lists cellular (rmnet) before wlan, and announcing a cellular
    // address, or broadcasting by the default route, would send discovery to
    // the carrier where it dies instead of to the LAN the players share.
    struct Adapter { std::string name; uint32_t address; uint32_t netmask; };
    std::vector<Adapter> Adapters()
    {
        std::vector<Adapter> found;
#ifdef _WIN32
        ULONG size = 16 * 1024;
        std::vector<uint8_t> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(size);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        }
        if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, adapters, &size) != NO_ERROR) return found;
        for (auto* at = adapters; at; at = at->Next)
        {
            if (at->OperStatus != IfOperStatusUp || at->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            char name[128] = "?";
            if (at->FriendlyName)
                WideCharToMultiByte(CP_UTF8, 0, at->FriendlyName, -1, name, sizeof name, nullptr, nullptr);
            name[sizeof name - 1] = 0;
            for (auto* unicast = at->FirstUnicastAddress; unicast; unicast = unicast->Next)
            {
                const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                if (address->sin_family != AF_INET) continue;
                const ULONG prefix = unicast->OnLinkPrefixLength > 32 ? 32 : unicast->OnLinkPrefixLength;
                const uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
                found.push_back({ name, ntohl(address->sin_addr.s_addr), mask });
            }
        }
#else
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) != 0) return found;
        for (ifaddrs* at = list; at; at = at->ifa_next)
        {
            if (!at->ifa_addr || at->ifa_addr->sa_family != AF_INET) continue;
            if (!(at->ifa_flags & IFF_UP) || (at->ifa_flags & IFF_LOOPBACK)) continue;
            uint32_t mask = 0;
            if (at->ifa_netmask && at->ifa_netmask->sa_family == AF_INET)
                mask = ntohl(reinterpret_cast<sockaddr_in*>(at->ifa_netmask)->sin_addr.s_addr);
            found.push_back({ at->ifa_name, ntohl(reinterpret_cast<sockaddr_in*>(at->ifa_addr)->sin_addr.s_addr), mask });
        }
        freeifaddrs(list);
#ifdef PES17_ANDROID
        std::stable_partition(found.begin(), found.end(), [](const Adapter& a) {
            return a.name.compare(0, 4, "wlan") == 0;
        });
#endif
#endif
        return found;
    }

    // The address other copies reach this one at: the first connection that is
    // up and not the loopback, or the loopback when there is none. The log
    // names them all: with more than one, the first may not be the one the
    // other players are on.
    //
    // PES17_LAN_IFACE pins one: any adapter whose name or address contains the
    // value ("WiFi", "wlan0", "192.168.1"). Matters with several up at once --
    // a VPN adapter first means sessions invite to an address only VPN peers
    // reach, and WiFi-only guests then loop on "attempt #N" forever.
    uint32_t OwnAddress()
    {
        const std::vector<Adapter> adapters = Adapters();
        std::string list;
        for (const Adapter& adapter : adapters)
        {
            in_addr address;
            address.s_addr = htonl(adapter.address);
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &address, text, sizeof text);
            list += (list.empty() ? "" : ", ") + adapter.name + " " + text;
        }
        if (adapters.empty()) LOGI("online: no network connection is up; lan stays on this machine");
        else LOGI("online: network connections: %s; announcing the first", list.c_str());
        const char* pinned = env::Text("PES17_LAN_IFACE");
        if (pinned && *pinned)
        {
            for (const Adapter& adapter : adapters)
            {
                in_addr address;
                address.s_addr = htonl(adapter.address);
                char text[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &address, text, sizeof text);
                if (adapter.name.find(pinned) != std::string::npos ||
                    std::string(text).find(pinned) != std::string::npos)
                {
                    LOGI("online: PES17_LAN_IFACE=%s, announcing %s %s", pinned,
                         adapter.name.c_str(), text);
                    return adapter.address;
                }
            }
            LOGW("online: PES17_LAN_IFACE=%s matches no connection, announcing the first", pinned);
        }
        return adapters.empty() ? uint32_t(INADDR_LOOPBACK) : adapters.front().address;
    }

    Socket OpenSocket(uint16_t port, bool shared)
    {
        Socket s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s == kNoSocket) return kNoSocket;
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
        if (shared) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
        MakeNonBlocking(s);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
        {
            CloseSocket(s);
            return kNoSocket;
        }
        return s;
    }

    class Lan final : public online::Service
    {
    public:
        ~Lan() override
        {
            if (own_ != kNoSocket) CloseSocket(own_);
            if (shared_ != kNoSocket) CloseSocket(shared_);
        }

        bool Start() override
        {
#ifdef _WIN32
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { LOGE("online: no Winsock"); return false; }
#endif
            sharedPort_ = uint16_t(env::Number("PES17_LAN_PORT", 3074));
            // A port of its own, the first free one above the shared port.
            for (slot_ = 0; slot_ < 64 && own_ == kNoSocket; slot_++)
                own_ = OpenSocket(uint16_t(sharedPort_ + 1 + slot_), false);
            slot_--;
            shared_ = OpenSocket(sharedPort_, true);
            if (own_ == kNoSocket || shared_ == kNoSocket)
            {
                LOGE("online: lan could not open its sockets (shared port %u)", sharedPort_);
                return false;
            }
            sockaddr_in bound{};
            socklen_t length = sizeof bound;
            getsockname(own_, reinterpret_cast<sockaddr*>(&bound), &length);
            bound.sin_addr.s_addr = htonl(OwnAddress());
            id_ = IdOf(bound);
            instance_ = std::random_device{}();
            const signin::Player player = signin::First();
            account_ = player.id;
            // Until there were profiles, the account was made from the name.
            const char* name = env::Text("PES17_NAME");
            const char* login = name ? name : UserName();
            former_ = Fnv(login ? login : "Player");
            name_ = name ? name : player.name;
            if (const char* join = env::Text("PES17_LAN_JOIN")) join_ = join;
            accept_ = env::Flag("PES17_LAN_ACCEPT");
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &bound.sin_addr, text, sizeof text);
            LOGI("online: lan as \"%s\" (%s) at %s:%u, broadcasts on %u", name_.c_str(), kOwnTuName, text,
                 ntohs(bound.sin_port), sharedPort_);
            // Discovery also goes to every connected subnet's broadcast
            // address: a limited broadcast (255.255.255.255) is filtered on
            // some routers and hotspots, while the subnet's own still passes.
            {
                std::string subnets;
                for (const Adapter& adapter : Adapters())
                {
                    if (adapter.netmask == 0 || adapter.netmask == 0xFFFFFFFFu) continue;
                    const uint32_t target = adapter.address | ~adapter.netmask;
                    if (target == adapter.address || target == INADDR_BROADCAST) continue;
                    sockaddr_in to{};
                    to.sin_family = AF_INET;
                    to.sin_addr.s_addr = htonl(target);
                    to.sin_port = htons(sharedPort_);
                    bool dup = false;
                    for (const sockaddr_in& have : broadcasts_)
                        dup = dup || have.sin_addr.s_addr == to.sin_addr.s_addr;
                    if (dup) continue;
                    broadcasts_.push_back(to);
                    in_addr bcast;
                    bcast.s_addr = to.sin_addr.s_addr;
                    char btext[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &bcast, btext, sizeof btext);
                    subnets += (subnets.empty() ? "" : ", ") + std::string(btext);
                }
                if (!subnets.empty()) LOGI("online: lan also announces to %s", subnets.c_str());
            }
            return true;
        }

        uint64_t LocalId() override { return id_; }
        // The name, and which copy on this machine this is: the first to start
        // takes the first slot, so a second copy is a second player.
        uint64_t Account() override { return account_ + slot_; }
        uint64_t FormerAccount() override { return former_ + slot_; }
        std::string LocalName() override { return name_; }

        bool Send(uint64_t peer, uint16_t fromPort, uint16_t toPort,
                  const void* bytes, size_t size) override
        {
            return SendTo(AddressOf(peer), fromPort, toPort, bytes, size);
        }

        bool Broadcast(uint16_t fromPort, uint16_t toPort, const void* bytes, size_t size) override
        {
            sockaddr_in everyone{};
            everyone.sin_family = AF_INET;
            everyone.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            everyone.sin_port = htons(sharedPort_);
            bool ok = SendTo(everyone, fromPort, toPort, bytes, size);
            for (const sockaddr_in& subnet : broadcasts_)
                ok = SendTo(subnet, fromPort, toPort, bytes, size) && ok;
            return ok;
        }

        bool Receive(uint16_t port, online::Datagram& out) override
        {
            std::lock_guard g(lock_);
            Drain(own_);
            Drain(shared_);
            auto it = waiting_.find(port);
            if (it == waiting_.end() || it->second.empty()) return false;
            out = std::move(it->second.front());
            it->second.pop_front();
            return true;
        }

        bool Lobbies() override { return true; }

        // Diagnostics for the app's HUD: how many copies this one hears.
        size_t PeerCount() override
        {
            std::lock_guard g(lock_);
            return peers_.size();
        }

        std::string PeerNames() override
        {
            std::lock_guard g(lock_);
            std::string names;
            for (const auto& [name, peer] : peers_)
                if (peer.joinable) { names += name; names += '\n'; }
            return names;
        }

        bool JoinPeer(const std::string& name) override
        {
            std::lock_guard g(lock_);
            auto it = peers_.find(name);
            if (it == peers_.end() || !it->second.joinable) return false;
            Peer& peer = it->second;
            if (!IsLiveSession(peer))
            {
                LOGI("online: joining %s's system-link game directly", name.c_str());
                joins_.push_back(ConnectLine(peer));
            }
            else
            {
                LOGI("online: accepting %s's invitation", name.c_str());
                invites_.push_back(InviteFrom(peer, true));
            }
            return true;
        }

        void SetJoinable(const online::Joinable* joinable) override
        {
            std::lock_guard g(lock_);
            hasJoinable_ = joinable != nullptr;
            if (joinable) joinable_ = *joinable;
            AnnounceLocked();
        }

        bool NextInvite(online::Invite& out) override
        {
            std::lock_guard g(lock_);
            Drain(own_);
            Drain(shared_);
            const auto now = std::chrono::steady_clock::now();
            if (now - announced_ >= kAnnounceEvery) AnnounceLocked();
            if (peers_.size() != loggedPeers_)
            {
                loggedPeers_ = peers_.size();
                LOGI("online: lan sees %zu player(s) on the network", loggedPeers_);
            }
            if (!join_.empty())
            {
                auto it = peers_.find(join_);
                if (it != peers_.end() && it->second.joinable)
                {
                    LOGI("online: lan joins %s", join_.c_str());
                    if (!IsLiveSession(it->second))
                        joins_.push_back(ConnectLine(it->second));
                    else
                        invites_.push_back(InviteFrom(it->second, false));
                    join_.clear();
                }
            }
            if (invites_.empty()) return false;
            out = invites_.front();
            invites_.pop_front();
            return true;
        }

        // System-link joins queued above, as the title's own `connect` lines.
        // service.cpp types the system-link menu first, then the line.
        bool NextJoin(std::string& connect) override
        {
            std::lock_guard g(lock_);
            if (joins_.empty()) return false;
            connect = std::move(joins_.front());
            joins_.pop_front();
            return true;
        }

        void InviteFriends() override
        {
            std::lock_guard g(lock_);
            if (!hasJoinable_) { LOGW("online: nothing to invite anyone to yet"); return; }
            const Presence invite = PresenceLocked(kInvite);
            for (const auto& [name, peer] : peers_)
            {
                LOGI("online: lan invites %s", name.c_str());
                const sockaddr_in to = AddressOf(peer.id);
                SendDatagram(own_, &invite, sizeof invite, to);
            }
        }

    private:
        static online::Invite InviteFrom(const Peer& peer, bool fromInvite)
        {
            online::Invite invite;
            invite.inviter = peer.account;
            std::memcpy(invite.session, peer.session, sizeof invite.session);
            invite.fromInvite = fromInvite;
            return invite;
        }

        Presence PresenceLocked(uint8_t kind)
        {
            Presence presence{};
            presence.magic = htonl(kPresenceMagic);
            presence.instance = instance_;
            presence.kind = kind;
            presence.joinable = hasJoinable_;
            presence.tu = kOwnTu;
            presence.pad = 0;
            presence.account = BigEndian64(Account());
            std::snprintf(presence.name, sizeof presence.name, "%s", name_.c_str());
            if (hasJoinable_)
            {
                std::memcpy(presence.session, joinable_.session, sizeof presence.session);
                presence.publicSlots = joinable_.publicSlots;
                presence.privateSlots = joinable_.privateSlots;
                std::memcpy(presence.nonce, joinable_.nonce, sizeof presence.nonce);
            }
            return presence;
        }

        void AnnounceLocked()
        {
            announced_ = std::chrono::steady_clock::now();
            const Presence presence = PresenceLocked(kAnnounce);
            sockaddr_in everyone{};
            everyone.sin_family = AF_INET;
            everyone.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            everyone.sin_port = htons(sharedPort_);
            SendDatagram(own_, &presence, sizeof presence, everyone);
            for (const sockaddr_in& subnet : broadcasts_)
                SendDatagram(own_, &presence, sizeof presence, subnet);
        }

        void Heard(const sockaddr_in& from, const Presence& presence)
        {
            if (presence.instance == instance_) return;
            const size_t rawLen = strnlen(presence.name, sizeof presence.name);
            std::string name;
            for (size_t i = 0; i < rawLen; ++i)
            {
                const unsigned char ch = static_cast<unsigned char>(presence.name[i]);
                if (ch >= 32 && ch < 127) name.push_back(static_cast<char>(ch));
            }
            if (name.empty()) name = "Player";
            const bool fresh = peers_.find(name) == peers_.end();
            Peer& peer = peers_[name];
            if (fresh)
            {
                char text[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, text, sizeof text);
                LOGI("online: lan discovered \"%s\" at %s:%u", name.c_str(), text, ntohs(from.sin_port));
            }
            peer.id = IdOf(from);
            peer.account = BigEndian64(presence.account);
            peer.publicSlots = presence.publicSlots;
            peer.privateSlots = presence.privateSlots;
            std::memcpy(peer.nonce, presence.nonce, sizeof peer.nonce);
            if (presence.tu != 0 && presence.tu != kOwnTu && !peer.warnedTu)
            {
                peer.warnedTu = true;
                LOGW("online: \"%s\" runs %s and this copy runs %s: versions never interoperate, "
                     "match them before joining", name.c_str(), TuName(presence.tu), kOwnTuName);
            }
            peer.tu = presence.tu;
            if (peer.account == Account() && !warned_)
            {
                warned_ = true;
                LOGE("online: %s on this network is the same player as this one; on one of the two machines,"
                     " take the `machine` line out of saves/profiles.txt", name.c_str());
            }
            peer.joinable = presence.joinable;
            std::memcpy(peer.session, presence.session, sizeof peer.session);
            if (presence.kind != kInvite || !peer.joinable) return;
            // A system-link invitation joins straight through the title's own
            // `connect` line -- the way picking the server would -- with no
            // Live sign-in or accept flag needed. A Live one keeps the
            // invitation path below, which the title answers in its Live menu.
            if (!IsLiveSession(peer))
            {
                LOGI("online: joining %s's system-link game directly", name.c_str());
                joins_.push_back(ConnectLine(peer));
                return;
            }
            if (accept_)
            {
                LOGI("online: lan accepts %s's invitation", name.c_str());
                invites_.push_back(InviteFrom(peer, true));
            }
            else
                LOGI("online: %s invites you (PES17_LAN_ACCEPT=1 accepts)", name.c_str());
        }

        bool SendTo(const sockaddr_in& to, uint16_t fromPort, uint16_t toPort,
                    const void* bytes, size_t size)
        {
            if (size > kLargest - sizeof(Frame)) return false;
            thread_local std::vector<uint8_t> packet;
            packet.resize(sizeof(Frame) + size);
            const Frame frame{ htonl(kMagic), instance_, htons(fromPort), htons(toPort) };
            std::memcpy(packet.data(), &frame, sizeof frame);
            std::memcpy(packet.data() + sizeof frame, bytes, size);
            return SendDatagram(own_, packet.data(), packet.size(), to);
        }

        // Everything the socket holds, sorted by the port it is for. A broadcast
        // comes back to the copy that sent it; the instance tells it apart.
        void Drain(Socket s)
        {
            thread_local std::vector<uint8_t> packet(kLargest);
            for (;;)
            {
                sockaddr_in from{};
                const long got = ReceiveDatagram(s, packet.data(), packet.size(), from);
                if (got < 0) return;
                uint32_t magic = 0;
                if (size_t(got) >= sizeof magic) std::memcpy(&magic, packet.data(), sizeof magic);
                if (ntohl(magic) == kPresenceMagic && size_t(got) == sizeof(Presence))
                {
                    Presence presence;
                    std::memcpy(&presence, packet.data(), sizeof presence);
                    Heard(from, presence);
                    continue;
                }
                Frame frame;
                if (size_t(got) < sizeof frame) continue;
                std::memcpy(&frame, packet.data(), sizeof frame);
                if (ntohl(frame.magic) != kMagic || frame.instance == instance_) continue;
                online::Datagram datagram;
                datagram.from = IdOf(from);
                datagram.fromPort = ntohs(frame.fromPort);
                datagram.toPort = ntohs(frame.toPort);
                datagram.bytes.assign(packet.begin() + sizeof frame, packet.begin() + got);
                // A port nobody reads fills up and drops, as a socket's buffer does.
                auto& queue = waiting_[datagram.toPort];
                if (queue.size() < kQueued) queue.push_back(std::move(datagram));
            }
        }

        Socket own_ = kNoSocket, shared_ = kNoSocket;
        uint16_t sharedPort_ = 0;
        uint64_t id_ = 0;
        uint32_t instance_ = 0;
        uint32_t slot_ = 0;
        std::string name_;
        uint64_t account_ = 0, former_ = 0;
        bool warned_ = false;                  // told once that another machine has this identity
        std::mutex lock_;
        std::map<uint16_t, std::deque<online::Datagram>> waiting_;

        std::map<std::string, Peer> peers_;   // by name
        size_t loggedPeers_ = 0;              // last count told to the log
        std::vector<sockaddr_in> broadcasts_; // subnet broadcasts, beside 255.255.255.255
        bool hasJoinable_ = false;
        online::Joinable joinable_;
        std::chrono::steady_clock::time_point announced_{};
        std::string join_;                     // PES17_LAN_JOIN, until joined
        bool accept_ = false;                  // PES17_LAN_ACCEPT
        std::deque<online::Invite> invites_;   // Live invitations, accepted above
        std::deque<std::string> joins_;        // system-link `connect` lines, typed by Frame
    };
}

std::unique_ptr<online::Service> online::MakeLan() { return std::make_unique<Lan>(); }
