"""Check two CTIs in one RTLD_GLOBAL process; use mock/simulator CTIs only.

Usage: python3 runtime_isolation.py COPY_CTI QUEUED_CTI [--reverse]
This initializes producers but never opens a camera or stream.
"""
import argparse
import ctypes as c
import json

H, Z, Q, I = c.c_void_p, c.c_size_t, c.c_uint64, c.c_int32
P = c.POINTER

class Entry(c.Structure):
    _fields_ = [('Address', Q), ('pBuffer', H), ('Size', Z)]

def load(path):
    lib = c.CDLL(path, mode=c.RTLD_GLOBAL)
    signatures = {
        'GCInitLib': [], 'GCCloseLib': [], 'TLOpen': [P(H)], 'TLClose': [H],
        'TLGetInfo': [H, I, P(I), H, P(Z)],
        'GCReadPort': [H, Q, H, P(Z)],
        'GCReadPortStacked': [H, P(Entry), P(Z)],
        'GCGetLastError': [P(I), H, P(Z)],
        'GCGetPortURL': [H, H, P(Z)],
    }
    for name, args in signatures.items():
        getattr(lib, name).argtypes = args
        getattr(lib, name).restype = I
    return lib

def ok(result):
    assert result == 0, result

def last_error(lib):
    code = I(); buf = c.create_string_buffer(4096); size = Z(len(buf))
    ok(lib.GCGetLastError(c.byref(code), buf, c.byref(size)))
    return code.value

def identity(lib, handle):
    buf = c.create_string_buffer(256); size = Z(len(buf)); typ = I()
    ok(lib.TLGetInfo(handle, 0, c.byref(typ), buf, c.byref(size)))
    return buf.value.decode()

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('copy_cti'); parser.add_argument('queued_cti')
parser.add_argument('--reverse', action='store_true')
args = parser.parse_args()
paths = {'copy': args.copy_cti, 'queued': args.queued_cti}
order = ['queued', 'copy'] if args.reverse else ['copy', 'queued']
libs = {name: load(paths[name]) for name in order}
handles = {}; initialized = []
try:
    first, second = order
    ok(libs[first].GCInitLib()); initialized.append(first)
    handle = H(); ok(libs[first].TLOpen(c.byref(handle))); handles[first] = handle
    value = c.create_string_buffer(128)
    entry = Entry(0, c.cast(value, H), len(value)); count = Z(1)
    # Must consult the second CTI's initialization state, not the first's.
    assert libs[second].GCReadPortStacked(handle, c.byref(entry), c.byref(count)) == -1002
    ok(libs[second].GCInitLib()); initialized.append(second)
    handle = H(); ok(libs[second].TLOpen(c.byref(handle))); handles[second] = handle
    identities = {name: identity(libs[name], handles[name]) for name in order}
    assert identities['copy'] != identities['queued'], identities
    for name in order:
        value = c.create_string_buffer(128)
        entry = Entry(0, c.cast(value, H), len(value)); count = Z(1)
        ok(libs[name].GCReadPortStacked(handles[name], c.byref(entry), c.byref(count)))
        assert value.value.decode() == identities[name]
    # Seed different errors and verify separate per-CTI local thread contexts.
    value = c.c_uint64(); size = Z(c.sizeof(value))
    assert libs[first].GCReadPort(handles[first], 0xfffffffffffffff0,
                                  c.byref(value), c.byref(size)) == -1015
    assert last_error(libs[first]) == -1015
    assert last_error(libs[second]) == -1002
    ok(libs[first].TLClose(handles.pop(first)))
    ok(libs[first].GCCloseLib()); initialized.remove(first)
    assert identity(libs[second], handles[second]) == identities[second]
    ok(libs[first].GCInitLib()); initialized.append(first)
    handle = H(); ok(libs[first].TLOpen(c.byref(handle))); handles[first] = handle
    assert identity(libs[first], handle) == identities[first]
    print(json.dumps({'result': 'PASS', 'load_order': order, 'identities': identities,
                      'checks': ['independent initialization', 'stacked reads',
                                 'error isolation', 'close/reinitialize isolation']}))
finally:
    for name, handle in handles.items():
        ok(libs[name].TLClose(handle))
    for name in reversed(initialized):
        ok(libs[name].GCCloseLib())
