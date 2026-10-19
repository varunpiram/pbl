#include "dns.hpp"
#include "storage.hpp"
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <resolv.h>
#include <algorithm>
#include <limits>
#include <iostream>

namespace blocker {
bool isIPAddress(const std::string& host) {
    in6_addr address{};
    return inet_pton(AF_INET, host.c_str(), &address) == 1 ||
           inet_pton(AF_INET6, host.c_str(), &address) == 1;
}

std::vector<std::string> websiteHosts(const std::string& host) {
    if (isIPAddress(host))
        return {host};
    return {host, host.starts_with("www.") ? host.substr(4) : "www." + host};
}

struct Resolver {
    struct __res_state state{};

    Resolver() {
        require(res_ninit(&state) == 0, "Cannot initialize DNS resolver");
        state.retrans = 2;
        state.retry = 1;
    }

    ~Resolver() {
        res_ndestroy(&state);
    }
};

AddressCache resolveAddresses(const std::string& host, Timestamp now) {
    if (isIPAddress(host))
        return {{host, now + 3600}};
    Resolver resolver;
    AddressCache result;
    for (const auto& name : websiteHosts(host)) {
        for (int type : {ns_t_a, ns_t_aaaa}) {
            unsigned char answer[65536];
            int size =
                res_nquery(&resolver.state, name.c_str(), ns_c_in, type, answer, sizeof(answer));
            if (size < 0) {
                require(resolver.state.res_h_errno == HOST_NOT_FOUND ||
                            resolver.state.res_h_errno == NO_DATA,
                        "DNS query failed for " + name);
                continue;
            }
            ns_msg message;
            require(ns_initparse(answer, size, &message) == 0, "Malformed DNS response");
            std::vector<std::pair<std::string, unsigned>> records;
            unsigned aliasTTL = std::numeric_limits<unsigned>::max();
            for (int i = 0; i < ns_msg_count(message, ns_s_an); ++i) {
                ns_rr record;
                require(ns_parserr(&message, ns_s_an, i, &record) == 0, "Malformed DNS record");
                if (ns_rr_type(record) == ns_t_cname)
                    aliasTTL = std::min(aliasTTL, ns_rr_ttl(record));
                if (ns_rr_type(record) != type)
                    continue;
                require(ns_rr_rdlen(record) == (type == ns_t_a ? 4 : 16), "Invalid DNS address");
                char address[INET6_ADDRSTRLEN];
                require(inet_ntop(type == ns_t_a ? AF_INET : AF_INET6, ns_rr_rdata(record), address,
                                  sizeof(address)),
                        "Invalid DNS address");
                records.emplace_back(address, ns_rr_ttl(record));
            }
            for (const auto& [address, ttl] : records)
                result[address] = std::max(result[address], now + std::min(ttl, aliasTTL) + 60);
        }
    }
    return result;
}

AddressCache mergeAddresses(AddressCache old, const AddressCache& fresh, Timestamp now) {
    for (const auto& [address, expiry] : fresh)
        old[address] = std::max(old[address], expiry);
    std::erase_if(old, [&](const auto& entry) {
        return entry.second <= now;
    });
    return old;
}

AddressCache refreshAddresses(const AddressCache& previous,
                              const std::function<AddressCache()>& query, Timestamp now) {
    try {
        return mergeAddresses(previous, query(), now);
    } catch (const std::exception& error) {
        if (previous.empty())
            throw;
        std::cerr << error.what() << "; retaining previous PF addresses\n";
        return previous;
    }
}

}
