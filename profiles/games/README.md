# Per-game profiles

Name a file after the game's Steam AppID (the number in its store URL, also
exported to the game as `SteamAppId`), e.g. `games/12345.ini`, and put only
the keys that differ from `default.ini`:

```ini
[display]
mode = immersive
game_fov = 103

[stereo]
separation = 0.02
convergence = 0.05

[input]
mouse_counts_per_degree = 22.7
```

Profiles are searched in this order (later ones win):

1. `<prefix>/share/vrlayer/profiles/default.ini`
2. `<prefix>/share/vrlayer/profiles/games/<appid>.ini`
3. `~/.config/vrlayer/default.ini`
4. `~/.config/vrlayer/games/<appid>.ini`

The quickest way to make one is to tune the game live with
`vrlayer-ctl set ...` and then run `vrlayer-ctl save`.
