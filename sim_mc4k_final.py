#!/usr/bin/env python3
"""
FINAL CORRECT MC4K Manchester decoder for Hitag S on Flipper Zero.

TIM2 capture gives:
  CC3 (level=true):  fires on FALLING edge of COMP1 → HIGH pulse duration
  CC4 (level=false): fires on RISING edge of COMP1 → period (rising-to-rising), resets counter

COMP1: HIGH = UNLOAD (carrier strong), LOW = LOAD (carrier weak)

MC4K encoding (Hitag S, from PM3):
  bit 0: UNLOAD(h) → LOAD(h)   → COMP: HIGH(h) LOW(h) → 2nd half = LOW  → bit=0
  bit 1: LOAD(h)   → UNLOAD(h) → COMP: LOW(h)  HIGH(h) → 2nd half = HIGH → bit=1

Decoding approach: extract raw signal transitions from capture events,
build alternating pulse durations, expand to half-periods, pair up,
and read the bit from the second half's level.
"""

T0 = 8
HALF = 128  # µs
FULL = 256  # µs
THRESHOLD = 192  # midpoint between HALF and FULL
GLITCH = 50     # minimum valid pulse


def generate_comp1_waveform(bits):
    """Generate COMP1 signal from bit sequence."""
    pulses = []
    for bit in bits:
        if bit == 0:
            pulses.append((True, HALF))   # HIGH first half
            pulses.append((False, HALF))  # LOW second half
        else:
            pulses.append((False, HALF))  # LOW first half
            pulses.append((True, HALF))   # HIGH second half
    merged = []
    for is_high, dur in pulses:
        if merged and merged[-1][0] == is_high:
            merged[-1] = (is_high, merged[-1][1] + dur)
        else:
            merged.append((is_high, dur))
    return merged


def simulate_tim2(waveform, initial_high_dur=5000, trailing_high_dur=2000):
    """Simulate TIM2 capture events.
    
    Models: carrier HIGH → tag response → carrier HIGH (idle).
    The trailing_high models the carrier continuing after tag stops modulating.
    In real hardware this happens automatically (carrier is always on).
    """
    events = []
    # Full signal: carrier HIGH + response + idle HIGH
    full = [(True, initial_high_dur)] + list(waveform) + [(True, trailing_high_dur)]
    # Merge adjacent same-polarity (e.g., trailing HIGH merging with response ending HIGH)
    merged = []
    for is_high, dur in full:
        if merged and merged[-1][0] == is_high:
            merged[-1] = (is_high, merged[-1][1] + dur)
        else:
            merged.append((is_high, dur))
    
    t_abs = 0
    t_last_rising = 0
    prev_high = None
    for is_high, dur in merged:
        if prev_high is not None and is_high != prev_high:
            if is_high:
                events.append((False, t_abs - t_last_rising))
                t_last_rising = t_abs
            else:
                events.append((True, t_abs - t_last_rising))
        t_abs += dur
        prev_high = is_high
    return events


def decode_mc4k_v3(events, sof_bits=1, max_bits=40, threshold=THRESHOLD, verbose=False):
    """
    MC4K decoder v3: transition-based pulse extraction.
    
    Each capture event represents a COMP1 transition:
      CC3 (level=true, V): FALLING edge at (T_base + V). Signal was HIGH for V µs.
      CC4 (level=false, V): RISING edge at (T_base + V). Period V µs. T_base resets.
    
    We extract alternating HIGH/LOW pulses from transitions,
    then expand to half-periods and decode.
    """
    # ---- Step 1: Extract raw pulse sequence from events ----
    # Each event marks a transition. Build the signal pulse train.
    # The signal alternates: ... HIGH ... FALLING ... LOW ... RISING ... HIGH ...
    #
    # From the capture, we get the pulse durations directly:
    # CC3(true, V) → HIGH pulse lasted V µs (preceding the falling edge)  
    # Between CC3 and next CC4: LOW pulse of duration (CC4_period - CC3_HIGH)
    #
    # But we need to handle the initial carrier and align properly.
    
    raw_pulses = []  # [(is_high, duration)]
    last_high_dur = 0
    started = False
    
    for level, dur in events:
        if level:  # CC3: HIGH pulse
            if dur < GLITCH:
                continue
            last_high_dur = dur
        else:  # CC4: period
            if last_high_dur == 0:
                continue
            if dur <= last_high_dur:
                last_high_dur = 0
                continue
            
            high_dur = last_high_dur
            low_dur = dur - high_dur
            
            if not started:
                # First pair: includes initial carrier HIGH
                # The HIGH is the carrier duration → skip it
                # The LOW is the FIRST half of the SOF bit → keep it!
                started = True
                if low_dur >= GLITCH:
                    raw_pulses.append((False, low_dur))  # First LOW (SOF start)
                if verbose:
                    print(f"  Initial: HIGH={high_dur}µs (carrier, skip), LOW={low_dur}µs (keep)")
            else:
                if high_dur >= GLITCH:
                    raw_pulses.append((True, high_dur))
                if low_dur >= GLITCH:
                    raw_pulses.append((False, low_dur))
            
            last_high_dur = 0
    
    # No trailing handler needed: carrier always returns to HIGH after response,
    # so the last event pair always includes the final LOW pulse.
    # The carrier-idle HIGH is just the "next HIGH period" which gets filtered
    # by the caller (known response length).
    
    if verbose:
        print(f"  Raw pulses ({len(raw_pulses)}):")
        for i, (h, d) in enumerate(raw_pulses[:30]):
            sl = 'S' if d < threshold else 'L'
            n = 1 if d < threshold else 2
            print(f"    [{i:2d}] {'H' if h else 'L'}{sl} {d}µs ({n}hp)")
    
    # ---- Step 2: Expand to half-period level stream ----
    half_periods = []
    for is_high, dur in raw_pulses:
        if dur < GLITCH:
            continue
        n = 1 if dur < threshold else 2
        for _ in range(n):
            half_periods.append(is_high)
    
    # If last half-period is incomplete and we need more bits,
    # the response likely ends with HIGH (carrier idle)
    if len(half_periods) % 2 == 1:
        # Add one more HIGH to complete the last bit
        half_periods.append(True)
    
    if verbose:
        hp_str = ''.join('H' if h else 'L' for h in half_periods[:40])
        print(f"  Half-periods ({len(half_periods)}): {hp_str}")
    
    # ---- Step 3: Pair into bits ----
    # bit value = 1 if second half is HIGH, 0 if LOW
    all_bits = []
    for i in range(0, len(half_periods) - 1, 2):
        second_half = half_periods[i + 1]
        all_bits.append(1 if second_half else 0)
    
    if verbose:
        for i in range(min(20, len(all_bits))):
            f = 'H' if half_periods[2*i] else 'L'
            s = 'H' if half_periods[2*i+1] else 'L'
            print(f"    Bit[{i}]: ({f},{s}) → {all_bits[i]}")
    
    # ---- Step 4: Verify SOF and strip ----
    for i in range(min(sof_bits, len(all_bits))):
        if all_bits[i] != 1 and verbose:
            print(f"  WARNING: SOF bit {i} = {all_bits[i]}, expected 1")
    
    data_bits = all_bits[sof_bits:sof_bits + max_bits]
    return data_bits, all_bits


def test(name, bits, sof_bits=1, verbose=True):
    all_bits = [1] * sof_bits + bits
    print(f"\n{'='*70}")
    print(f"TEST: {name}")
    print(f"SOF={sof_bits}, Data ({len(bits)}): {''.join(str(b) for b in bits)}")
    if len(bits) == 32:
        val = sum(b << (31-i) for i, b in enumerate(bits))
        print(f"Expected: 0x{val:08X}")
    
    waveform = generate_comp1_waveform(all_bits)
    events = simulate_tim2(waveform)
    data, all_dec = decode_mc4k_v3(events, sof_bits=sof_bits, max_bits=len(bits), verbose=verbose)
    
    match = data == bits
    print(f"\n  All decoded ({len(all_dec)}): {''.join(str(b) for b in all_dec[:50])}")
    print(f"  Data ({len(data)}): {''.join(str(b) for b in data)}")
    print(f"  Expected ({len(bits)}): {''.join(str(b) for b in bits)}")
    if len(bits) == 32 and len(data) >= 32:
        dval = sum(b << (31-i) for i, b in enumerate(data[:32]))
        print(f"  Decoded: 0x{dval:08X}")
    print(f"  MATCH: {'✓' if match else '✗'}")
    return match


def main():
    print("=" * 70)
    print("MC4K Decoder V3: Transition-based / Half-period Pairing")
    print("=" * 70)
    
    results = {}
    
    results['all 1s (8)'] = test("All 1s (8)", [1]*8)
    results['all 0s (8)'] = test("All 0s (8)", [0]*8)
    results['alt 10 (8)'] = test("Alt 1010 (8)", [1,0,1,0,1,0,1,0])
    results['alt 01 (8)'] = test("Alt 0101 (8)", [0,1,0,1,0,1,0,1])
    results['11001010'] = test("11001010", [1,1,0,0,1,0,1,0])
    results['10110010'] = test("10110010", [1,0,1,1,0,0,1,0])
    results['ACK 01'] = test("ACK (01)", [0, 1])
    
    # 32-bit values
    for val in [0x060000E8, 0x5FC21184, 0xDEADBEEF, 0x00000000, 0xFFFFFFFF, 0xAAAAAAAA]:
        bits = [(val >> (31-i)) & 1 for i in range(32)]
        results[f'0x{val:08X}'] = test(f"32-bit 0x{val:08X}", bits, verbose=False)
    
    # ADV mode (6 SOF bits)
    for val in [0xAABBCCDD, 0x060000E8]:
        bits = [(val >> (31-i)) & 1 for i in range(32)]
        results[f'ADV 0x{val:08X}'] = test(f"ADV 0x{val:08X}", bits, sof_bits=6, verbose=False)
    
    # Summary
    print(f"\n{'='*70}")
    print("SUMMARY")
    print(f"{'='*70}")
    total = len(results)
    passed = sum(1 for v in results.values() if v)
    for name, ok in results.items():
        print(f"  {'✓' if ok else '✗'} {name}")
    print(f"\n  {passed}/{total} tests passed")
    
    if passed < total:
        print("\n  FAILURES need investigation!")
        return 1
    else:
        print("\n  ALL TESTS PASSED!")
        return 0


if __name__ == "__main__":
    exit(main())
