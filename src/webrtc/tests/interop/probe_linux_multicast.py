#!/usr/bin/env python3
"""Standalone Linux probe for shared multicast sockets, packet metadata and interface enumeration.

This is not an mDNS service.
"""

import argparse
from contextlib import ExitStack
import secrets
import socket
import struct
import time


def receive(sock, expected, family, index):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        data, ancillary, flags, source = sock.recvmsg(9000, 1024)
        if data != expected:
            continue
        if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC):
            raise RuntimeError('Truncated multicast data/metadata')
        if source[1] != 5353:
            raise RuntimeError('mDNS source port is not 5353')
        if family == socket.AF_INET:
            packet_type, hop_type, level = 8, 2, socket.IPPROTO_IP  # Linux IP_PKTINFO / IP_TTL
        else:
            packet_type, hop_type, level = socket.IPV6_PKTINFO, socket.IPV6_HOPLIMIT, socket.IPPROTO_IPV6
        metadata = {kind: value for protocol, kind, value in ancillary if protocol == level}
        info = metadata.get(packet_type)
        actual_index = struct.unpack('I', info[:4] if family == socket.AF_INET else info[16:20])[0] if info else 0
        hops = struct.unpack('i', metadata.get(hop_type, b'\0' * 4))[0]
        if actual_index != index or hops != 255:
            raise RuntimeError(f'Multicast scope/hop mismatch: interface {actual_index}, hops {hops}')
        return
    raise RuntimeError('Multicast packet not observed within deadline')


def multicast(family, address, index):
    label = secrets.token_hex(16).encode('ascii')
    name = bytes([len(label)]) + label + b'\x05local\0'
    question = name + struct.pack('!HH', 1, 1)
    query = struct.pack('!HHHHHH', 0, 0, 1, 0, 0, 0) + question
    # Synthetic A response tests UDP behaviour only; it does not register an address or probe a name.
    answer = (struct.pack('!HHHHHH', 0, 0x8400, 0, 1, 0, 0) + name +
              struct.pack('!HHIH', 1, 0x8001, 120, 4) + socket.inet_aton(address))
    with ExitStack() as stack:
        sockets = []
        for _ in range(2):
            sock = stack.enter_context(socket.socket(family, socket.SOCK_DGRAM))
            sock.settimeout(2)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if family == socket.AF_INET:
                sock.setsockopt(socket.IPPROTO_IP, 8, 1)  # IP_PKTINFO
                sock.setsockopt(socket.IPPROTO_IP, 12, 1)  # IP_RECVTTL
                sock.bind(('', 5353))
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                                socket.inet_aton('224.0.0.251') + socket.inet_aton(address))
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
                destination = ('224.0.0.251', 5353)
            else:
                sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
                sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_RECVPKTINFO, 1)
                sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_RECVHOPLIMIT, 1)
                sock.bind(('::', 5353))
                sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP,
                                socket.inet_pton(socket.AF_INET6, 'ff02::fb') + struct.pack('I', index))
                sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, index)
                sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS, 255)
                destination = ('ff02::fb', 5353, 0, index)
            sockets.append(sock)
        sockets[0].sendto(query, destination)
        for sock in sockets:
            receive(sock, query, family, index)
        sockets[1].sendto(answer, destination)
        for sock in sockets:
            receive(sock, answer, family, index)
    print(f'PASS {"IPv4" if family == socket.AF_INET else "IPv6"} UDP 5353 shared query/answer, '
          f'interface {index}, hop limit 255')


def interfaces():
    # Subscribe and request an address snapshot on one non-blocking route-netlink descriptor.
    with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE) as sock:
        sock.bind((0, 1 | 0x10 | 0x100))
        sock.setblocking(False)
        seq = 1
        sock.send(struct.pack('IHHII', 24, 22, 0x301, seq, 0) + b'\0' * 8)  # RTM_GETADDR
        deadline, count = time.monotonic() + 2, 0
        while time.monotonic() < deadline:
            try:
                data = sock.recv(65536)
            except BlockingIOError:
                time.sleep(0.01)
                continue
            offset = 0
            while offset + 16 <= len(data):
                length, kind, _, sequence, _ = struct.unpack_from('IHHII', data, offset)
                if length < 16 or offset + length > len(data):
                    raise RuntimeError('Malformed/truncated netlink enumeration')
                if sequence == seq:
                    if kind == 2:
                        raise RuntimeError('Netlink error response')
                    if kind == 3:
                        if not count:
                            raise RuntimeError('No addresses enumerated')
                        print(f'PASS non-blocking netlink subscription and address snapshot ({count} addresses)')
                        return
                    if kind == 20:
                        count += 1
                offset += (length + 3) & ~3
        raise RuntimeError('Netlink enumeration timeout')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--interface', required=True)
    parser.add_argument('--ipv4', required=True)
    args = parser.parse_args()
    index = socket.if_nametoindex(args.interface)
    interfaces()
    multicast(socket.AF_INET, args.ipv4, index)
    multicast(socket.AF_INET6, args.ipv4, index)


if __name__ == '__main__':
    main()
