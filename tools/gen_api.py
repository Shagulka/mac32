#!/usr/bin/env python3
"""Generate thunks for a C framework API, read from the macOS SDK headers:
  tools/gen_api.py cg   -> src/cg_thunks.c  (CoreGraphics)
  tools/gen_api.py cf   -> src/cf_thunks.c  (CoreFoundation)
Types are mapped to call_sig codes (see thunks.c):

  o  CF/CG object refs        g  CGFloat           P  CGPoint/CGSize/CGVector
  R  CGRect                   A  CGAffineTransform B/b bool (arg/return)
  i  int32 and enums          u  uint32/size_t      l  64-bit integers
  d  double   f  float        p  pointers whose pointee has the same layout
  N  CFRange                  S  const char * return (copied for the guest)

Functions taking callbacks, varargs, or pointers to CGFloat/struct arrays are
skipped (they need hand-written conversion), as are names that already have a
hand-written thunk in src/. Run from the repo root: tools/gen_cg.py"""
import glob, os, re, subprocess, sys

FW = {
    'cg': dict(dir='CoreGraphics', macro='CG_EXTERN', prefix='CG', out='src/cg_thunks.c', table='cg',
               include='#include <CoreGraphics/CoreGraphics.h>'),
    'cf': dict(dir='CoreFoundation', macro='CF_EXPORT', prefix='CF', out='src/cf_thunks.c', table='cf',
               include='#include <CoreFoundation/CoreFoundation.h>'),
    # Carbon-era APIs (CarbonCore, LaunchServices, HIToolbox, PrintCore, SC, ...):
    # only the names in tools/wanted.txt, plus prototypes the SDK no longer has
    # (tools/carbon_protos.h)
    'carbon': dict(dir=None, macro='extern', prefix=r'[A-Za-z_]', out='src/carbon_thunks.c', table='carbon',
                   include='#include <Carbon/Carbon.h>\n#include <SystemConfiguration/SystemConfiguration.h>\n'
                           '#include <ImageIO/ImageIO.h>\n#include "../tools/carbon_protos.h"'),
}
which = sys.argv[1] if len(sys.argv) > 1 else 'cg'
fw = FW[which]
sdk = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip()
if fw['dir']:
    hdrs = sorted(glob.glob(sdk + f'/System/Library/Frameworks/{fw["dir"]}.framework/Headers/*.h'))
    wanted = None
else:
    hdrs = sorted(glob.glob(sdk + '/System/Library/Frameworks/*.framework/Headers/*.h') +
                  glob.glob(sdk + '/System/Library/Frameworks/*.framework/Frameworks/*.framework/Headers/*.h'))
    hdrs = [h for h in hdrs if '/CoreGraphics.framework/' not in h and '/CoreFoundation.framework/' not in h]
    hdrs.append('tools/carbon_protos.h')
    wanted = set(open('tools/wanted.txt').read().split())
wanted_rx = re.compile(r'\b(' + '|'.join(sorted(wanted, key=len, reverse=True)) + r')\s*\(') if wanted else None

# names that already have a thunk somewhere in src/ (hand-written wins)
have = set()
for f in glob.glob('src/*.c') + glob.glob('src/*.m'):
    if f.endswith(fw['out'].split('/')[-1]):
        continue
    have |= set(re.findall(r'\b[GRD]\("_(' + fw['prefix'] + r'\w+)"', open(f).read()))

# CF_ENUM/CF_OPTIONS typedefs give each enum's width
ENUM_I, ENUM_U = set(), set()
for h in glob.glob(sdk + '/System/Library/Frameworks/CoreFoundation.framework/Headers/*.h') + hdrs:
    t = open(h, errors='replace').read()
    for base, name in re.findall(r'typedef\s+CF_(?:CLOSED_)?ENUM\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', t):
        (ENUM_U if base.startswith(('U', 'CFOption', 'unsigned', 'uint', 'CFStringEncoding')) else ENUM_I).add(name)
    for base, name in re.findall(r'typedef\s+CF_OPTIONS\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', t):
        ENUM_U.add(name)

INT32 = {'int', 'int32_t', 'SInt32', 'OSErr', 'OSStatus', 'short', 'signed short', 'signed char', 'SInt', 'SInt16', 'SInt8', 'long', 'CFComparisonResult', 'CGError', 'CGWindowLevel', 'CFIndex', 'CGEventField', 'CGEventType',
         'CGEventTapLocation', 'CGEventSourceStateID', 'CGMouseButton', 'CGScrollEventUnit',
         'CGEventMouseSubtype', 'CGEventTapPlacement', 'CGEventTapOptions', 'CGFontPostScriptFormat',
         'CGTextDrawingMode', 'CGTextEncoding', 'CGLineJoin', 'CGLineCap', 'CGPathDrawingMode',
         'CGBlendMode', 'CGInterpolationQuality', 'CGColorRenderingIntent', 'CGPathElementType',
         'CGColorSpaceModel', 'CGImageAlphaInfo', 'CGGradientDrawingOptions', 'CGPatternTiling',
         'CGPDFBox', 'CGConfigureOption', 'CGCaptureOptions', 'CGWindowImageOption',
         'CGWindowListOption', 'CGImageByteOrderInfo', 'CGImagePixelFormatInfo', 'CGPDFAccessPermissions'}
UINT32 = {'unsigned int', 'uint32_t', 'size_t', 'unsigned char', 'unsigned short', 'FourCharCode', 'OSType',
          'ResType', 'OptionBits', 'ItemCount', 'ByteCount', 'UniCharCount', 'TextEncoding', 'LangCode', 'RegionCode', 'CFHashCode', 'CFOptionFlags', 'CFTypeID', 'UInt32',
          'UInt16', 'UInt8', 'UniChar', 'UTF32Char', 'CFStringEncoding', 'CFStringEncodings', 'unsigned long', 'CGDirectDisplayID', 'CGWindowID', 'CGBitmapInfo',
          'CGDisplayCount', 'CGOpenGLDisplayMask', 'CGEventMask', 'CGButtonCount', 'CGWheelCount',
          'CGRefreshRate', 'CGDisplayFadeReservationToken', 'CGDisplayReservationInterval',
          'CGDisplayConfigRef', 'CGGlyph', 'CGFontIndex', 'CGKeyCode', 'CGCharCode', 'uint16_t',
          'unsigned short', 'uint8_t', 'CGSRegionObj', 'boolean_t'}
INT64 = {'int64_t', 'uint64_t', 'SInt64', 'UInt64', 'long long', 'unsigned long long', 'CGEventFlags', 'CGEventTimestamp', 'CGEventSourceKeyboardType'}
SAME_LAYOUT_PTR = {'char', 'FSRef', 'PMRect', 'PMResolution', 'PMPaperMargins', 'OSType', 'OSErr', 'SInt16',
                   'ScriptCode', 'TextEncoding', 'EventTypeSpec', 'RGBColor', 'Str255', 'unsigned char', 'UniChar', 'Byte', 'HFSUniStr255', 'FSSpec', 'ProcessSerialNumber', 'UInt8', 'UInt16', 'UInt32', 'SInt32', 'Boolean', 'UTF32Char', 'CFStringEncoding', 'unsigned char', 'uint8_t', 'uint16_t', 'uint32_t', 'int', 'int32_t',
                   'UniChar', 'CGGlyph', 'CGDirectDisplayID', 'CGWindowID', 'CGDisplayCount', 'bool',
                   'CGDisplayFadeReservationToken', 'float', 'double', 'CGKeyCode', 'uint64_t', 'int64_t'}

# plain typedefs (typedef UInt32 OptionBits;) to resolve Carbon's integer aliases
ALIAS = {}
for h in hdrs + glob.glob(sdk + '/usr/include/MacTypes.h'):
    try: txt = re.sub(r'/\*.*?\*/|//[^\n]*', '', open(h, errors='replace').read(), flags=re.S)
    except OSError: continue
    for base, name in re.findall(r'typedef\s+((?:unsigned\s+|signed\s+|const\s+)*[A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*;', txt):
        ALIAS.setdefault(name, base.strip())
    for base, name in re.findall(r'typedef\s+CF_(?:CLOSED_)?ENUM\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', txt):
        ALIAS.setdefault(name, base)
    for base, name in re.findall(r'typedef\s+CF_OPTIONS\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', txt):
        ALIAS.setdefault(name, base)

# typedef struct OpaqueX *X;  -> an object handle
OPAQUE = set()
for h in hdrs:
    try: txt = open(h, errors='replace').read()
    except OSError: continue
    OPAQUE |= set(re.findall(r'typedef\s+(?:const\s+)?struct\s+\w+\s*\*\s*(\w+)\s*;', txt))
    OPAQUE |= set(re.findall(r'typedef\s+(?:const\s+)?void\s*\*\s*(\w+)\s*;', txt))

def resolve(t):
    seen = 0
    while t in ALIAS and seen < 12 and ALIAS[t] != t:
        t = ALIAS[t]; seen += 1
    return t

def clean(t):
    t = re.sub(r'\b(cg_nullable|cg_nonnull|_Nullable|_Nonnull|__nullable|__nonnull|CF_RETURNS_RETAINED|'
               r'CF_RETURNS_NOT_RETAINED|CF_CONSUMED|CG_EXTERN|CF_EXPORT|APPKIT_EXTERN|FOUNDATION_EXPORT|IMAGEIO_EXTERN|_iio_Nullable|_iio_Nonnull|IMAGEIO_AVAILABLE_STARTING\([^)]*\)|CF_NOESCAPE|CF_REFINED_FOR_SWIFT|CG_PURE|CG_INLINE|CF_INLINE|CG_LOCAL|__attribute__\s*\(\(.*?\)\)|restrict|__restrict)\b',
               '', t)
    t = re.sub(r'\bstruct\s+', '', t)
    return re.sub(r'\s+', ' ', t).strip()

def code(t, ret=False):
    t = clean(t)
    if t == 'void':
        return 'v' if ret else None
    if t.endswith('*'):
        base = clean(t[:-1].replace('const ', ''))
        if not ret:
            if base in ('HIRect', 'CGRect'): return 'r'
            if base in ('ItemCount', 'ByteCount', 'Size', 'UniCharCount', 'CFIndex', 'unsigned long', 'long',
                        'UniCharArrayOffset'): return 'W'
            if not t.startswith('const') and (base.endswith('Ref') or (which == 'carbon' and base in OPAQUE)):
                return 'O'
            if base in ('FSSpec', 'Str255', 'Str63', 'Str31', 'Str15'): return 'p'
        if base.endswith('*'):
            return 'X'
        if ret:
            return 'S' if base == 'char' else 'X'	# pointers into host storage
        if base == 'void':
            return 'p' if which == 'cg' else 'X'	# CF: values are objects or raw, can't tell
        return 'p' if base in SAME_LAYOUT_PTR else 'X'
    base = clean(t.replace('const ', ''))
    c = basic(base, ret)
    if c == 'X' and which == 'carbon':
        if base in OPAQUE: return 'o'
        r = resolve(base)
        c = basic(r, ret)
        if c == 'X' and r in OPAQUE: return 'o'
    return c

def basic(base, ret):
    base = {'signed int': 'int', 'signed short': 'short', 'signed long': 'long', 'unsigned': 'unsigned int',
            '__uint32_t': 'uint32_t', '__int32_t': 'int32_t', '__uint16_t': 'uint16_t', '__uint8_t': 'uint8_t',
            '__int64_t': 'int64_t', '__uint64_t': 'uint64_t', '__darwin_size_t': 'size_t'}.get(base, base)
    if base == 'CGFloat': return 'g'
    if base in ('Str255', 'ConstStr255Param', 'ConstStringPtr', 'StringPtr', 'FSSpecPtr', 'ConstStr63Param',
                'ConstStr31Param'): return 'p'
    if base in ('NSInteger', 'kern_return_t', 'mach_error_t'): return 'i'
    if base in ('NSUInteger', 'io_connect_t', 'io_object_t', 'io_service_t', 'mach_port_t', 'io_iterator_t'): return 'u'
    if base.startswith('NS') and base.endswith(' *'): return 'o'
    if base == 'CFRange': return 'N'
    if base == 'Boolean': return 'b' if ret else 'B'
    if base in ('CFAbsoluteTime',): return 'd'
    if base in ENUM_I: return 'i'
    if base in ENUM_U: return 'u'
    if base in ('CGPoint', 'CGSize', 'CGVector'): return 'P'
    if base == 'CGRect': return 'R'
    if base == 'CGAffineTransform': return 'A'
    if base == 'bool': return 'b' if ret else 'B'
    if base.endswith('Ref') or base in ('CFTypeRef', 'id'): return 'o'
    if base in INT32: return 'i'
    if base in UINT32: return 'u'
    if base in INT64: return 'l'
    if base in ('double', 'CFTimeInterval', 'CGGammaValue', 'EventTime', 'Float64'): return 'd'
    if base == 'float': return 'f'
    return 'X'

# a declaration at the start of a line, with or without the export macro
decl = re.compile(r'(?:^|\n)[ \t]*(?:' + fw['macro'] + r'\s+)?([A-Za-z_][^;{}()=#\n]*?(?:\n[^;{}()=#\n]*?)?)\s*\b('
                  + fw['prefix'] + r'\w+)\s*\(([^;{}]*?)\)\s*([^;{}]*);', re.S)
rows, skipped, why = {}, [], {}
def chunks(src):
    # the whole header, or (carbon) just the declarations that name a wanted
    # function: a few lines around each "name(" keeps the regex fast
    if wanted is None:
        yield src
        return
    for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', src):
        if m.group(1) in wanted:
            a = src.rfind(';', 0, m.start()) + 1
            a = max(a, src.rfind('}', 0, m.start()) + 1, src.rfind('#', 0, m.start()))
            if src[a:a + 1] == '#':
                a = src.find('\n', a) + 1
            b = src.find(';', m.end())
            if b > 0 and b - a < 2000:
                yield '\n' + src[a:b + 1]

for h in hdrs:
    raw = open(h, errors='replace').read()
    if wanted is not None and not any(w in raw for w in wanted_rx.findall(raw)):
        continue
    src = re.sub(r'/\*.*?\*/|//[^\n]*', '', raw, flags=re.S)
    for m in (mm for c in chunks(src) for mm in decl.finditer(c)):
        rett, name, args, tail = m.group(1), m.group(2), m.group(3), m.group(4)
        if re.search(r'\b(typedef|return|define|if|else|struct\s*\{)\b', rett):
            continue
        if name in have or name in rows:
            continue
        if wanted is not None and name not in wanted:
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
            bad = [clean(rett)] if r == 'X' else []
            parts = [clean(x) for x in args.split(',')]
            bad += [parts[i] for i, c in enumerate(a) if c in ('X', None) and i < len(parts)]
            skipped.append(name)
            why[name] = bad
            continue
        rows[name] = r + ':' + ''.join(a)

consts = {} if which != 'cg' else {  # geometry constants, written as i386 floats
    'CGPointZero': ('P', 'CGPointZero.x, CGPointZero.y'),
    'CGSizeZero': ('P', 'CGSizeZero.width, CGSizeZero.height'),
    'CGRectZero': ('R', 'CGRectZero.origin.x, CGRectZero.origin.y, CGRectZero.size.width, CGRectZero.size.height'),
    'CGRectNull': ('R', 'CGRectNull.origin.x, CGRectNull.origin.y, CGRectNull.size.width, CGRectNull.size.height'),
    'CGRectInfinite': ('R', 'CGRectInfinite.origin.x, CGRectInfinite.origin.y, CGRectInfinite.size.width, CGRectInfinite.size.height'),
    'CGAffineTransformIdentity': ('A', 'CGAffineTransformIdentity.a, CGAffineTransformIdentity.b, '
                                  'CGAffineTransformIdentity.c, CGAffineTransformIdentity.d, '
                                  'CGAffineTransformIdentity.tx, CGAffineTransformIdentity.ty'),
}
out = [f'// Generated by tools/gen_api.py {which} from the macOS SDK {fw["dir"]} headers. Do not edit.',
       '#include "thunks.h"', fw['include'], '',
       '#pragma clang diagnostic ignored "-Wdeprecated-declarations"', '']
for n, (k, vals) in consts.items():
    if n in have:
        continue
    cnt = vals.count(',') + 1
    out.append(f'static void init_{n}(g32 a) {{ float v[{cnt}] = {{ {vals} }}; memcpy(G2H(a), v, sizeof v); }}')
out.append('')
out.append(f'static struct hfn {fw["table"]}[] = {{')
for n, (k, vals) in consts.items():
    if n not in have:
        out.append(f'\tD("_{n}", {4 * (vals.count(",") + 1)}, init_{n}),')
for n in sorted(rows):
    out.append(f'\tG("_{n}", {n}, "{rows[n]}"),')
out.append('};')
out.append('')
out.append(f'void thunks_{fw["table"]}_init(void) {{ REGISTER({fw["table"]}); }}')
open(fw['out'], 'w').write('\n'.join(out) + '\n')

# drop entries the 64-bit SDK does not declare or marks unavailable
for _ in range(6):
    r = subprocess.run(['clang', '-arch', 'x86_64', '-fsyntax-only', '-w', '-Isrc', fw['out']], capture_output=True, text=True)
    bad = set(re.findall(r"(?:undeclared (?:function|identifier)|is unavailable|incompatible pointer|"
                         r"incompatible integer).*?'(\w+)'", r.stderr))
    bad |= set(re.findall(r"'(\w+)' is unavailable", r.stderr))
    if not bad:
        break
    lines = open(fw['out']).read().split('\n')
    lines = [l for l in lines if not any(f'"_{b}"' in l for b in bad)]
    open(fw['out'], 'w').write('\n'.join(lines))
    for b in bad: rows.pop(b, None)
if '--why' in sys.argv:
    for n in sorted(why): print(f'  skip {n}: {why[n]}')
print(f'{len(rows)} functions written, {len(skipped)} skipped (callbacks, arrays, unknown types)')
