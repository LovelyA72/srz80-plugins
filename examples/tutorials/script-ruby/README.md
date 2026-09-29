# Ruby script card demo

Open `project.json` in SRZ80 and start the simulation. The script card maps one
byte at `demo.io:0x10`; no CPU or ROM is needed. Reads return a counter that
increases by 7 every 250 ms of **simulated** time and wraps at 255. A write to
the byte sets the current value. The host log shows the value once per simulated
second.

`main.rb` shows the three card hooks (`on_reset`, `on_read`, `on_write`), a
recurring `Card.after` callback, `Card.log`, and `Project.read`. Change the
number in `settings.txt` to change the step. While the simulation is paused,
edit the file and resume to reload the script with the new setting.

The global `$state` is a JSON-compatible Hash, so the script card can save its
counter in a snapshot. Pending timers are recreated by `on_reset` when a new
run starts; they are not stored in snapshots. See the
[script card documentation](../../plugins/cards/script/README.md) for the full
Ruby API.
