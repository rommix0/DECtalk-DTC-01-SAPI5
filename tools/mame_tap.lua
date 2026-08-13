-- Differential-trace harness: capture the DTC-01's internal buses from MAME,
-- to compare against our emulator's. MAME is the oracle -- it plays both
-- firmware versions correctly (DESIGN.md section 21).
--
--   mame dectalk [-bios v18] -rompath <dir with dectalk.zip> \
--        -video none -sound none -seconds_to_run 12 -skip_gameinfo \
--        -autoboot_script tools/mame_tap.lua -autoboot_delay 0 \
--        -nvram_directory <fresh dir>       # MUST be empty: a saved nvram
--                                           # changes the boot path
--
-- Environment: DTCTAP_OUT = output file, DTCTAP_WHAT = in|out|dspread|time
--   in      the 68000's parameter words to the DSP (m68k_infifo_w, 0x09c002).
--           Ours matches MAME's word for word -- this is how the whole 68000
--           side got exonerated.
--   out     the DSP's synthesised samples (spc_outfifo_data_w, io port 1).
--           This is where we diverge.
--   dspread the words the DSP pulls back out of the infifo
--           (spc_infifo_data_r, io port 1). Different from `in`: `in` is what
--           the 68000 puts in, this is what the DSP actually gets. If these
--           two sequences differ, the fifo is delivering duplicated or skipped
--           words and the DSP is computing garbage from correct input.
--   time    write rate and audio onset. MAME writes exactly 10000/s while
--           speaking -- one per DAC tick. Ours: v2.0 9993/s, v1.8 8898/s.
--
-- NB: opcode fetches bypass memory taps (they go through MAME's cache), so
-- a read tap on the DSP program space does NOT count instructions. Do not
-- try to measure the DSP's instruction rate that way -- it undercounts.

local what = os.getenv("DTCTAP_WHAT") or "out"
local path = os.getenv("DTCTAP_OUT") or "mametap.txt"
local out = io.open(path, "w")

local mach = manager.machine
local n, first = 0, false

local function tap_writes(space, lo, hi, label)
    space:install_write_tap(lo, hi, label, function(offs, data, mask)
        n = n + 1
        if what == "time" then
            if (not first) and data ~= 0 then
                first = true
                out:write(string.format("first nonzero write: index=%d t=%.4fs\n",
                                        n, mach.time:as_double()))
                out:flush()
            end
            if n % 10000 == 0 then
                out:write(string.format("n=%d t=%.4fs\n", n, mach.time:as_double()))
                out:flush()
            end
        else
            out:write(string.format("%04X\n", data))
            if n % 5000 == 0 then out:flush() end
        end
        return data
    end)
end

if what == "dsp0" then
    -- The DSP's writes to io port 0: sets the infifo semaphore, and bit 0 is
    -- the "soft error" latch the 68000 sees as spcflags bit 5. MAME's v1.8 DSP
    -- never raises it; ours does, which is what puts the firmware on its error
    -- path (DESIGN.md section 21).
    mach.devices[":dsp"].spaces["io"]:install_write_tap(0, 0, "dsp_port0",
        function(offs, data, mask)
            n = n + 1
            out:write(string.format("%04X\n", data))
            if n % 1000 == 0 then out:flush() end
            return data
        end)
elseif what == "spcflags" then
    -- The 68000's view of the speech handshake: reads of the SPC flags
    -- register (bit 7 = infifo semaphore, bit 5 = dsp error) and the commands
    -- it writes back. The firmware polls bit 7 in a tight loop, so read counts
    -- differ harmlessly between emulators -- the WRITE sequence is the
    -- firmware's actual decisions and is what should be compared.
    -- NB: do NOT read cpu.state[...] from inside a tap; MAME segfaults.
    local sp = mach.devices[":maincpu"].spaces["program"]
    sp:install_read_tap(0x09c000, 0x09c001, "spcflags_r", function(offs, data, mask)
        n = n + 1
        out:write(string.format("R %04X\n", data))
        if n % 2000 == 0 then out:flush() end
        return data
    end)
    sp:install_write_tap(0x09c000, 0x09c001, "spcflags_w", function(offs, data, mask)
        n = n + 1
        out:write(string.format("W %04X\n", data))
        out:flush()
        return data
    end)
elseif what == "inpc" then
    -- Same as `in`, but records which 68000 instruction issued each word.
    -- The write stream for v1.8 diverges from ours at word 62; this says
    -- which code path each emulator was on when it happened.
    local cpu = mach.devices[":maincpu"]
    cpu.spaces["program"]:install_write_tap(0x09c002, 0x09c003, "infifo_pc",
        function(offs, data, mask)
            n = n + 1
            out:write(string.format("%04X %06X\n", data, cpu.state["CURPC"].value))
            if n % 2000 == 0 then out:flush() end
            return data
        end)
elseif what == "in" then
    tap_writes(mach.devices[":maincpu"].spaces["program"], 0x09c002, 0x09c003, "infifo")
elseif what == "dspread" then
    -- Read tap, not a write tap: this is the DSP's IN instruction pulling a
    -- word out of the infifo. I/O reads run through spc_infifo_data_r, so the
    -- tap fires -- unlike opcode fetches, which bypass taps via MAME's cache.
    mach.devices[":dsp"].spaces["io"]:install_read_tap(1, 1, "infifo_rd",
        function(offs, data, mask)
            n = n + 1
            out:write(string.format("%04X\n", data))
            if n % 5000 == 0 then out:flush() end
            return data
        end)
else
    tap_writes(mach.devices[":dsp"].spaces["io"], 1, 1, "outfifo")
end

out:write(string.format("-- tap %s installed\n", what))
out:flush()
