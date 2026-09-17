/*
 * The CoreGraphics headers sdkgen parses, which is the framework's own
 * umbrella: CoreGraphics.h includes every CG*.h the framework has.
 *
 * It is parsed as C.  Nothing in CoreGraphics's interface is Objective-C, and
 * the x86 programs native mode runs, an AppKit program's drawRect: included,
 * call it through that C interface.  CGFloat is double on both architectures,
 * so CGPoint, CGSize, CGRect and CGAffineTransform are structures of doubles
 * alike on both, and cross by value as the ABI engine lays them out.
 */
#include <CoreGraphics/CoreGraphics.h>
