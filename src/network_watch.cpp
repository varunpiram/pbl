#include "network_watch.hpp"
#include "config.h"
#include "process.hpp"
#include "storage.hpp"
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>
#include <iostream>

namespace blocker {
static void networkChanged(SCDynamicStoreRef, CFArrayRef, void* context) {
    auto queue = static_cast<dispatch_queue_t>(context);
    static unsigned revision = 0;
    auto current = ++revision;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), queue, ^{
      if (current != revision)
          return;
      auto result = run({PBL_WORKER, "refresh"});
      if (result.status != 0)
          std::cerr << result.output;
    });
}

void watchNetwork() {
    auto queue = dispatch_queue_create("local.pbl.network", DISPATCH_QUEUE_SERIAL);
    SCDynamicStoreContext context{0, queue, nullptr, nullptr, nullptr};
    auto store = SCDynamicStoreCreate(nullptr, CFSTR("pbl"), networkChanged, &context);
    const void* patterns[] = {CFSTR("State:/Network/(Global|Service/[^/]+)/(DNS|IPv4|IPv6)"),
                              CFSTR("State:/Network/Interface/[^/]+/(Link|AirPort)")};
    auto keys = CFArrayCreate(nullptr, patterns, 2, &kCFTypeArrayCallBacks);
    bool ready = store && SCDynamicStoreSetNotificationKeys(store, nullptr, keys) &&
                 SCDynamicStoreSetDispatchQueue(store, queue);
    CFRelease(keys);
    require(ready, "Cannot watch network configuration");
    dispatch_main();
}
}
