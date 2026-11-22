# Game SFX profile v1

`game_sfx.v1` is the first domain profile used by the External AI/MCP
connector. It organizes game sound effects into Harken Playlists and child
Playlists. It does not move, copy, rename, export or rewrite physical files.

## Domain

Game SFX are sounds attached to an interaction or authored game event:

- player or character actions and reactions;
- combat, weapons and abilities;
- world objects and environment interactions;
- UI and system feedback;
- environmental Foley and authored ambience cues;
- cinematic, reveal and transition events;
- narrative-triggered one-shot effects.

Music is a separate domain and is excluded from this profile. Full tracks,
stems, soundtrack, score, BPM/tempo/key-oriented material and complete songs
are not placed in `game_sfx`, even when their file format or duration looks
similar to an effect.

## Taxonomy

The default child Playlist taxonomy is:

1. `Gameplay Feedback`
2. `Character & Creature`
3. `Combat & Ability`
4. `World & Props`
5. `UI & System`
6. `Environment & Foley`
7. `Cinematic & Transition`
8. `Review Needed`

`domain=game_sfx` is independent from `style`. Style may be `realistic`,
`cinematic`, `cartoon`, `stylized`, `sci-fi`, or another user-provided label;
style changes the requested Playlist pack metadata, not the authorization
boundary or the taxonomy domain.

## Evidence and output

Evidence is ordered from stronger to weaker context:

1. pack/vendor or official product material supplied by the calling Skill or
   upper layer;
2. folder and file names;
3. existing library metadata;
4. audio evidence when an authorized analyzer supplies it.

The MCP connector does not fetch websites or call an AI provider. The preview
currently uses only authorized filename/path and metadata evidence, reports
that source explicitly, and sends uncertain rows to `Review Needed`.

Every preview selection reports:

- `playlist_path` — the proposed Harken Playlist plus child Playlist;
- `confidence` — a bounded score from 0 to 1;
- `reason` — the evidence used for the classification;
- `source` — the evidence source label;
- `needs_review` — whether a human review is required.

The preview is read-only. Apply checks confirmation and the independent
`readPlaylist` and `editPlaylist` grants. Without a trusted Harken host
connection it returns `playlist_write_unavailable`: it makes no changes to
Playlist hierarchy, membership or physical files. `editMetadata` never
substitutes for Playlist permission.
