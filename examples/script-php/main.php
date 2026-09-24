<?php
declare(strict_types=1);

$state = (object) ['value' => 0, 'ticks' => 0, 'step' => 15];

function on_reset(bool $cold): void
{
    global $state;
    $settings = project_read('settings.txt');
    if (preg_match('/^step=(\d+)$/m', $settings, $match) !== 1) {
        throw new Exception('settings.txt needs step=<degrees>');
    }
    $state->step = max(1, min(180, (int) $match[1]));
    $state->ticks = 0;
    $state->value = 128;
    card_log('PHP script started');
    card_after(250_000_000, 'tick');
}

function tick(int $time): void
{
    global $state;
    ++$state->ticks;
    $state->value = (int) round(127.5 + 127.5 * sin(deg2rad($state->ticks * $state->step)));
    if ($state->ticks % 4 === 0) {
        card_log(sprintf('t=%.2fs value=%d', $time / 1e9, $state->value));
    }
    card_after(250_000_000, 'tick');
}

function on_read(int $address): int
{
    global $state;
    return $state->value;
}

function on_write(int $address, int $value): void
{
    global $state;
    $state->value = $value;
}
