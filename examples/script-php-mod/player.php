<?php
// I am sorry about this...
declare(strict_types=1);

const MODULE_FILE = 'enigma.mod';

$pt = new PTPlayer();

function on_reset(bool $cold): void
{
    global $pt;
    $pt->reset();
}

final class PTPlayer
{
    private const SPACE = 'mod.io';
    private const RATE = 22050;
    private const BLOCK = 16;
    private const DAC = [0x10, 0x20, 0x30, 0x40];
    private const PERIODS = [
        856,808,762,720,678,640,604,570,538,508,480,453,
        428,404,381,360,339,320,302,285,269,254,240,226,
        214,202,190,180,170,160,151,143,135,127,120,113,
    ];
    private const TUNING = [
        1,1.007246,1.014545,1.021897,1.029302,1.036762,1.044274,1.051842,
        .943874,.950714,.957603,.964542,.971532,.978572,.985663,.992806,
    ];
    private const SINE = [
        0,24,49,74,97,120,141,161,180,197,212,224,235,244,250,253,
        255,253,250,244,235,224,212,197,180,161,141,120,97,74,49,24,
    ];

    private string $mod = '';
    private array $samples = [];
    private array $voices = [];
    private int $orders = 0;
    private int $restart = 0;
    private int $order = 0;
    private int $row = 0;
    private int $tick = 0;
    private int $speed = 6;
    private int $bpm = 125;
    private int $jump = -1;
    private int $breakrow = -1;
    private int $looprow = -1;
    private int $delayrows = 0;
    private float $remaining = 0;
    private int $played = 0;
    private float $deadline = 0;

    private function byte(int $offset): int
    {
        return ord($this->mod[$offset]); // MOD offsets are zero based.
    }

    private function be16(int $offset): int
    {
        return $this->byte($offset) * 256 + $this->byte($offset + 1);
    }

    private static function clamp(int $n, int $lo, int $hi): int
    {
        return max($lo, min($hi, $n));
    }

    private static function nearest(int $period): int
    {
        $best = 0;
        $distance = PHP_INT_MAX;
        foreach (self::PERIODS as $index => $candidate) {
            $d = abs($period - $candidate);
            if ($d < $distance) {
                $best = $index;
                $distance = $d;
            }
        }
        return $best;
    }

    private function instrument(stdClass $v, int $number): void
    {
        if ($number > 0 && $number <= 31) {
            $v->sample = $this->samples[$number];
            $v->volume = $v->sample->volume;
            $v->fine = $v->sample->fine;
        }
    }

    private static function trigger(stdClass $v, int $period, int $offset): void
    {
        if ($period !== 0) {
            $v->target = $period;
            $v->period = $period;
        }
        $v->playing = $v->sample;
        $v->position = $offset;
        if (($v->vibwave & 4) === 0) $v->vibphase = 0;
        if (($v->tremwave & 4) === 0) $v->tremphase = 0;
    }

    private static function slideVolume(stdClass $v): void
    {
        $hi = $v->param >> 4;
        $lo = $v->param & 15;
        $v->volume = self::clamp($v->volume + ($hi !== 0 ? $hi : -$lo), 0, 64);
    }

    private static function portamento(stdClass $v): void
    {
        if ($v->target === 0) return;
        if ($v->period < $v->target) {
            $v->period = self::clamp($v->period + $v->porta, 1, $v->target);
        } elseif ($v->period > $v->target) {
            $v->period = self::clamp($v->period - $v->porta, $v->target, 4095);
        }
    }

    private static function wave(int $phase, int $shape): int
    {
        if (($shape & 3) === 1) $n = 255 - ($phase & 31) * 16;
        elseif (($shape & 3) === 2) $n = 255;
        else $n = self::SINE[$phase & 31];
        return ($phase & 32) !== 0 ? -$n : $n;
    }

    private function readRow(): void
    {
        $this->jump = $this->breakrow = $this->looprow = -1;
        foreach ($this->voices as $channel => $v) {
            $p = 1084 + $this->byte(952 + $this->order) * 1024 + $this->row * 16 + $channel * 4;
            $b0 = $this->byte($p);
            $b2 = $this->byte($p + 2);
            $note = (($b0 & 15) << 8) | $this->byte($p + 1);
            $ins = ($b0 & 240) | ($b2 >> 4);
            $e = $b2 & 15;
            $x = $this->byte($p + 3);
            $hi = $x >> 4;
            $lo = $x & 15;
            $v->effect = $e;
            $v->param = $x;
            $v->delayed_period = $v->delayed_sample = 0;
            if ($e === 14 && $hi === 13 && $lo !== 0) {
                $v->delayed_period = $note;
                $v->delayed_sample = $ins;
            } else {
                $this->instrument($v, $ins);
                if ($e === 14 && $hi === 5) $v->fine = $lo;
                if ($e === 9 && $x !== 0) $v->offset = $x * 256;
                if ($note !== 0) {
                    if ($e === 3 || $e === 5) {
                        $v->target = $note;
                        if ($v->period === 0) $v->period = $note;
                    } else {
                        self::trigger($v, $note, $e === 9 ? $v->offset : 0);
                    }
                }
            }
            if ($e === 3 && $x !== 0) $v->porta = $x;
            if ($e === 4) {
                if ($hi !== 0) $v->vibrato = ($v->vibrato & 15) | ($hi << 4);
                if ($lo !== 0) $v->vibrato = ($v->vibrato & 240) | $lo;
            }
            if ($e === 7) {
                if ($hi !== 0) $v->tremolo = ($v->tremolo & 15) | ($hi << 4);
                if ($lo !== 0) $v->tremolo = ($v->tremolo & 240) | $lo;
            }
            if ($e === 11) $this->jump = $x;
            if ($e === 12) $v->volume = self::clamp($x, 0, 64);
            if ($e === 13) $this->breakrow = self::clamp($hi * 10 + $lo, 0, 63);
            if ($e === 15 && $x !== 0) {
                if ($x < 32) $this->speed = $x;
                else $this->bpm = $x;
            }
            if ($e === 14) {
                if ($hi === 1 && $v->period !== 0) $v->period = self::clamp($v->period - $lo, 113, 856);
                elseif ($hi === 2 && $v->period !== 0) $v->period = self::clamp($v->period + $lo, 113, 856);
                elseif ($hi === 3) $v->gliss = $lo;
                elseif ($hi === 4) $v->vibwave = $lo;
                elseif ($hi === 6) {
                    if ($lo === 0) $v->looprow = $this->row;
                    else {
                        $v->loops = $v->loops === 0 ? $lo : $v->loops - 1;
                        if ($v->loops !== 0) $this->looprow = $v->looprow;
                    }
                } elseif ($hi === 7) $v->tremwave = $lo;
                elseif ($hi === 10) $v->volume = self::clamp($v->volume + $lo, 0, 64);
                elseif ($hi === 11) $v->volume = self::clamp($v->volume - $lo, 0, 64);
                elseif ($hi === 14) $this->delayrows = $lo;
            }
        }
    }

    private function effects(): void
    {
        foreach ($this->voices as $v) {
            $e = $v->effect;
            $x = $v->param;
            if ($this->tick !== 0) {
                if ($e === 1 && $v->period !== 0) $v->period = self::clamp($v->period - $x, 113, 856);
                elseif ($e === 2 && $v->period !== 0) $v->period = self::clamp($v->period + $x, 113, 856);
                elseif ($e === 3) self::portamento($v);
                elseif ($e === 5) { self::portamento($v); self::slideVolume($v); }
                elseif ($e === 6 || $e === 10) self::slideVolume($v);
            }
            if ($e === 14) {
                $hi = $x >> 4;
                $lo = $x & 15;
                if ($hi === 9 && $lo !== 0 && $this->tick !== 0 && $this->tick % $lo === 0) {
                    self::trigger($v, 0, 0);
                }
                if ($hi === 12 && $this->tick === $lo) $v->volume = 0;
                if ($hi === 13 && $this->tick === $lo && $lo !== 0) {
                    $this->instrument($v, $v->delayed_sample);
                    if ($v->delayed_period !== 0) self::trigger($v, $v->delayed_period, 0);
                }
            }
            $p = $v->period;
            $volume = $v->volume;
            if ($v->gliss !== 0 && ($e === 3 || $e === 5) && $p !== 0) {
                $p = self::PERIODS[self::nearest($p)];
            }
            if ($e === 0 && $x !== 0 && $p !== 0) {
                $semi = $this->tick % 3 === 1 ? ($x >> 4) : ($this->tick % 3 === 2 ? ($x & 15) : 0);
                if ($semi !== 0) $p = self::PERIODS[self::clamp(self::nearest($p) + $semi, 0, 35)];
            }
            if ($e === 4 || $e === 6) {
                // PHP casts truncate toward zero, like Lua's math.modf here.
                $p += (int) (self::wave($v->vibphase, $v->vibwave) * ($v->vibrato & 15) / 128);
                if ($this->tick !== 0) $v->vibphase = ($v->vibphase + ($v->vibrato >> 4)) & 63;
            }
            if ($e === 7) {
                $volume = self::clamp($volume + (int) (self::wave($v->tremphase, $v->tremwave) * ($v->tremolo & 15) / 64), 0, 64);
                if ($this->tick !== 0) $v->tremphase = ($v->tremphase + ($v->tremolo >> 4)) & 63;
            }
            $v->output_volume = $volume;
            $v->step = $p > 0 ? (3546894.6 / self::RATE) * self::TUNING[$v->fine & 15] / $p : 0;
        }
    }

    private function advanceRow(): void
    {
        $old = $this->order;
        if ($this->jump >= 0 || $this->breakrow >= 0) {
            $this->order = $this->jump >= 0 ? $this->jump : $this->order + 1;
            $this->row = $this->breakrow >= 0 ? $this->breakrow : 0;
        } elseif ($this->looprow >= 0) {
            $this->row = $this->looprow;
        } else {
            ++$this->row;
            if ($this->row === 64) { $this->row = 0; ++$this->order; }
        }
        if ($this->order >= $this->orders) $this->order = $this->restart;
        if ($this->order !== $old) {
            foreach ($this->voices as $v) { $v->looprow = 0; $v->loops = 0; }
        }
    }

    private function trackerTick(): void
    {
        if ($this->played !== 0) {
            ++$this->tick;
            if ($this->tick >= $this->speed) {
                $this->tick = 0;
                if ($this->delayrows !== 0) --$this->delayrows;
                else { $this->advanceRow(); $this->readRow(); }
            }
        } else {
            $this->readRow();
        }
        $this->effects();
        ++$this->played;
    }

    private function voiceFrame(stdClass $v): int
    {
        $s = $v->playing;
        if ($s === null || $s->length === 0 || $v->period === 0) return 128;
        $last = $s->replen !== 0 ? $s->loop + $s->replen : $s->length;
        if ($v->position >= $last) {
            if ($s->replen === 0) { $v->playing = null; return 128; }
            $v->position -= (floor(($v->position - $last) / $s->replen) + 1) * $s->replen;
        }
        $value = $this->byte($s->start + (int) floor($v->position));
        if ($value >= 128) $value -= 256;
        $v->position += $v->step;
        return self::clamp(128 + (int) ($value * $v->output_volume / 256), 0, 255);
    }

    private function renderFrame(): void
    {
        if ($this->remaining <= 0) {
            $this->trackerTick();
            $this->remaining += self::RATE * 2.5 / $this->bpm;
        }
        foreach ($this->voices as $channel => $v) {
            card_write(self::SPACE, self::DAC[$channel], $this->voiceFrame($v));
        }
        --$this->remaining;
    }

    public function fill(int $now): void
    {
        for ($i = 0; $i < self::BLOCK; ++$i) $this->renderFrame();
        $this->deadline += self::BLOCK * 1_000_000_000 / self::RATE;
        card_after(max(1, (int) floor($this->deadline - $now + 0.5)), [$this, 'fill']);
    }

    public function reset(): void
    {
        try {
            $this->mod = project_read(MODULE_FILE);
        } catch (Throwable $error) {
            throw new Exception('Cannot load ' . MODULE_FILE . '. Place your own four-channel MOD beside player.php. ' . $error->getMessage());
        }
        $size = strlen($this->mod);
        if ($size < 1084) throw new Exception('MOD header is truncated');
        if (!in_array(substr($this->mod, 1080, 4), ['M.K.', 'M!K!', '4CHN', 'FLT4'], true)) {
            throw new Exception('MOD must have four channels');
        }
        $this->orders = $this->byte(950);
        if ($this->orders < 1 || $this->orders > 128) throw new Exception('Invalid MOD order count');
        $patterns = 0;
        for ($i = 0; $i < 128; ++$i) {
            $number = $this->byte(952 + $i);
            if ($number > 127) throw new Exception('Invalid MOD pattern number');
            $patterns = max($patterns, $number + 1);
        }
        $pos = 1084 + $patterns * 1024;
        if ($pos > $size) throw new Exception('MOD patterns are truncated');
        $this->samples = [];
        $missing = 0;
        for ($i = 1; $i <= 31; ++$i) {
            $h = 20 + ($i - 1) * 30;
            $declared = $this->be16($h + 22) * 2;
            // Decode only bytes actually present, as tolerant MOD loaders do.
            // Keep declared offsets for following samples; never rewrite the file.
            $length = min($declared, max(0, $size - $pos));
            $missing += $declared - $length;
            $s = (object) [
                'start' => $pos, 'length' => $length, 'fine' => $this->byte($h + 24) & 15,
                'volume' => self::clamp($this->byte($h + 25), 0, 64),
                'loop' => $this->be16($h + 26) * 2, 'replen' => $this->be16($h + 28) * 2,
            ];
            $pos += $declared;
            if ($s->loop >= $length) $s->replen = 0;
            elseif ($s->replen > $length - $s->loop) $s->replen = $length - $s->loop;
            if ($s->replen <= 2) $s->replen = 0;
            $this->samples[$i] = $s;
        }
        if ($missing !== 0) {
            card_log('MOD sample data ends ' . $missing . ' bytes early; affected samples shortened for playback.');
        }
        $this->restart = $this->byte(951) < $this->orders ? $this->byte(951) : 0;
        $this->voices = [];
        foreach (self::DAC as $base) {
            $this->voices[] = (object) [
                'sample'=>null, 'playing'=>null, 'position'=>0, 'step'=>0,
                'period'=>0, 'target'=>0, 'volume'=>0, 'output_volume'=>0,
                'fine'=>0, 'effect'=>0, 'param'=>0, 'porta'=>0, 'offset'=>0,
                'vibrato'=>0, 'tremolo'=>0, 'vibphase'=>0, 'tremphase'=>0,
                'vibwave'=>0, 'tremwave'=>0, 'gliss'=>0, 'looprow'=>0, 'loops'=>0,
                'delayed_period'=>0, 'delayed_sample'=>0,
            ];
            card_write(self::SPACE, $base + 1, 2);
            card_write(self::SPACE, $base, 128);
            card_write(self::SPACE, $base + 3, self::RATE & 255);
            card_write(self::SPACE, $base + 4, self::RATE >> 8);
            card_write(self::SPACE, $base + 1, 3);
        }
        $this->order = $this->row = $this->tick = 0;
        $this->speed = 6;
        $this->bpm = 125;
        $this->jump = $this->breakrow = $this->looprow = -1;
        $this->delayrows = $this->played = 0;
        $this->remaining = 0;
        $this->deadline = card_time_ns();
        // Prime each FIFO, then refill at the DAC consumption rate.
        for ($i = 0; $i < 32; ++$i) $this->renderFrame();
        $this->deadline += self::BLOCK * 1_000_000_000 / self::RATE;
        card_after((int) floor($this->deadline - card_time_ns() + 0.5), [$this, 'fill']);
    }
}
