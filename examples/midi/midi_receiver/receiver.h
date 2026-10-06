#ifndef MIDI_RECEIVER_H
#define MIDI_RECEIVER_H

// GM1 processing from the SWP00 example
// Include after the board defines note_on, release, silence and update
// GM_PROGRAM_CHANGE selects program reception, GM_PERCUSSION_ONESHOT selects drum release
static void all_sound_off(Gm *s, u8 channel) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i)
        if (s->voices[i].note != 255 && s->voices[i].channel == channel) silence(s, i);
}
static void stop(Gm *s) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i) silence(s, i);
    for (i = 0; i < 16; ++i) s->channels[i].sustain = s->channels[i].sostenuto = 0;
    s->transport = 0;
}
static void controllers(GmChannel *c) {
    c->expression = 127; c->modulation = c->pressure = c->sustain = 0;
    c->sostenuto = c->soft = 0;
    c->bend = 8192; c->rpn_msb = c->rpn_lsb = 127;
}
void gm_reset(Gm *s) {
    u8 i;
    stop(s);
    for (i = 0; i < 16; ++i) {
        GmChannel *c = &s->channels[i];
        controllers(c);
        c->program = 0; c->volume = 100; c->pan = 64;
        c->bend_semitones = 2; c->bend_cents = 0; c->fine = 8192; c->coarse = 64;
    }
    s->age = 0; s->running = s->have_first = s->sysex_length = 0;
    s->enabled = 1; s->master = 127;
    s->master_fine = 8192; s->master_coarse = 64;
}
static void pedal_up(Gm *s, u8 channel) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i)
        if (s->voices[i].active && !s->voices[i].held && s->voices[i].channel == channel &&
            !s->channels[channel].sustain &&
            !(s->channels[channel].sostenuto && s->voices[i].latched)) release(s, i);
}
static void notes_off(Gm *s, u8 channel) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i) if (s->voices[i].note != 255 && s->voices[i].channel == channel) {
        s->voices[i].held = 0;
        if (!s->channels[channel].sustain &&
            !(s->channels[channel].sostenuto && s->voices[i].latched) && s->voices[i].active) release(s, i);
    }
}
static void note_off(Gm *s, u8 channel, u8 note) {
    u8 i, oldest = 255;
    for (i = 0; i < GM_VOICES; ++i) {
        GmVoice *v = &s->voices[i];
        if (v->active && v->held && v->channel == channel && v->note == note &&
            (oldest == 255 || s->age - v->age > s->age - s->voices[oldest].age)) oldest = i;
    }
    if (GM_PERCUSSION_ONESHOT && channel == 9 && oldest != 255) {
        // Let one-shot drums finish while making their software slots reusable
        s->voices[oldest].active = s->voices[oldest].held = 0;
        return;
    }
    if (oldest != 255) {
        u32 age = s->voices[oldest].age;
        for (i = 0; i < GM_VOICES; ++i) if (s->voices[i].active && s->voices[i].age == age) {
            s->voices[i].held = 0;
            if (!s->channels[channel].sustain &&
                !(s->channels[channel].sostenuto && s->voices[i].latched)) release(s, i);
        }
    }
}
static void data_entry(Gm *s, u8 channel, u8 cc, u8 value) {
    GmChannel *c = &s->channels[channel];
    if (c->rpn_msb) return;
    if (c->rpn_lsb == 0) {
        if (cc == 6) c->bend_semitones = value;
        else c->bend_cents = (u8)clamp(value, 0, 99);
    } else if (c->rpn_lsb == 1) {
        if (cc == 6) c->fine = (u16)((c->fine & 127u) | (u16)value << 7);
        else c->fine = (u16)((c->fine & 16256u) | value);
    } else if (c->rpn_lsb == 2 && cc == 6) c->coarse = value;
    update(s, channel, 1);
}
static void control(Gm *s, u8 channel, u8 cc, u8 value) {
    GmChannel *c = &s->channels[channel];
    switch (cc) {
    case 1: c->modulation = value; update(s, channel, 4); break;
    case 7: c->volume = value; update(s, channel, 2); break;
    case 10: c->pan = value; update(s, channel, 2); break;
    case 11: c->expression = value; update(s, channel, 2); break;
    case 64: c->sustain = value >= 64; if (!c->sustain) pedal_up(s, channel); break;
    case 66:
        if (value >= 64 && !c->sostenuto) {
            u8 i;
            for (i = 0; i < GM_VOICES; ++i)
                if (s->voices[i].channel == channel) s->voices[i].latched = s->voices[i].held;
        }
        c->sostenuto = value >= 64;
        if (!c->sostenuto) pedal_up(s, channel);
        break;
    case 67: c->soft = value >= 64; update(s, channel, 2); break;
    case 100: c->rpn_lsb = value; break;
    case 101: c->rpn_msb = value; break;
    case 98: case 99: c->rpn_msb = c->rpn_lsb = 127; break;
    case 6: case 38: data_entry(s, channel, cc, value); break;
    case 120: all_sound_off(s, channel); break;
    case 121: controllers(c); pedal_up(s, channel); update(s, channel, 7); break;
    case 123: case 124: case 125: case 126: case 127: notes_off(s, channel); break;
    default: break;
    }
}

void gm_control(Gm *s, u8 channel, u8 controller, u8 value) {
    if (channel < 16 && value < 128) control(s, channel, controller, value);
}
static void sysex(Gm *s) {
    const u8 *b = s->sysex;
    if (s->sysex_length > sizeof s->sysex + 1u) return;
    if (s->sysex_length < 5) return;
    if (b[1] != 0x7f && b[1] != 0) return; // device 0 or broadcast
    if (s->sysex_length == 5 && b[0] == 0x7e && b[2] == 9) {
        if (b[3] == 1) gm_reset(s);
        else if (b[3] == 2) { stop(s); s->enabled = 0; }
    } else if (s->sysex_length == 7 && b[0] == 0x7f && b[2] == 4 && b[3] == 1) {
        u8 i;
        s->master = b[5];
        for (i = 0; i < 16; ++i) update(s, i, 2);
    } else if (s->sysex_length == 7 && b[0] == 0x7f && b[2] == 4 &&
               (b[3] == 3 || b[3] == 4)) {
        u8 i;
        if (b[3] == 3) s->master_fine = (u16)((u16)b[5] << 7 | b[4]);
        else s->master_coarse = b[5];
        for (i = 0; i < 16; ++i) update(s, i, 1);
    }
}
void gm_byte(Gm *s, u8 byte) {
    u8 kind, channel;
    if (byte >= 0xf8) {
        if (byte == 0xff) gm_reset(s);
        else if (byte == 0xfc) stop(s);
        else if (byte == 0xfa) { stop(s); s->transport = 1; }
        else if (byte == 0xfb) s->transport = 1;
        return; // Real-time bytes do not interrupt running status or SysEx
    }
    if (s->sysex_length) {
        if (byte == 0xf7) { sysex(s); s->sysex_length = 0; return; }
        if (byte < 128) {
            if (s->sysex_length <= sizeof s->sysex) s->sysex[s->sysex_length - 1] = byte;
            if (s->sysex_length < 255) ++s->sysex_length;
            return;
        }
        s->sysex_length = 0;
    }
    if (byte & 128) {
        s->have_first = 0; s->running = byte < 0xf0 ? byte : 0;
        if (byte == 0xf0) s->sysex_length = 1;
        return;
    }
    if (!s->running) return;
    kind = s->running & 0xf0; channel = s->running & 15;
    if (kind == 0xc0) {
#if GM_PROGRAM_CHANGE
        s->channels[channel].program = byte;
#endif
        return;
    }
    if (kind == 0xd0) { s->channels[channel].pressure = byte; update(s, channel, 4); return; }
    if (!s->have_first) { s->first = byte; s->have_first = 1; return; }
    s->have_first = 0;
    if (kind == 0x90) note_on(s, channel, s->first, byte);
    else if (kind == 0x80) note_off(s, channel, s->first);
    else if (kind == 0xb0) control(s, channel, s->first, byte);
    else if (kind == 0xe0) {
        s->channels[channel].bend = (u16)((u16)byte << 7 | s->first);
        update(s, channel, 1);
    }
}

#endif
