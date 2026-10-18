#import "BackendProtocol.h"

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 4)
            return 2;
        NSXPCConnection* connection = [[NSXPCConnection alloc] initWithMachServiceName:@(argv[1])
                                                                               options:0];
        [connection setCodeSigningRequirement:@(argv[2])];
        connection.remoteObjectInterface =
            [NSXPCInterface interfaceWithProtocol:@protocol(PBLBackend)];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block int result = 2;
        BOOL allowed = [@(argv[3]) isEqualToString:@"allow"];
        [connection activate];
        id<PBLBackend> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError* error) {
          fprintf(stderr, "%s\n", error.description.UTF8String);
          result = !allowed && [error.domain isEqualToString:NSCocoaErrorDomain] &&
                           (error.code == NSXPCConnectionCodeSigningRequirementFailure ||
                            error.code == NSXPCConnectionInterrupted)
                       ? 0
                       : 1;
          dispatch_semaphore_signal(done);
        }];
        [proxy statusWithReply:^(NSString* status) {
          result = allowed && [status isEqualToString:@"pbl"] ? 0 : 1;
          dispatch_semaphore_signal(done);
        }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
            result = 2;
        [connection invalidate];
        printf("%s\n", result == 0 ? "PASS" : "FAIL");
        return result;
    }
}
