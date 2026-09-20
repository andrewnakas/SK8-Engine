// HTTPS download for iOS.
//
// Every other platform's DownloadToFile either shells out to curl or asks the
// app shell to do it. iOS can do neither: an app cannot spawn a subprocess at
// all, and there is no Java activity to delegate to. So the title update row
// simply reported "downloading is not supported on iOS" and the player had to
// copy the package in over file sharing - which is exactly the step that a
// launcher menu exists to remove.
//
// NSURLSession is the answer, but it is asynchronous and the installer calls
// this from its own worker thread expecting a blocking call. A semaphore
// bridges the two; the caller is never the UI thread, so blocking it is safe.

#include "skate3_ios_download.h"

#import <Foundation/Foundation.h>

#include <atomic>
#include <string>

#include <rex/logging.h>

// Delegate at file scope: an Objective-C class cannot be declared inside a C++
// namespace. Progress is reported through the same two atomics the other
// platforms use, so the installer's progress bar needs no platform knowledge.
@interface Skate3DownloadDelegate : NSObject <NSURLSessionDownloadDelegate>
@property(nonatomic, assign) std::atomic<uint64_t>* copiedBytes;
@property(nonatomic, assign) std::atomic<uint64_t>* totalBytes;
@property(nonatomic, strong) NSURL* destination;
@property(nonatomic, strong) NSError* failure;
@property(nonatomic, strong) dispatch_semaphore_t done;
@end

@implementation Skate3DownloadDelegate

- (void)URLSession:(NSURLSession*)session
                 downloadTask:(NSURLSessionDownloadTask*)downloadTask
                 didWriteData:(int64_t)bytesWritten
            totalBytesWritten:(int64_t)totalBytesWritten
    totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
  if (self.copiedBytes != nullptr) {
    self.copiedBytes->store(static_cast<uint64_t>(totalBytesWritten > 0 ? totalBytesWritten : 0));
  }
  // NSURLSessionTransferSizeUnknown (-1) when the server sends no
  // Content-Length. Left at zero in that case rather than stored as a huge
  // unsigned number, which the progress bar would read as "nearly done".
  if (self.totalBytes != nullptr && totalBytesExpectedToWrite > 0) {
    self.totalBytes->store(static_cast<uint64_t>(totalBytesExpectedToWrite));
  }
}

- (void)URLSession:(NSURLSession*)session
                 downloadTask:(NSURLSessionDownloadTask*)downloadTask
    didFinishDownloadingToURL:(NSURL*)location {
  // The temporary file is deleted the moment this returns, so it must be moved
  // here and not later.
  NSError* error = nil;
  NSFileManager* fm = [NSFileManager defaultManager];
  [fm removeItemAtURL:self.destination error:nil];
  if (![fm moveItemAtURL:location toURL:self.destination error:&error]) {
    self.failure = error;
  }
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  if (error != nil && self.failure == nil) {
    self.failure = error;
  }
  // An HTTP error is not an NSError: the transfer succeeded and delivered the
  // error page. Without this a 404 would be moved into place as the title
  // update and fail much later, somewhere far less informative.
  if (self.failure == nil && [task.response isKindOfClass:[NSHTTPURLResponse class]]) {
    const NSInteger status = [(NSHTTPURLResponse*)task.response statusCode];
    if (status < 200 || status >= 300) {
      self.failure = [NSError errorWithDomain:@"skate3"
                                         code:status
                                     userInfo:@{
                                       NSLocalizedDescriptionKey : [NSString
                                           stringWithFormat:@"the server answered HTTP %ld",
                                                            (long)status]
                                     }];
      [[NSFileManager defaultManager] removeItemAtURL:self.destination error:nil];
    }
  }
  dispatch_semaphore_signal(self.done);
}

@end

namespace skate3 {

bool IosDownloadToFile(const std::string& url, const std::filesystem::path& destination,
                       std::atomic<uint64_t>& copied_bytes, std::atomic<uint64_t>& total_bytes,
                       std::string& error) {
  error.clear();
  @autoreleasepool {
    NSURL* source = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (source == nil) {
      error = "the configured download URL is not valid";
      return false;
    }

    Skate3DownloadDelegate* delegate = [[Skate3DownloadDelegate alloc] init];
    delegate.copiedBytes = &copied_bytes;
    delegate.totalBytes = &total_bytes;
    delegate.destination =
        [NSURL fileURLWithPath:[NSString stringWithUTF8String:destination.string().c_str()]];
    delegate.done = dispatch_semaphore_create(0);

    NSURLSessionConfiguration* config = [NSURLSessionConfiguration defaultSessionConfiguration];
    // A phone on a slow connection should not fail a 300 MB transfer because
    // one response header was slow; the resource timeout is the one that
    // bounds the whole download.
    config.timeoutIntervalForRequest = 60.0;
    config.timeoutIntervalForResource = 60.0 * 60.0;
    config.allowsCellularAccess = YES;

    // A private queue, not the main one: this call blocks its caller until the
    // semaphore is signalled, and delivering the completion on the main queue
    // while the caller holds it would deadlock if the caller ever were the UI
    // thread.
    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1;
    NSURLSession* session = [NSURLSession sessionWithConfiguration:config
                                                         delegate:delegate
                                                    delegateQueue:queue];
    REXLOG_WARN("Skate 3: downloading {} to {}", url, destination.string());
    NSURLSessionDownloadTask* task = [session downloadTaskWithURL:source];
    [task resume];
    dispatch_semaphore_wait(delegate.done, DISPATCH_TIME_FOREVER);
    [session finishTasksAndInvalidate];

    if (delegate.failure != nil) {
      error = [[delegate.failure localizedDescription] UTF8String];
      REXLOG_WARN("Skate 3: download failed: {}", error);
      return false;
    }
    REXLOG_WARN("Skate 3: download finished ({} bytes)", copied_bytes.load());
    return true;
  }
}

}  // namespace skate3
