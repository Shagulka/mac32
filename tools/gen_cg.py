#!/usr/bin/env python3
"""Generate src/cg_thunks.c: thunks for the CoreGraphics C API, read from the
macOS SDK headers. Types are mapped to call_sig codes (see thunks.c):

  o  CF/CG object refs        g  CGFloat           P  CGPoint/CGSize/CGVector
  R  CGRect                   A  CGAffineTransform B/b bool (arg/return)
  i  int32 and enums          u  uint32/size_t      l  64-bit integers
  d  double   f  float        p  pointers whose pointee has the same layout

Functions taking callbacks, varargs, or pointers to CGFloat/struct arrays are
skipped (they need hand-written conversion), as are names that already have a
hand-written thunk in src/. Run from the repo root: tools/gen_cg.py"""
import glob, os, re, subprocess

sdk = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip()
hdrs = sorted(glob.glob(sdk + '/System/Library/Frameworks/CoreGraphics.framework/Headers/*.h'))

# names that already have a thunk somewhere in src/ (hand-written wins)
have = set()
for f in glob.glob('src/*.c') + glob.glob('src/*.m'):
    if f.endswith('cg_thunks.c'):
        continue
    have |= set(re.findall(r'\b[GRD]\("_(CG\w+)"', open(f).read()))

INT32 = {'int', 'int32_t', 'CGError', 'CGWindowLevel', 'CFIndex', 'CGEventField', 'CGEventType',
         'CGEventTapLocation', 'CGEventSourceStateID', 'CGMouseButton', 'CGScrollEventUnit',
         'CGEventMouseSubtype', 'CGEventTapPlacement', 'CGEventTapOptions', 'CGFontPostScriptFormat',
         'CGTextDrawingMode', 'CGTextEncoding', 'CGLineJoin', 'CGLineCap', 'CGPathDrawingMode',
         'CGBlendMode', 'CGInterpolationQuality', 'CGColorRenderingIntent', 'CGPathElementType',
         'CGColorSpaceModel', 'CGImageAlphaInfo', 'CGGradientDrawingOptions', 'CGPatternTiling',
         'CGPDFBox', 'CGConfigureOption', 'CGCaptureOptions', 'CGWindowImageOption',
         'CGWindowListOption', 'CGImageByteOrderInfo', 'CGImagePixelFormatInfo', 'CGPDFAccessPermissions'}
UINT32 = {'unsigned int', 'uint32_t', 'size_t', 'CGDirectDisplayID', 'CGWindowID', 'CGBitmapInfo',
          'CGDisplayCount', 'CGOpenGLDisplayMask', 'CGEventMask', 'CGButtonCount', 'CGWheelCount',
          'CGRefreshRate', 'CGDisplayFadeReservationToken', 'CGDisplayReservationInterval',
          'CGDisplayConfigRef', 'CGGlyph', 'CGFontIndex', 'CGKeyCode', 'CGCharCode', 'uint16_t',
          'unsigned short', 'uint8_t', 'CGSRegionObj', 'boolean_t'}
INT64 = {'int64_t', 'uint64_t', 'CGEventFlags', 'CGEventTimestamp', 'CGEventSourceKeyboardType'}
SAME_LAYOUT_PTR = {'void', 'char', 'unsigned char', 'uint8_t', 'uint16_t', 'uint32_t', 'int', 'int32_t',
                   'UniChar', 'CGGlyph', 'CGDirectDisplayID', 'CGWindowID', 'CGDisplayCount', 'bool',
                   'CGDisplayFadeReservationToken', 'float', 'double', 'CGKeyCode', 'uint64_t', 'int64_t'}

def clean(t):
    t = re.sub(r'\b(cg_nullable|cg_nonnull|_Nullable|_Nonnull|__nullable|__nonnull|CF_RETURNS_RETAINED|'
               r'CF_RETURNS_NOT_RETAINED|CF_CONSUMED|CG_EXTERN|CG_PURE|CG_INLINE|CF_INLINE|CG_LOCAL|__attribute__\s*\(\(.*?\)\)|restrict|__restrict)\b',
               '', t)
    return re.sub(r'\s+', ' ', t).strip()

def code(t, ret=False):
    t = clean(t)
    if t == 'void':
        return 'v' if ret else None
    if t.endswith('*'):
        base = clean(t[:-1].replace('const ', ''))
        if base.endswith('*'):
            return 'X'
        return 'p' if base in SAME_LAYOUT_PTR else 'X'
    base = clean(t.replace('const ', ''))
    if base == 'CGFloat': return 'g'
    if base in ('CGPoint', 'CGSize', 'CGVector'): return 'P'
    if base == 'CGRect': return 'R'
    if base == 'CGAffineTransform': return 'A'
    if base == 'bool': return 'b' if ret else 'B'
    if base.endswith('Ref') or base in ('CFTypeRef', 'id'): return 'o'
    if base in INT32: return 'i'
    if base in UINT32: return 'u'
    if base in INT64: return 'l'
    if base in ('double', 'CFTimeInterval', 'CGGammaValue'): return 'd'
    if base == 'float': return 'f'
    return 'X'

decl = re.compile(r'CG_EXTERN\s+([^;{}()]+?)\s*\b(CG\w+)\s*\(([^;{}]*?)\)\s*([^;]*);', re.S)
rows, skipped = {}, []
for h in hdrs:
    src = re.sub(r'/\*.*?\*/|//[^\n]*', '', open(h).read(), flags=re.S)
    for m in decl.finditer(src):
        rett, name, args, tail = m.group(1), m.group(2), m.group(3), m.group(4)
        if name in have or name in rows:
            continue
        if re.search(r'API_UNAVAILABLE\s*\([^)]*macos|__IOS_PROHIBITED|UNAVAILABLE_ATTRIBUTE|OBSOLETE', tail + rett):
            continue
        if '...' in args or '(' in args or '^' in args:
            skipped.append(name); continue
        r = code(rett, ret=True)
        a = []
        if clean(args) not in ('', 'void'):
            for part in args.split(','):
                part = clean(part)
                part = re.sub(r'\s*\b\w+\s*$', '', part) if re.search(r'[\w*]\s+\**\w+$', part) else part
                part = re.sub(r'\[\s*\d*\s*\]$', '*', part)
                a.append(code(part))
        if r == 'X' or 'X' in a or None in a:
            skipped.append(name); continue
        rows[name] = r + ':' + ''.join(a)

consts = {  # geometry constants, written as i386 floats
    'CGPointZero': ('P', 'CGPointZero.x, CGPointZero.y'),
    'CGSizeZero': ('P', 'CGSizeZero.width, CGSizeZero.height'),
    'CGRectZero': ('R', 'CGRectZero.origin.x, CGRectZero.origin.y, CGRectZero.size.width, CGRectZero.size.height'),
    'CGRectNull': ('R', 'CGRectNull.origin.x, CGRectNull.origin.y, CGRectNull.size.width, CGRectNull.size.height'),
    'CGRectInfinite': ('R', 'CGRectInfinite.origin.x, CGRectInfinite.origin.y, CGRectInfinite.size.width, CGRectInfinite.size.height'),
    'CGAffineTransformIdentity': ('A', 'CGAffineTransformIdentity.a, CGAffineTransformIdentity.b, '
                                  'CGAffineTransformIdentity.c, CGAffineTransformIdentity.d, '
                                  'CGAffineTransformIdentity.tx, CGAffineTransformIdentity.ty'),
}
out = ['// Generated by tools/gen_cg.py from the macOS SDK CoreGraphics headers. Do not edit.',
       '#include "thunks.h"', '#include <CoreGraphics/CoreGraphics.h>', '',
       '#pragma clang diagnostic ignored "-Wdeprecated-declarations"', '']
for n, (k, vals) in consts.items():
    if n in have:
        continue
    cnt = vals.count(',') + 1
    out.append(f'static void init_{n}(g32 a) {{ float v[{cnt}] = {{ {vals} }}; memcpy(G2H(a), v, sizeof v); }}')
out.append('')
out.append('static struct hfn cg[] = {')
for n, (k, vals) in consts.items():
    if n not in have:
        out.append(f'\tD("_{n}", {4 * (vals.count(",") + 1)}, init_{n}),')
for n in sorted(rows):
    out.append(f'\tG("_{n}", {n}, "{rows[n]}"),')
out.append('};')
out.append('')
out.append('void thunks_cg_init(void) { REGISTER(cg); }')
open('src/cg_thunks.c', 'w').write('\n'.join(out) + '\n')
print(f'{len(rows)} functions written, {len(skipped)} skipped (callbacks, arrays, unknown types)')
