#import "BackendProtocol.h"
#include "executor.hpp"
#include "macos_environment.hpp"
#include "storage.hpp"
#include <unistd.h>
#ifndef PBL_CLIENT_REQUIREMENT
#define PBL_CLIENT_REQUIREMENT "never"
#endif
using namespace blocker;

static NSString* string(const std::string& value) {
    return @(value.c_str());
}

static std::string text(id value) {
    require([value isKindOfClass:NSString.class] && [value length] <= 4096, "Invalid text");
    return [value UTF8String];
}

static NSArray* array(id value) {
    require([value isKindOfClass:NSArray.class] && [value count] <= 1024, "Invalid list");
    return value;
}

static int number(id value) {
    require([value isKindOfClass:NSNumber.class] && [value doubleValue] == [value intValue],
            "Invalid integer");
    return [value intValue];
}

static NSData* snapshot(const Schedule& schedule) {
    NSMutableArray* sets = [NSMutableArray new];
    for (const auto& [id, set] : schedule.sets) {
        NSMutableArray *resources = [NSMutableArray new], *windows = [NSMutableArray new],
                       *intervals = [NSMutableArray new], *quickIntervals = [NSMutableArray new];
        for (const auto& rid : set.resources) {
            const auto& r = schedule.resources.at(rid);
            [resources addObject:@{
                @"id" : string(rid),
                @"name" : string(r.target),
                @"isApp" : @(r.type == "app"),
                @"path" : string(r.target)
            }];
        }
        for (const auto& w : schedule.weekly)
            if (w.set == id)
                [windows addObject:@{
                    @"id" : @(windows.count).stringValue,
                    @"day" : @(w.day),
                    @"start" : @(w.start),
                    @"end" : @(w.end)
                }];
        for (auto [start, end] : setIntervals(schedule, id))
            if (end > currentTime())
                [intervals addObject:@{@"start" : @(start), @"end" : @(end)}];
        for (auto [start, end] : setIntervals(schedule, id, true))
            if (end > currentTime())
                [quickIntervals addObject:@{@"start" : @(start), @"end" : @(end)}];
        [sets addObject:@{
            @"id" : string(id),
            @"name" : @"New set",
            @"resources" : resources,
            @"windows" : windows,
            @"intervals" : intervals,
            @"quickIntervals" : quickIntervals
        }];
    }
    return [NSJSONSerialization dataWithJSONObject:sets options:0 error:nil];
}

@interface PBLHelper : NSObject <PBLBackend, NSXPCListenerDelegate>
@property(copy) NSString* requirement;
@end
@implementation PBLHelper

- (BOOL)listener:(NSXPCListener*)listener shouldAcceptNewConnection:(NSXPCConnection*)connection {
    [connection setCodeSigningRequirement:self.requirement];
    connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(PBLBackend)];
    connection.exportedObject = self;
    [connection activate];
    return YES;
}

- (void)statusWithReply:(void (^)(NSString*))reply {
    reply(@"pbl");
}

- (void)request:(NSData*)data reply:(void (^)(NSData*, NSString*))reply {
    try {
        require(data && data.length <= 65536, "Invalid request size");
        NSDictionary* input = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        require([input isKindOfClass:NSDictionary.class], "Invalid request");
        MacOSEnvironment environment(PBL_STATE, PBL_WORKER);
        Executor executor(PBL_STATE, environment);
        auto command = text(input[@"command"]);
        if (command == "renew")
            executor.recover();
        else if (command == "save") {
            auto id = text(input[@"id"]);
            std::vector<ResourceDefinition> resources;
            std::vector<WeeklyWindow> windows;
            for (NSDictionary* r in array(input[@"resources"])) {
                require([r isKindOfClass:NSDictionary.class], "Invalid resource");
                bool app = number(r[@"isApp"]) != 0;
                resources.push_back({text(r[@"id"]), app ? "app" : "website",
                                     text(app ? r[@"path"] : r[@"name"]),
                                     NSXPCConnection.currentConnection.effectiveUserIdentifier});
            }
            for (NSDictionary* w in array(input[@"windows"])) {
                require([w isKindOfClass:NSDictionary.class], "Invalid weekly window");
                windows.push_back({id, number(w[@"day"]), number(w[@"start"]), number(w[@"end"])});
            }
            executor.saveSet({id, {}}, resources, windows);
        } else if (command == "remove")
            executor.removeSet(text(input[@"id"]));
        else if (command == "submit") {
            auto id = text(input[@"id"]);
            int minutes = number(input[@"minutes"]);
            require(minutes >= 15 && minutes <= 720 && minutes % 15 == 0,
                    "Choose a duration from 15 minutes to 12 hours");
            auto end = ((currentTime() + minutes * 60 + 59) / 60) * 60;
            executor.submit({{id, true, ExecuteImmediately{}}, {id, false, ExecuteAt{end}}});
        } else
            require(command == "read", "Unknown command");
        reply(snapshot(executor.inspect()), nil);
    } catch (const std::exception& error) {
        reply(nil, @(error.what()));
    }
}

@end

int main(int argc, char** argv) {
    @autoreleasepool {
        NSString *name = @PBL_SERVICE, *requirement = @PBL_CLIENT_REQUIREMENT;
#ifdef PBL_AUTH_TEST
        if (geteuid() == 0 || argc != 3)
            return 2;
        requirement = @(argv[1]);
        name = @(argv[2]);
#else
        if (geteuid() != 0 || argc != 1 || [requirement isEqualToString:@"never"])
            return 2;
#endif
        PBLHelper* helper = [PBLHelper new];
        helper.requirement = requirement;
        NSXPCListener* listener = [[NSXPCListener alloc] initWithMachServiceName:name];
        [listener setConnectionCodeSigningRequirement:requirement];
        listener.delegate = helper;
        [listener activate];
        [[NSRunLoop currentRunLoop] run];
        return 0;
    }
}
