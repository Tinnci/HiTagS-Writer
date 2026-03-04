#!/usr/bin/env python3
"""
Simulate the Flipper Zero Manchester decoder and HAL capture pipeline
to verify our hitag_s_decode_mc4k() implementation.

Goal: Generate synthetic Hitag S MC4K waveforms for known data,
simulate the TIM2 capture ISR output, and decode using the exact
same state machine as the Flipper SDK.

Also compare with what lfrfid_worker does (split into two half-period feeds).
"""

# === Manchester State Machine (exact copy from manchester_decoder.c) ===

# States
ManchesterStateStart1 = 0
ManchesterStateMid1   = 1
ManchesterStateMid0   = 2
ManchesterStateStart0 = 3

# Events
ManchesterEventShortLow  = 0
ManchesterEventShortHigh = 2
ManchesterEventLongLow   = 4
ManchesterEventLongHigh  = 6
ManchesterEventReset     = 8

transitions = [0b00000001, 0b10010001, 0b10011011, 0b11111011]
manchester_reset_state = ManchesterStateMid1

state_names = {0: "Start1", 1: "Mid1", 2: "Mid0", 3: "Start0"}
event_names = {0: "ShortLow", 2: "ShortHigh", 4: "LongLow", 6: "LongHigh", 8: "Reset"}

def manchester_advance(state, event):
    """Returns (next_state, data_bit_or_None)"""
    if event == ManchesterEventReset:
        return manchester_reset_state, None
    
    new_state = (transitions[state] >> event) & 0x3
    if new_state == state:
        # Invalid transition → reset
        return manchester_reset_state, None
    
    data = None
    if new_state == ManchesterStateMid0:
        data = False
    elif new_state == ManchesterStateMid1:
        data = True
    
    return new_state, data


# === Hitag S MC4K Waveform Generator ===
# MC4K: bit period = 256µs (32 × T0, T0=8µs), half-bit = 128µs

T0 = 8  # µs
HALF_BIT = 16 * T0  # 128 µs
FULL_BIT = 32 * T0  # 256 µs

def generate_mc4k_waveform(bits, sof_bits=1):
    """
    Generate Hitag S MC4K tag response waveform as alternating
    HIGH(UNLOAD) and LOW(LOAD) pulse durations.
    
    From PM3 hitag_common.c hitag_tag_send_bit():
      bit '0': UNLOAD(half) then LOAD(half) → HIGH(128µs) LOW(128µs) 
      bit '1': LOAD(half) then UNLOAD(half) → LOW(128µs) HIGH(128µs)
    
    Returns list of (is_high, duration_us) tuples representing
    the modulation envelope.
    """
    # SOF is all 1s
    all_bits = [1] * sof_bits + bits
    
    # Build waveform as sequence of (LOAD/UNLOAD, duration) pulses
    pulses = []  # (is_unload, duration_us)
    
    for bit in all_bits:
        if bit == 0:
            # UNLOAD then LOAD
            pulses.append((True, HALF_BIT))   # HIGH (UNLOAD)
            pulses.append((False, HALF_BIT))  # LOW (LOAD)
        else:
            # LOAD then UNLOAD
            pulses.append((False, HALF_BIT))  # LOW (LOAD)
            pulses.append((True, HALF_BIT))   # HIGH (UNLOAD)
    
    # Merge adjacent pulses of same polarity
    merged = []
    for is_unload, dur in pulses:
        if merged and merged[-1][0] == is_unload:
            merged[-1] = (is_unload, merged[-1][1] + dur)
        else:
            merged.append((is_unload, dur))
    
    return merged


def simulate_tim2_capture(waveform):
    """
    Simulate TIM2 capture ISR from merged waveform.
    
    TIM2 setup:
    - COMP1: antenna > 0.6V → HIGH output (= UNLOAD = carrier strong)
    - CH3 (indirect, falling edge): captures HIGH pulse width → callback(true, high_dur)
    - CH4 (direct, rising edge + counter reset): captures period → callback(false, period)
    
    The ISR fires:
    1. On falling edge of COMP1 (HIGH→LOW = UNLOAD→LOAD): 
       CC3 captures counter value = duration of HIGH pulse → callback(true, high_dur)
    2. On rising edge of COMP1 (LOW→HIGH = LOAD→UNLOAD):
       CC4 captures counter value = period (since last reset) → callback(false, period)
       Counter reset to 0
    
    So for each COMP1 cycle:
    - Rising edge: CC4 fires → callback(false, full_period), counter=0
    - Falling edge: CC3 fires → callback(true, high_dur_since_rising)
    
    The callback sequence is: (false, period), (true, high_dur), (false, period), ...
    But actually the FIRST event depends on what COMP1 starts at.
    
    Let's trace through a real waveform starting from carrier on (COMP1 HIGH = UNLOAD):
    """
    events = []  # [(level_bool, duration_us)]
    
    # Track COMP1 state and counter
    counter = 0
    
    for i, (is_unload, dur) in enumerate(waveform):
        is_high = is_unload  # COMP1: UNLOAD → HIGH
        
        if is_high:
            # HIGH pulse → will end with falling edge
            # Falling edge fires CC3: callback(true, counter + dur)
            # But wait - CC3 captures counter value at falling edge
            # Counter has been running since last CC4 reset
            counter += dur
            events.append((True, counter))  # CC3 event
        else:
            # LOW pulse → will end with rising edge
            # Rising edge fires CC4: callback(false, counter + dur), then counter=0
            counter += dur
            events.append((False, counter))  # CC4 event
            counter = 0  # CC4 resets counter
    
    return events


def simulate_tim2_capture_v2(waveform):
    """
    Alternative: simulate exactly what happens pulse by pulse.
    
    COMP1 output = HIGH when antenna voltage > 0.6V (UNLOAD/carrier strong)
    
    CC4 captures on RISING edge of TI4 (= COMP1 rising = LOAD→UNLOAD transition)
    CC3 captures on FALLING edge of TI4 (= COMP1 falling = UNLOAD→LOAD transition)
    
    Counter resets on CC4 (rising edge).
    
    So:
    - At UNLOAD→LOAD transition: CC3 fires, value = time since last CC4 reset = HIGH pulse width
    - At LOAD→UNLOAD transition: CC4 fires, value = time since last CC4 reset = full period, then reset
    """
    events = []
    counter = 0
    
    for i, (is_unload, dur) in enumerate(waveform):
        counter += dur
        if is_unload:
            pass  # HIGH pulse, no edge at start, edge at END (falling)
        else:
            pass  # LOW pulse, no edge at start, edge at END (rising)
    
    # Actually, edges occur at TRANSITIONS between pulses
    # Let's think about it differently:
    # Walk through waveform, detect transitions
    
    events = []
    counter = 0
    prev_high = None
    
    for i, (is_unload, dur) in enumerate(waveform):
        is_high = is_unload
        
        if prev_high is not None and is_high != prev_high:
            # Transition detected
            if is_high:
                # LOW→HIGH = rising edge → CC4 fires
                events.append((False, counter))
                counter = 0
            else:
                # HIGH→LOW = falling edge → CC3 fires
                events.append((True, counter))
        
        counter += dur
        prev_high = is_high
    
    return events


def our_decode_mc4k(capture_events, threshold, sof_bits, max_bits=40):
    """
    Simulate our hitag_s_decode_mc4k() function exactly.
    """
    # Pre-process: compute HIGH and LOW pulse durations
    durations = []
    last_high_dur = 0
    for level, dur in capture_events:
        if level:
            durations.append(dur)
            last_high_dur = dur
        else:
            if last_high_dur > 0 and dur > last_high_dur:
                durations.append(dur - last_high_dur)
            else:
                durations.append(dur)
    
    glitch_min = 80 if threshold > 200 else 50
    
    state = ManchesterStateMid1  # Reset state
    sof_remaining = sof_bits
    data_bits = []
    total_bits = 0
    
    for i, (dur, (level, _)) in enumerate(zip(durations, capture_events)):
        if dur < glitch_min:
            continue
        
        is_short = dur < threshold
        
        # Our current mapping: level=true → High, level=false → Low
        if is_short:
            event = ManchesterEventShortHigh if level else ManchesterEventShortLow
        else:
            event = ManchesterEventLongHigh if level else ManchesterEventLongLow
        
        next_state, data = manchester_advance(state, event)
        
        if data is not None:
            total_bits += 1
            if sof_remaining > 0:
                sof_remaining -= 1
            else:
                data_bits.append(data)
                if len(data_bits) >= max_bits:
                    break
        
        state = next_state
    
    return data_bits, total_bits


def lfrfid_decode(capture_events, threshold, sof_bits, max_bits=40):
    """
    Simulate what Unleashed's lfrfid_worker does:
    It unpacks (pulse, duration) pairs and calls:
      feed(true, pulse)         — HIGH time
      feed(false, duration-pulse) — LOW time
    
    With EM4100 mapping: level=true→ShortLow, level=false→ShortHigh
    """
    # Group capture events into (HIGH, period) pairs
    pairs = []
    i = 0
    while i < len(capture_events):
        level, dur = capture_events[i]
        if level:  # HIGH event
            pulse = dur
            if i + 1 < len(capture_events) and not capture_events[i+1][0]:
                period = capture_events[i+1][1]
                # lfrfid computes: pulse=HIGH dur, duration=period (rising-to-rising)
                # LOW time = period - pulse... but wait, period from CC4 is 
                # time since last reset = HIGH + LOW
                # Actually varint_pair packs (level=true, value=pulse) first,
                # then (level=false, value=period)
                # On unpack: value_1=pulse, value_2=period
                # Feed: feed(true, pulse), feed(false, duration-pulse)
                pairs.append((pulse, period))
                i += 2
            else:
                i += 1
        else:
            i += 1
    
    state = ManchesterStateMid1
    sof_remaining = sof_bits
    data_bits = []
    total_bits = 0
    
    for pulse, period in pairs:
        low_dur = period - pulse if period > pulse else period
        
        # Feed HIGH pulse first
        for level, dur in [(True, pulse), (False, low_dur)]:
            if dur < 16:  # noise filter
                continue
            
            is_short = dur < (threshold // 2)  # half-period threshold
            
            # EM4100 mapping: !level → High, level → Low
            if is_short:
                event = ManchesterEventShortLow if level else ManchesterEventShortHigh
            else:
                event = ManchesterEventLongLow if level else ManchesterEventLongHigh
            
            next_state, data = manchester_advance(state, event)
            
            if data is not None:
                total_bits += 1
                if sof_remaining > 0:
                    sof_remaining -= 1
                else:
                    data_bits.append(data)
            
            state = next_state
    
    return data_bits, total_bits


# === Test Cases ===

def bits_to_hex(bits):
    """Convert list of bool bits to hex string"""
    result = 0
    for b in bits:
        result = (result << 1) | (1 if b else 0)
    # Pad to multiple of 4
    hex_str = hex(result)
    return hex_str

def test_known_data():
    """
    Test with known EM4100 data that the official Flipper reads correctly.
    
    Card info from user:
    - hex: 000000204C (5 bytes EM4100 ID)
    - FC: 000, Card: 08268, CL: 64
    - DEZ 8: 00008268
    
    The 64-bit EM4100 encoded frame for ID 00:00:00:20:4C:
    Header 9×1, then 10 rows of (4 data + 1 parity), 4 col parity, 1 stop
    """
    from struct import pack
    
    # EM4100 ID bytes
    id_bytes = bytes([0x00, 0x00, 0x00, 0x20, 0x4C])
    
    # Encode EM4100
    data_40 = 0
    for b in id_bytes:
        data_40 = (data_40 << 8) | b
    
    # Build 64-bit frame
    result = 0x1FF  # 9 header bits
    
    for row in range(9, -1, -1):
        nibble = (data_40 >> (row * 4)) & 0x0F
        result <<= 4
        result |= nibble
        # Row parity (even)
        parity = bin(nibble).count('1') % 2
        result <<= 1
        result |= parity
    
    # Column parity
    for col in range(3, -1, -1):
        parity_sum = 0
        for row in range(10):
            bit_pos = row * 4 + col
            parity_sum += (data_40 >> bit_pos) & 1
        result <<= 1
        result |= (parity_sum % 2)
    
    # Stop bit
    result <<= 1
    
    print(f"EM4100 ID: {id_bytes.hex().upper()}")
    print(f"EM4100 encoded (64-bit): {result:016X}")
    print(f"  Page 4 (hi): {(result >> 32) & 0xFFFFFFFF:08X}")
    print(f"  Page 5 (lo): {result & 0xFFFFFFFF:08X}")
    
    # Convert to bit array
    em_bits = []
    for i in range(63, -1, -1):
        em_bits.append((result >> i) & 1)
    
    print(f"  Bits: {''.join(str(b) for b in em_bits)}")
    
    return result, em_bits


def test_config_response():
    """
    Test decoding a config page response after SELECT.
    
    For an 8268 chip, a typical config might be:
    - CON0: 0x48 (MEMT=00 for 32-page)... actually for 256-page it would be 0x06
    - Let's use a realistic one
    """
    # Test with a known 32-bit value
    test_value = 0x060000E8  # Typical 8268 config
    bits = []
    for i in range(31, -1, -1):
        bits.append((test_value >> i) & 1)
    
    print(f"\n=== Config page test: {test_value:08X} ===")
    print(f"  Bits: {''.join(str(b) for b in bits)}")
    
    # Generate MC4K waveform (SOF=1 for STD mode)
    waveform = generate_mc4k_waveform(bits, sof_bits=1)
    print(f"  Waveform pulses ({len(waveform)}):")
    for i, (is_unload, dur) in enumerate(waveform):
        print(f"    [{i}] {'UNLOAD(H)' if is_unload else 'LOAD(L)':10s} {dur}µs")
    
    # Simulate TIM2 capture
    events_v1 = simulate_tim2_capture(waveform)
    events_v2 = simulate_tim2_capture_v2(waveform)
    
    print(f"\n  TIM2 capture v1 ({len(events_v1)} events):")
    for i, (level, dur) in enumerate(events_v1):
        print(f"    e[{i}] {'H' if level else 'L'} {dur}µs")
    
    print(f"\n  TIM2 capture v2 ({len(events_v2)} events):")
    for i, (level, dur) in enumerate(events_v2):
        print(f"    e[{i}] {'H' if level else 'L'} {dur}µs")
    
    # Decode with our algorithm
    print(f"\n  === Our decode (level→High, no inversion) ===")
    decoded, total = our_decode_mc4k(events_v2, threshold=192, sof_bits=1, max_bits=32)
    decoded_val = 0
    for b in decoded:
        decoded_val = (decoded_val << 1) | (1 if b else 0)
    print(f"  Decoded: {decoded_val:08X} ({len(decoded)} bits, {total} total)")
    print(f"  Expected: {test_value:08X}")
    print(f"  Match: {decoded_val == test_value}")
    
    # Also try inverted mapping
    print(f"\n  === Inverted decode (level→Low, no inversion) ===")
    # Temporarily swap mapping
    decoded2 = []
    state = ManchesterStateMid1
    sof_remaining = 1
    durations = []
    last_high_dur = 0
    for level, dur in events_v2:
        if level:
            durations.append(dur)
            last_high_dur = dur
        else:
            if last_high_dur > 0 and dur > last_high_dur:
                durations.append(dur - last_high_dur)
            else:
                durations.append(dur)
    
    for i, (dur, (level, _)) in enumerate(zip(durations, events_v2)):
        if dur < 50:
            continue
        is_short = dur < 192
        # INVERTED: level=true→Low, level=false→High
        if is_short:
            event = ManchesterEventShortLow if level else ManchesterEventShortHigh
        else:
            event = ManchesterEventLongLow if level else ManchesterEventLongHigh
        
        next_state, data = manchester_advance(state, event)
        if data is not None:
            if sof_remaining > 0:
                sof_remaining -= 1
            else:
                decoded2.append(data)
        state = next_state
    
    decoded2_val = 0
    for b in decoded2:
        decoded2_val = (decoded2_val << 1) | (1 if b else 0)
    print(f"  Decoded: {decoded2_val:08X} ({len(decoded2)} bits)")
    print(f"  Match: {decoded2_val == test_value}")
    
    # Try with data_bit inversion 
    print(f"\n  === level→Low + data_bit inversion ===")
    decoded3 = []
    state = ManchesterStateMid1
    sof_remaining = 1

    for i, (dur, (level, _)) in enumerate(zip(durations, events_v2)):
        if dur < 50:
            continue
        is_short = dur < 192
        if is_short:
            event = ManchesterEventShortLow if level else ManchesterEventShortHigh
        else:
            event = ManchesterEventLongLow if level else ManchesterEventLongHigh
        
        next_state, data = manchester_advance(state, event)
        if data is not None:
            data = not data  # INVERT
            if sof_remaining > 0:
                sof_remaining -= 1
            else:
                decoded3.append(data)
        state = next_state
    
    decoded3_val = 0
    for b in decoded3:
        decoded3_val = (decoded3_val << 1) | (1 if b else 0)
    print(f"  Decoded: {decoded3_val:08X} ({len(decoded3)} bits)")
    print(f"  Match: {decoded3_val == test_value}")
    
    # Try level→High + data_bit inversion
    print(f"\n  === level→High + data_bit inversion ===")
    decoded4 = []
    state = ManchesterStateMid1
    sof_remaining = 1

    for i, (dur, (level, _)) in enumerate(zip(durations, events_v2)):
        if dur < 50:
            continue
        is_short = dur < 192
        if is_short:
            event = ManchesterEventShortHigh if level else ManchesterEventShortLow
        else:
            event = ManchesterEventLongHigh if level else ManchesterEventLongLow
        
        next_state, data = manchester_advance(state, event)
        if data is not None:
            data = not data  # INVERT
            if sof_remaining > 0:
                sof_remaining -= 1
            else:
                decoded4.append(data)
        state = next_state
    
    decoded4_val = 0
    for b in decoded4:
        decoded4_val = (decoded4_val << 1) | (1 if b else 0)
    print(f"  Decoded: {decoded4_val:08X} ({len(decoded4)} bits)")
    print(f"  Match: {decoded4_val == test_value}")


def trace_state_machine():
    """
    Trace the state machine step by step for a simple bit sequence
    to verify the event mapping.
    """
    print("\n=== State Machine Trace ===")
    print("Testing bit sequence: 1, 0, 1, 1, 0, 0")
    
    test_bits = [1, 0, 1, 1, 0, 0]
    waveform = generate_mc4k_waveform(test_bits, sof_bits=0)
    
    print(f"\nWaveform ({len(waveform)} pulses):")
    for i, (is_unload, dur) in enumerate(waveform):
        tag = 'UNLOAD(H)' if is_unload else 'LOAD(L)'
        print(f"  [{i}] {tag:10s} {dur}µs")
    
    events = simulate_tim2_capture_v2(waveform)
    
    print(f"\nTIM2 capture ({len(events)} events):")
    for i, (level, dur) in enumerate(events):
        print(f"  e[{i}] {'H' if level else 'L'} {dur}µs")
    
    # Pre-process durations
    durations = []
    last_high_dur = 0
    for level, dur in events:
        if level:
            durations.append(dur)
            last_high_dur = dur
        else:
            if last_high_dur > 0 and dur > last_high_dur:
                durations.append(dur - last_high_dur)
            else:
                durations.append(dur)
    
    print(f"\nPre-processed durations:")
    for i, (dur, (level, orig)) in enumerate(zip(durations, events)):
        print(f"  [{i}] {'H' if level else 'L'} {dur}µs (orig={orig}µs)")
    
    # Trace with level→High mapping (our current code)
    print(f"\n--- Mapping: level→High ---")
    state = ManchesterStateMid1
    bits_out = []
    
    for i, (dur, (level, _)) in enumerate(zip(durations, events)):
        if dur < 50:
            print(f"  [{i}] skip glitch {dur}µs")
            continue
        
        is_short = dur < 192
        if is_short:
            event = ManchesterEventShortHigh if level else ManchesterEventShortLow
        else:
            event = ManchesterEventLongHigh if level else ManchesterEventLongLow
        
        next_state, data = manchester_advance(state, event)
        
        data_str = f"→ data={'1' if data else '0'}" if data is not None else "→ no output"
        reset_str = " [RESET]" if next_state == state and event != ManchesterEventReset else ""
        if data is None and next_state == ManchesterStateMid1 and state != ManchesterStateMid1:
            reset_str = ""
        
        print(f"  [{i}] {'H' if level else 'L'} {'S' if is_short else 'L'} {dur:3d}µs | "
              f"{state_names[state]:7s} + {event_names[event]:10s} → {state_names[next_state]:7s} {data_str}{reset_str}")
        
        if data is not None:
            bits_out.append(data)
        state = next_state
    
    print(f"\n  Output bits: {''.join('1' if b else '0' for b in bits_out)}")
    print(f"  Expected:    {''.join(str(b) for b in test_bits)}")
    print(f"  Match: {bits_out == [bool(b) for b in test_bits]}")
    
    # Trace with level→Low mapping (inverted)
    print(f"\n--- Mapping: level→Low ---")
    state = ManchesterStateMid1
    bits_out = []
    
    for i, (dur, (level, _)) in enumerate(zip(durations, events)):
        if dur < 50:
            continue
        
        is_short = dur < 192
        if is_short:
            event = ManchesterEventShortLow if level else ManchesterEventShortHigh
        else:
            event = ManchesterEventLongLow if level else ManchesterEventLongHigh
        
        next_state, data = manchester_advance(state, event)
        
        data_str = f"→ data={'1' if data else '0'}" if data is not None else "→ no output"
        print(f"  [{i}] {'H' if level else 'L'} {'S' if is_short else 'L'} {dur:3d}µs | "
              f"{state_names[state]:7s} + {event_names[event]:10s} → {state_names[next_state]:7s} {data_str}")
        
        if data is not None:
            bits_out.append(data)
        state = next_state
    
    print(f"\n  Output bits: {''.join('1' if b else '0' for b in bits_out)}")
    print(f"  Expected:    {''.join(str(b) for b in test_bits)}")
    print(f"  Match: {bits_out == [bool(b) for b in test_bits]}")


if __name__ == "__main__":
    print("=" * 70)
    print("Manchester Decoder Simulation for Hitag S MC4K")
    print("=" * 70)
    
    # First: trace the state machine step by step
    trace_state_machine()
    
    # Second: test with real data
    print("\n" + "=" * 70)
    test_config_response()
    
    # Third: generate EM4100 data
    print("\n" + "=" * 70)
    test_known_data()
