#!/usr/bin/env bash
#
# copybas.sh - paste a TI BASIC listing into pico-994A over the serial console.
#
# The emulator's "Serial keyboard" setting (settings menu, off by default) turns
# characters arriving on the UART into TI keypresses, so a listing written on a PC types
# itself into TI BASIC or Extended BASIC. This script sends one over.
#
# It does three things in order:
#
#   1. Clears whatever the editor was in the middle of. A few harmless PRINT statements
#      terminate any half-finished line left over from a previous session, and NEW then
#      wipes the program so the listing lands in an empty machine.
#   2. Sends the listing.
#   3. Commits the final line and pushes the tail through, in case the file does not end
#      with a newline.
#
# The TI accepts about twelve characters a second against the serial port's eleven
# thousand, so this takes minutes rather than seconds. That is the machine's own keyboard
# scan rate, not a fault. Leave it alone until the TI screen stops changing.
#
# Note on the stty line: "raw" implies -ixon -ixoff, so "ixon" has to come after it -
# reversing the two silently disables XON/XOFF and the emulator loses its only way to hold
# the sender off. "raw" does not touch ECHO, which is why -echo is spelled out: left on,
# the host echoes whatever the board prints back into the board, and the emulator types it
# into TI BASIC as though someone had keyed it in.
#
# Usage: copybas.sh [-w] <listing.bas> [serial-device]
#        copybas.sh examples/pastetest.bas /dev/ttyACM0
#        copybas.sh -w examples/pastetest.bas
#
# -w waits afterwards and shows what the board says, including the summary it prints when
#    it has finished typing:
#
#        Serial keyboard: 2115 characters received, 2115 typed
#
#    That total covers everything sent, so it is the listing plus the framing statements
#    around it, not just the size of the file. A count short of the expected total means
#    those characters never reached the board at all, which is a fault in the link or the
#    sender rather than in the typing.
#    Without -w the script just sends and exits, and you need a terminal open on the port
#    to see any of that.
#
# The device is /dev/ttyACM0 by default. A Raspberry Pi Debug Probe and other CDC-ACM
# adapters appear there; FTDI, CP210x and CH340 adapters appear as /dev/ttyUSB0.

set -u

BAUD=115200
FLUSH_CMD='PRINT 0'     # harmless statement used to flush a half-finished editor line
FLUSH_LINES=5           # how many of them are sent before and after the listing
CHARS_PER_SECOND=12     # what the TI's keyboard scan actually sustains
SECONDS_PER_LINE=0.5    # the pause after each ENTER while BASIC tokenises and scrolls

prog=$(basename "$0")
say() { printf '%s: %s\n' "$prog" "$*"; }
die() { printf '%s: %s\n' "$prog" "$*" >&2; exit 1; }

wait_for_summary=0
if [ "${1:-}" = "-w" ] || [ "${1:-}" = "--wait" ]; then
    wait_for_summary=1
    shift
fi

file="${1:-}"
serial="${2:-/dev/ttyACM0}"

if [ -z "$file" ]; then
    printf 'Usage: %s [-w] <listing.bas> [serial-device]\n' "$prog" >&2
    printf '       %s examples/pastetest.bas /dev/ttyACM0\n' "$prog" >&2
    printf '       -w  wait and show the summary the board prints when it finishes\n' >&2
    exit 1
fi

[ -f "$file" ]   || die "no such file: $file"
[ -r "$file" ]   || die "cannot read: $file"
[ -c "$serial" ] || die "not a serial device: $serial"
[ -w "$serial" ] || die "cannot write to $serial (are you in the dialout group?)"

bytes=$(wc -c < "$file" | tr -d ' ')
lines=$(wc -l < "$file" | tr -d ' ')
[ "$bytes" -gt 0 ] || die "$file is empty"

# Rough, but close enough to tell you whether to wait or go and make tea.
est_secs=$(awk -v b="$bytes" -v l="$lines" -v c="$CHARS_PER_SECOND" -v s="$SECONDS_PER_LINE" \
           'BEGIN { printf "%d", b / c + l * s }')
est=$(awk -v t="$est_secs" 'BEGIN { printf "%dm%02ds", t / 60, t % 60 }')

# What the board should end up counting: the listing plus the framing this script wraps
# around it. Comparing its report against the file size alone would always look short.
flush_bytes=$(( FLUSH_LINES * (${#FLUSH_CMD} + 1) ))
expected=$(( bytes + flush_bytes + 4 + 1 + flush_bytes ))

say "device   $serial at $BAUD 8N1, XON/XOFF flow control"
say "listing  $file - $bytes bytes, $lines lines"
say "sending  $expected bytes in all, counting the framing statements"
say "expect   about $est of typing"

# Anything past the emulator's XOFF threshold makes it hold this script off mid-file,
# which is worth knowing about because it is the awkward case.
if [ "$bytes" -gt 12288 ]; then
    say "note     larger than the emulator's 12 KB buffer, so flow control will engage"
fi

# -echo matters as much as ixon here: "raw" does not clear ECHO, and with it left on the
# host echoes the board's own output back at it, which the emulator then dutifully types
# into TI BASIC. "raw" also implies -ixon, so ixon has to follow it.
stty -F "$serial" "$BAUD" raw -echo ixon -crtscts || die "stty failed on $serial"

say "clearing the editor ($FLUSH_LINES x $FLUSH_CMD, then NEW)"
for _ in $(seq "$FLUSH_LINES"); do
    echo "$FLUSH_CMD" > "$serial" || die "write failed on $serial"
done
echo 'NEW' > "$serial"
sleep 1

say "sending the listing"
cat "$file" > "$serial" || die "send failed on $serial"

# A file with no trailing newline would otherwise leave its last line uncommitted, and
# the first flush statement would be typed onto the end of it.
say "committing the last line"
printf '\n' > "$serial"
for _ in $(seq "$FLUSH_LINES"); do
    echo "$FLUSH_CMD" > "$serial"
done

if [ "$wait_for_summary" -eq 0 ]; then
    say "sent - the TI is still typing it in, for about $est yet"
    say "       it reports its totals on this port when it finishes; re-run with -w to see"
    say "       them, or watch a terminal open on $serial"
    say "       it should report $expected characters received"
    exit 0
fi

# Typing takes as long as it takes, so allow the estimate plus a generous margin.
deadline=$(( est_secs + 120 ))
say "sent - waiting up to $(( deadline / 60 ))m for the board to finish typing (Ctrl-C to stop)"

exec 3< "$serial" || die "cannot read back from $serial"
end=$(( SECONDS + deadline ))
status=1
while [ "$SECONDS" -lt "$end" ]; do
    IFS= read -r -t 5 line <&3 || continue
    line=${line%$'\r'}
    [ -n "$line" ] || continue
    say "board | $line"
    case "$line" in
        *"characters received"*)
            got=$(printf '%s' "$line" | sed -n 's/.*keyboard: \([0-9]*\) characters.*/\1/p')
            if [ "$got" = "$expected" ]; then
                say "all $expected characters reached the board"
                status=0
            else
                say "expected $expected characters, the board received $got -"
                say "       the difference never arrived, so look at the link or the sender"
                status=2
            fi
            break
            ;;
    esac
done
exec 3<&-

# Only status 1 means nothing came back at all; 2 is a summary that did not add up, and
# has already been reported in full.
if [ "$status" -eq 1 ]; then
    say "no summary within $(( deadline / 60 ))m - it may still be typing, or the board may"
    say "       have the Serial keyboard setting switched off"
fi
exit "$status"
