/*
 * The AppKit headers sdkgen parses, which is the framework's own umbrella and
 * nothing else.
 *
 * AppKit.h imports every AppKit header but five, none of which declares a
 * function or a variable: NSNibConnector.h, NSNibControlConnector.h and
 * NSNibOutletConnector.h declare classes, whose exports come from the .tbd
 * with no declaration needed, and NSFileWrapper.h and NSSpellServer.h only
 * warn that the class moved to Foundation, whose umbrella AppKit.h imports
 * anyway.  The umbrella reaches Foundation, and through ApplicationServices
 * CoreGraphics, CoreText, HIServices and the rest of the C frameworks, which
 * AppKit's own declarations are written in terms of.  It is parsed as
 * Objective-C, because AppKit's C interface takes and returns its classes,
 * and the method declarations beside it are not functions and are passed
 * over.  The private frameworks AppKit re-exports, UIFoundation and
 * CollectionViewCore, have their public declarations in AppKit's own headers,
 * NSAttributedString.h, NSParagraphStyle.h and
 * NSCollectionViewCompositionalLayout.h among them.
 */
#import <AppKit/AppKit.h>
