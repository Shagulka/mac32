#!/usr/bin/env python3
"""Generate src/steam_ifaces.c: per Steam interface version, the methods in
vtable order with mac32 signature strings (see thunks.c), from steam_api.json.

Arg codes:  o this, B bool, i/u 32-bit, l 64-bit (incl. CSteamID), f, d,
            p pointer passed through, F function pointer (passed as NULL),
            X not bridged.
Return:     v b i u l f d, S const char*, I interface (by version string), X."""
import json, re, sys

files = ['third_party/steam_api_1.55.json', 'third_party/steam_api_1.53.json']
ifaces = {}          # version -> (classname, methods)
typedefs, enums, structs = {}, set(), {}
for f in files:
    d = json.load(open(f))
    for t in d['typedefs']: typedefs.setdefault(t['typedef'], t['type'])
    for e in d['enums']: enums.add(e['enumname'])
    for i in d['interfaces']:
        for e in i.get('enums', []): enums.add(e['enumname']); enums.add(i['classname'] + '::' + e['enumname'])
    for s in d['structs'] + d['callback_structs']:
        structs[s['struct']] = s
    for i in d['interfaces']:
        v = i.get('version_string') or ('SteamClient020' if i['classname'] == 'ISteamClient' else None)
        if v and v not in ifaces: ifaces[v] = (i['classname'], i['methods'])

INT = {'int', 'signed char', 'short', 'char', 'intptr_t', 'int32', 'int16', 'int8'}
UINT = {'unsigned int', 'unsigned char', 'unsigned short', 'uint32', 'uint16', 'uint8'}
I64 = {'long long', 'unsigned long long', 'CSteamID', 'CGameID', 'int64', 'uint64'}
# typedefs that are pointer-sized on the 64-bit side but 32-bit in the guest
PTRSIZED = {'intp', 'uintp'}

def base(t):
    t = t.replace('const ', '').strip()
    seen = 0
    while t in typedefs and seen < 10 and t not in PTRSIZED:
        t = typedefs[t].replace('const ', '').strip(); seen += 1
    return t

def struct_has_pointer(name, depth=0):
    s = structs.get(name)
    if not s or depth > 4: return False
    for f in s.get('fields', []):
        ft = f['fieldtype']
        if '*' in ft: return True
        b = base(ft)
        if b in structs and struct_has_pointer(b, depth + 1): return True
    return False

def arg_code(t):
    raw = t.strip()
    if re.fullmatch(r'(const )?char \*\*', raw): return 'Q'      # out: pointer to a string Steam owns
    if raw.count('*') + raw.count('&') >= 2 or '[' in raw: return 'X'
    if '*' in raw or '&' in raw:
        pointee = base(raw.replace('*', '').replace('&', '').strip())
        if pointee.startswith('ISteam'): return 'X'          # guest C++ objects (response interfaces)
        if pointee in structs and struct_has_pointer(pointee): return 'X'
        return 'p'
    b = base(raw)
    if b.startswith(('Fn', 'F')) and (b.startswith('Fn') or b.startswith('FSteam')) or b.endswith(('Hook_t', 'CallbackPointer')):
        return 'F'
    if b == 'bool': return 'B'
    if b in INT or b in PTRSIZED: return 'i'
    if b in UINT: return 'u'
    if b in I64: return 'l'
    if b == 'float': return 'f'
    if b == 'double': return 'd'
    if b in enums or b.split('::')[-1] in enums or b.startswith('E'): return 'i'
    return 'X'

def ret_code(t):
    raw = t.strip()
    if raw == 'void': return 'v'
    if '*' in raw:
        if 'char' in raw and raw.count('*') == 1: return 'S'
        if raw.replace('*', '').strip().startswith('ISteam') or raw.strip() == 'void *': return 'I'
        return 'X'
    c = arg_code(raw)
    return {'B': 'b', 'F': 'X'}.get(c, c)


# ---- vtable indices from the game's own 32-bit libsteam_api flat wrappers ----
import subprocess, os
LIB = os.path.expanduser('~/Library/Application Support/Steam/steamapps/common/Portal 2/bin/osx32/libsteam_api.dylib')
dis = subprocess.run(['otool', '-tV', LIB], capture_output=True, text=True).stdout.split('\n')
index = {}		# flat name -> vtable index
cur, vreg, done = None, None, False
for line in dis:
    m = re.match(r'^(_SteamAPI_\w+):$', line)
    if m: cur, vreg, done = m.group(1)[1:], None, False; continue
    if not cur or done: continue
    m = re.search(r'movl\s+\((%e\w\w)\), (%e\w\w)', line)
    if m and vreg is None: vreg = m.group(2); continue
    if vreg:
        m = re.search(r'(?:movl|calll|jmpl)\s+\*?(0x[0-9a-f]+|)\(' + re.escape(vreg) + r'\)', line)
        if m:
            index[cur] = int(m.group(1) or '0', 16) // 4
            done = True
lib_versions = set(re.findall(rb'(Steam[A-Za-z]+\d{3}|STEAM[A-Z_]+INTERFACE_VERSION\d{3})', open(LIB, 'rb').read()))
lib_versions = {v.decode() for v in lib_versions}
print(f'{len(index)} flat wrappers decoded', file=sys.stderr)

# private (STEAM_PRIVATE_API) slots the JSON leaves out, by interface and index
PRIVATE = {
    'ISteamClient': {20: ('RunFrame', 'v:o', 0), 25: ('DEPRECATED_GetISteamUnifiedMessages', 'I:oiip', 3),
                     32: ('DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess', 'v:oF', 0),
                     33: ('DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess', 'v:oF', 0),
                     34: ('Set_SteamAPI_CCheckCallbackRegisteredInProcess', 'v:oF', 0)},
    'ISteamUtils': {7: ('GetCSERIPPort', 'b:opp', 0), 14: ('RunFrame', 'v:o', 0)},
    'ISteamGameServer': {0: ('InitGameServer', 'b:ouuuuup', 0)},
}
out = []
out.append('// Generated by tools/gen_steam.py from third_party/steam_api_*.json. Do not edit.')
out.append('#include "steam.h"\n')
names = []
for v, (cls, methods) in sorted(ifaces.items()):
    sym = re.sub(r'\W', '_', v)
    # place each method at the index libsteam_api uses; if this version is not
    # the one libsteam_api was built with, fall back to JSON order (and say so)
    slots = {}
    trusted = v in lib_versions or cls == 'ISteamClient'
    for k, m in enumerate(methods):
        idx = index.get(m['methodname_flat']) if trusted else None
        if idx is None:
            if trusted: print(f'warning: {v}.{m["methodname"]}: no flat wrapper, using JSON order', file=sys.stderr)
            idx = k
        args = ''.join(arg_code(p['paramtype']) for p in m['params'])
        ver = next((j for j, p in enumerate(m['params']) if p['paramname'] in ('pchVersion', 'version')), -1)
        slots[idx] = (m['methodname'], f'{ret_code(m["returntype"])}:o{args}', ver + 1 if ver >= 0 else 0)
    if trusted:
        for idx, row in PRIVATE.get(cls, {}).items():
            slots.setdefault(idx, row)
    n = max(slots) + 1 if slots else 0
    if trusted and cls == 'ISteamClient': n = max(n, 42)	# DestroyAllInterfaces is last
    rows = []
    for idx in range(n):
        name, sig, ver = slots.get(idx, (f'private_{idx}', 'X:o', 0))
        if cls == 'ISteamClient' and idx == 41 and idx not in slots: name, sig = 'DestroyAllInterfaces', 'v:o'
        rows.append(f'\t{{ "{name}", "{sig}", {ver} }},	/* {idx} */')
    out.append(f'static const struct smethod m_{sym}[] = {{\n' + '\n'.join(rows) + '\n};')
    names.append(f'\t{{ "{cls}", "{v}", {n}, m_{sym}, {1 if trusted else 0} }},')
# interfaces not in the JSON: from the public Steamworks headers
out.append('''static const struct smethod m_SteamGameCoordinator001[] = {
	{ "SendMessage", "i:oupu", 0 },
	{ "IsMessageAvailable", "b:op", 0 },
	{ "RetrieveMessage", "i:oppup", 0 },
};''')
names.append('\t{ "ISteamGameCoordinator", "SteamGameCoordinator001", 3, m_SteamGameCoordinator001, 1 },')
out.append('''static const struct smethod m_SteamGameStats001[] = {
	{ "GetNewSession", "l:oiiiu", 0 },
	{ "EndSession", "l:olui", 0 },
	{ "AddSessionAttributeInt", "i:olpi", 0 },
	{ "AddSessionAttributeString", "i:olpp", 0 },
	{ "AddSessionAttributeFloat", "i:olpf", 0 },
	{ "AddNewRow", "i:oplp", 0 },
	{ "CommitRow", "i:ol", 0 },
	{ "CommitOutstandingRows", "i:ol", 0 },
	{ "AddRowAttributeInt", "i:olpi", 0 },
	{ "AddRowAtributeString", "i:olpp", 0 },
	{ "AddRowAttributeFloat", "i:olpf", 0 },
	{ "AddSessionAttributeInt64", "i:olpl", 0 },
	{ "AddRowAttributeInt64", "i:olpl", 0 },
};''')
names.append('\t{ "ISteamGameStats", "SteamGameStats001", 13, m_SteamGameStats001, 1 },')
out.append('const struct siface steam_ifaces[] = {\n' + '\n'.join(names) + '\n\t{ 0 }\n};')
open('src/steam_ifaces.c', 'w').write('\n'.join(out) + '\n')
nx = sum(r.count('"X') + r.count('X",') for r in out)
print(f'{len(names)} interfaces written', file=sys.stderr)
