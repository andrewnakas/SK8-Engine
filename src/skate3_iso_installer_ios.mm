// iOS file pickers for the install wizards.
//
// The wizard calls these from the UI thread and expects a path back before it
// returns, the same shape NSOpenPanel's runModal provides on macOS. UIKit has
// no modal equivalent: UIDocumentPickerViewController is presented and reports
// through a delegate, and blocking the main thread outright would deadlock the
// very runloop the picker needs. So the wait below pumps a nested runloop,
// which is what runModal does internally on the Mac.

#include <filesystem>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace {

// A file chosen out of Files is security-scoped: it lives outside our sandbox
// and is only readable between start/stopAccessingSecurityScopedResource. The
// installer reads it long after the picker returns, so access is started here
// and deliberately held for the rest of the process's life - a 7 GB ISO is far
// too large to copy into the sandbox just to read it once.
NSMutableArray<NSURL*>* RetainedScopedURLs() {
  static NSMutableArray<NSURL*>* urls = [[NSMutableArray alloc] init];
  return urls;
}

UIViewController* PresentingViewController() {
  UIWindow* window = nil;
  if (SDL_Window* sdl_window = SDL_GetKeyboardFocus()) {
    SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
    window = (__bridge UIWindow*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
  }
  if (!window) {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
      if ([scene isKindOfClass:[UIWindowScene class]]) {
        for (UIWindow* candidate in ((UIWindowScene*)scene).windows) {
          if (candidate.isKeyWindow) {
            window = candidate;
            break;
          }
        }
      }
      if (window) {
        break;
      }
    }
  }
  UIViewController* controller = window.rootViewController;
  // Present from the topmost controller; presenting on one that is already
  // covered is ignored by UIKit.
  while (controller.presentedViewController) {
    controller = controller.presentedViewController;
  }
  return controller;
}

}  // namespace

// The delegate has to be a real object with a lifetime, so it is declared at
// file scope and kept alive by the picker until the interaction finishes.
@interface Skate3DocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, assign) BOOL finished;
@property(nonatomic, strong) NSURL* selectedURL;
@end

@implementation Skate3DocumentPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  self.selectedURL = urls.firstObject;
  self.finished = YES;
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  self.selectedURL = nil;
  self.finished = YES;
}
@end

namespace skate3 {

namespace {

std::filesystem::path PickFileIOS(NSArray<UTType*>* content_types) {
  @autoreleasepool {
    UIViewController* presenter = PresentingViewController();
    if (!presenter) {
      return {};
    }

    Skate3DocumentPickerDelegate* delegate = [[Skate3DocumentPickerDelegate alloc] init];
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:content_types
                                                                   asCopy:NO];
    picker.delegate = delegate;
    picker.allowsMultipleSelection = NO;
    picker.modalPresentationStyle = UIModalPresentationFormSheet;
    [presenter presentViewController:picker animated:YES completion:nil];

    // Nested runloop, mirroring runModal: the picker is driven by the main
    // runloop, so returning to it here is what lets the user interact at all.
    while (!delegate.finished) {
      [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                               beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }

    NSURL* url = delegate.selectedURL;
    if (!url || !url.isFileURL) {
      return {};
    }
    if ([url startAccessingSecurityScopedResource]) {
      [RetainedScopedURLs() addObject:url];
    }
    return std::string(url.path.UTF8String);
  }
}

NSArray<UTType*>* IsoContentTypes() {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  // .iso has no standard system UTI; typeWithFilenameExtension: returns the
  // dynamic type Files uses for it. UTTypeData keeps the picker from greying
  // out discs that arrived with no type information at all.
  if (UTType* iso = [UTType typeWithFilenameExtension:@"iso"]) {
    [types addObject:iso];
  }
  [types addObject:UTTypeData];
  return types;
}

}  // namespace

std::filesystem::path PickIsoFileIOS() { return PickFileIOS(IsoContentTypes()); }

std::filesystem::path PickTitleUpdateFileIOS() {
  // The title update package has no extension worth filtering on, so accept
  // any item and let the installer validate it.
  return PickFileIOS(@[ UTTypeItem, UTTypeData ]);
}

}  // namespace skate3
