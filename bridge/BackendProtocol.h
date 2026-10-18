#import <Foundation/Foundation.h>
#import "config.h"
NS_ASSUME_NONNULL_BEGIN
@protocol PBLBackend
- (void)statusWithReply:(void (^)(NSString*))reply;
- (void)request:(NSData*)request reply:(void (^)(NSData* _Nullable, NSString* _Nullable))reply;
@end
NS_ASSUME_NONNULL_END
