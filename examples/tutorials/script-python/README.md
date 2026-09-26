# Python script card tutorial

Open `project.json` in SRZ80 and start the simulation. The script card selects
pocketpy because `main_file` ends in `.py`. It maps one byte at `demo.io:0x10`.
Every 250 ms of simulated time, `tick()` adds the step from `settings.txt` to
the byte. Every fourth tick writes a message to the host log.

Read address `0x10` in the `demo.io` space to see the counter. Write a byte to
that address to change it. The mapped `on_read` and `on_write` functions run
for bus accesses; memory inspector peeks do not call them.

Change `step=3` in `settings.txt`, pause, and resume. The script card detects
the changed project file, reloads the Python VM, and starts with the new step.
The global `state` dictionary is the card's snapshot data. Save and load a
snapshot to restore its counter and tick count. Timers restart on reset and
are not stored in snapshots.

`card.log`, `card.read`, `card.write`, `card.time_ns`, and `card.after` expose
host services. `project.read` returns bytes, and `project.write` accepts bytes.
Both project functions use paths relative to the project folder. The pocketpy
build disables operating system, thread, and native module access.
