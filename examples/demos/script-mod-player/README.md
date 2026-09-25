# Lua MOD player

Open `project.json` in SRZ80 and run the simulation. The Lua script reads
`chromag_-_abyss.mod` from the project, plays its four channels through four
PCM DAC cards, and loops at the song's restart order. No CPU or ROM is needed.

To play another classic four-channel ProTracker MOD, replace the bundled MOD
file with one of the same name. The player accepts `M.K.`, `M!K!`, `4CHN`, and
`FLT4` signatures. The script card is listed after the DACs so its reset
callback can initialize them and schedule FIFO fills in simulated time.
