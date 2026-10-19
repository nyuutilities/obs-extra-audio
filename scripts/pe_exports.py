"""Extract PE exports into a .def file without third-party packages."""
from pathlib import Path
import struct
import sys


def exports(path):
    data = Path(path).read_bytes()
    u16 = lambda offset: struct.unpack_from("<H", data, offset)[0]
    u32 = lambda offset: struct.unpack_from("<I", data, offset)[0]
    pe = u32(0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Not a PE file")
    opt = pe + 24
    directory = opt + (112 if u16(opt) == 0x20B else 96)
    sections = opt + u16(pe + 20)

    def offset(rva):
        for i in range(u16(pe + 6)):
            section = sections + i * 40
            size, address, raw_size, raw = struct.unpack_from("<IIII", data, section + 8)
            if address <= rva < address + max(size, raw_size):
                return raw + rva - address
        raise ValueError(f"RVA {rva:x} not found")

    table = offset(u32(directory))
    names = offset(u32(table + 32))
    result = []
    for i in range(u32(table + 24)):
        name = offset(u32(names + i * 4))
        result.append(data[name:data.index(b"\0", name)].decode("ascii"))
    return result


if __name__ == "__main__":
    dll, destination = map(Path, sys.argv[1:])
    destination.write_text(f"LIBRARY {dll.name}\nEXPORTS\n" + "\n".join(exports(dll)) + "\n")
