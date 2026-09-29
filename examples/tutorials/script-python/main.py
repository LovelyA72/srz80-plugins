import card
import project

# Mapped byte at demo.io:0x10. Python state is saved with the card snapshot.
state = {"value": 0, "ticks": 0}

def tick(now):
    state["value"] = (state["value"] + step) & 255
    state["ticks"] += 1
    if state["ticks"] % 4 == 0:
        card.log("Python counter: %d at %d ns" % (state["value"], now))
    card.after(250000000, tick)

def on_reset(cold):
    global step
    setting = project.read("settings.txt").decode().strip()
    if not setting.startswith("step="):
        raise ValueError("settings.txt must contain step=<number>")
    step = int(setting[5:])
    if step < 1 or step > 255:
        raise ValueError("step must be between 1 and 255")
    state["value"] = 0
    state["ticks"] = 0
    card.log("Python counter started (step %d)" % step)
    card.after(250000000, tick)

def on_read(address):
    return state["value"]

def on_write(address, value):
    state["value"] = value
