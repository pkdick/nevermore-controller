#!/bin/bash
"true" '''\'
set -eu
set -o pipefail

FILE="$(readlink -f "$0")"
ROOT_DIR="$(dirname "$FILE")"

"$ROOT_DIR/setup-tool-env.bash"
"$ROOT_DIR/.venv/bin/python" "$FILE" "$@"
exit 0 # required to stop shell execution here
'''

# Script for configuring the Wi-Fi of a Nevermore controller.
#
# This file may be distributed under the terms of the GNU AGPLv3 license.

__doc__ = """Script for configuring the Wi-Fi (GATT over TCP) of Nevermore controllers.

Typically run once over USB (`--serial .../usb-Nevermore_..._-if02`).
Afterwards the controller can be reached w/ `--network <host>` and Klipper's
`network_address` option."""

import asyncio
import enum
import getpass
import logging
import struct
import time
from typing import Optional

import typed_argparse as tap
from nevermore_tool_utilities import NevermoreToolCmdLnArgs
from nevermore_utilities import *

__all__ = ['CmdLnArgs', 'main']

SSID_MAX = 32
PASSWORD_MIN = 8
PASSWORD_MAX = 63
STATUS_POLL_PERIOD = 1  # seconds


# mirrors `nevermore::wifi::State`
class WifiState(enum.IntEnum):
    UNSUPPORTED = 0
    DISABLED = 1
    CONNECTING = 2
    CONNECTED = 3
    FAILED_AUTH = 4
    FAILED_NO_NETWORK = 5
    FAILED = 6


WIFI_STATE_DESCRIPTION = {
    WifiState.UNSUPPORTED: "not supported by this firmware build",
    WifiState.DISABLED: "disabled (no credentials)",
    WifiState.CONNECTING: "connecting...",
    WifiState.CONNECTED: "connected",
    WifiState.FAILED_AUTH: "failed: wrong password",
    WifiState.FAILED_NO_NETWORK: "failed: network not found",
    WifiState.FAILED: "failed",
}


@dataclass(frozen=True)
class WifiStatus:
    state: WifiState
    ipv4: str
    port: int

    FORMAT = struct.Struct("<B4sH")

    @staticmethod
    def parse(raw: bytes) -> 'WifiStatus':
        state, ipv4, port = WifiStatus.FORMAT.unpack(raw[: WifiStatus.FORMAT.size])
        try:
            state = WifiState(state)
        except ValueError:
            state = WifiState.FAILED
        return WifiStatus(state, ".".join(str(x) for x in ipv4), port)

    def __str__(self) -> str:
        desc = WIFI_STATE_DESCRIPTION[self.state]
        if self.state == WifiState.CONNECTED:
            return f"{desc}; address {self.ipv4}, port {self.port}"
        return desc


class CmdLnArgs(NevermoreToolCmdLnArgs):
    ssid: Optional[str] = tap.arg(help="Wi-Fi network name")
    password: Optional[str] = tap.arg(
        help="Wi-Fi password (prompted for if omitted; empty for an open network)"
    )
    forget: bool = tap.arg(help="forget credentials & disable Wi-Fi", default=False)
    status: bool = tap.arg(help="only show the Wi-Fi status", default=False)
    wait: float = tap.arg(
        help="seconds to wait for the controller to connect", default=45
    )


def credentials_encode(ssid: str, password: str) -> bytes:
    raw_ssid = ssid.encode("utf-8")
    raw_password = password.encode("utf-8")
    if not (len(raw_ssid) <= SSID_MAX):
        raise ValueError(f"SSID must be at most {SSID_MAX} bytes")
    if raw_password and not (PASSWORD_MIN <= len(raw_password) <= PASSWORD_MAX):
        raise ValueError(
            f"password must be {PASSWORD_MIN} to {PASSWORD_MAX} characters (or empty)"
        )
    if b"\0" in raw_ssid or b"\0" in raw_password:
        raise ValueError("SSID & password cannot contain NUL")

    return (
        bytes([len(raw_ssid)]) + raw_ssid + bytes([len(raw_password)]) + raw_password
    )


async def status_read(transport: Transport) -> WifiStatus:
    return WifiStatus.parse(await transport(UUID_CHAR_CONFIG_WIFI_STATUS).read())


async def status_wait(transport: Transport, timeout: float) -> WifiStatus:
    deadline = time.monotonic() + timeout
    last: Optional[WifiStatus] = None
    while True:
        status = await status_read(transport)
        if status != last:
            print(f"Wi-Fi: {status}")
            last = status

        if status.state in {
            WifiState.CONNECTED,
            WifiState.DISABLED,
            WifiState.UNSUPPORTED,
        }:
            return status
        # connecting or failed: the controller retries failures, keep waiting
        if deadline <= time.monotonic():
            return status

        await asyncio.sleep(STATUS_POLL_PERIOD)


async def configure(args: CmdLnArgs, transport: Transport, blob: bytes) -> bool:
    P = CharacteristicProperty
    try:
        chars = transport(UUID_CHAR_CONFIG_WIFI_CREDENTIALS, {P.WRITE})
    except Transport.AttrNotFound:
        print("This firmware doesn't support Wi-Fi.")
        return False

    try:
        await chars.write(blob)
    except Exception:
        status = await status_read(transport)
        if status.state == WifiState.UNSUPPORTED:
            print("This firmware build doesn't support Wi-Fi (Pico 2 W builds do).")
        else:
            logging.exception("failed to write credentials")
        return False

    if args.forget:
        print("Wi-Fi credentials forgotten.")
        return True

    print("Credentials sent. They are saved to flash within ~10 seconds.")
    status = await status_wait(transport, args.wait)
    if status.state != WifiState.CONNECTED:
        print("Not connected yet. The controller keeps retrying; check with `--status`.")
        return False

    print()
    print("Klipper config (`[nevermore]` section), replacing `serial`/`bt_address`:")
    print(f"  network_address: {status.ipv4}")
    print("(or `nevermore.local` if the host resolves mDNS names.)")
    return True


async def main(args: CmdLnArgs) -> int:
    if not args.validate():
        return 1

    blob: Optional[bytes] = None
    if args.forget:
        blob = credentials_encode("", "")
    elif not args.status:
        if args.ssid is None:
            logging.error("`--ssid` is required (or use `--status` / `--forget`)")
            return 1

        password = args.password
        if password is None:
            password = getpass.getpass(f"Password for `{args.ssid}` (empty if open): ")

        try:
            blob = credentials_encode(args.ssid, password)
        except ValueError as e:
            logging.error(e)
            return 1

    if (transport := await args.connect()) is None:
        print("unable to connect to the controller")
        return 1

    async with transport:
        try:
            if blob is None:
                print(f"Wi-Fi: {await status_read(transport)}")
                return 0

            return 0 if await configure(args, transport, blob) else 1
        except Transport.AttrNotFound:
            print("This firmware doesn't support Wi-Fi.")
            return 1


if __name__ == "__main__":

    def go(args: CmdLnArgs):
        exit(asyncio.run(main(args)))

    tap.Parser(CmdLnArgs).bind(go).run()
