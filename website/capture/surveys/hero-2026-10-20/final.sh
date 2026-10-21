#!/bin/bash
cd ~/halo-capture-runs
M=chillout:team_slayer+rocket,prisoner:team_slayer+rocket,hangemhigh:team_slayer+rocket,damnation:team_slayer+rocket,bloodgulch:team_slayer+rocket
M=$M,bloodgulch:slayer+sniper,bloodgulch:team_slayer+sniper,longest:team_slayer+sniper,sidewinder:slayer+sniper
M=$M,bloodgulch:slayer+warthog,sidewinder:slayer+warthog
M=$M,hangemhigh:slayer,bloodgulch:slayer,damnation:slayer
start=$SECONDS
~/.cache/capture-venv/bin/python tools/scout.py $M --seeds 7,8,9 --bots 15 --out final-scout --parallel 10 > final-scout.log 2>&1
echo "scout $((SECONDS - start)) s"
~/.cache/capture-venv/bin/python tools/shoot.py final-scout --count 30 --per-kind 7 --size 960x540 --parallel 4 > final-shoot.log 2>&1
echo "scout and previews $((SECONDS - start)) s"
touch final.done
