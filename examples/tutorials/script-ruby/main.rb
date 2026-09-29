# The script card maps one byte at demo.io:0x10. Its hooks run when the host
# resets the card or reads and writes that address.
$state = {"value" => 0, "ticks" => 0}

def schedule_tick
  Card.after(250_000_000, Proc.new { |time| tick(time) })
end

def tick(time)
  $state["value"] = ($state["value"] + $step) & 255
  $state["ticks"] += 1
  Card.log("Ruby counter: #{$state['value']} at #{time} ns") if ($state["ticks"] % 4) == 0
  schedule_tick
end

def on_reset(cold)
  setting = Project.read("settings.txt")
  raise "settings.txt must contain step=<number>" unless setting[0, 5] == "step="
  $step = setting[5, setting.length - 5].to_i
  raise "step must be an integer from 1 to 255" unless $step >= 1 && $step <= 255 &&
    (setting == "step=#{$step}" || setting == "step=#{$step}\n")

  $state["value"] = 0
  $state["ticks"] = 0
  Card.log("Ruby counter started (step #{$step})")
  schedule_tick
end

def on_read(address)
  $state["value"]
end

def on_write(address, value)
  $state["value"] = value
end
