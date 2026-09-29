# PHP script card proof of concept

Requires the optional Linux/glibc PHP backend. Build instructions and limitations:
[script card](../../plugins/cards/script/README.md#experimental-php-backend).

PHP building is disabled by default. If you downloaded
your plugin pack from GitHub, then your copy does not have
PHP enabled. It's also Linux only.

Open `project.json` in the host and start the simulation. The card computes a sine
wave byte every 250 ms and logs its value once per simulated second. Reads from
`demo.io:0xF000` return the latest byte. No emulated CPU is required.

`main.php` demonstrates PHP math, `preg_match`, project-file reads, timers,
logging and the memory callbacks. Change `step` in `settings.txt` while paused,
then resume to reload the script with the new setting.

Use `project_write('output.txt', $bytes)` for writes through the host helper.
Native PHP file functions and network APIs are unavailable.

For a larger timer-driven example, see the
[four-channel MOD player](../script-php-mod/README.md), which accepts a
user-supplied ProTracker module and feeds four PCM DAC cards.
