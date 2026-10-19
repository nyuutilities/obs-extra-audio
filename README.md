# OBS Extra Audio 0.11.14

OBS Extra Audio adds up to ten extra audio tracks, numbered **7–16**, to your OBS recordings and replay clips. Each extra track is its own mix of the sources you choose (for example only your microphone, or only Discord). It's saved as a separate, named stereo AAC stream inside the original MP4, so an editor can mute or process each one on its own.

It works for:

- main OBS recordings and **OBS Replay Buffer** clips;
- **Aitum Vertical** recordings and **Backtrack** clips.

Extra tracks are recording-only. To have a source in your live stream too, also tick its Track 1 (OBS's stream track by default) in Advanced Audio Properties. If you use OBS's Twitch VOD track, it works the same way. It doesn't add video encoders or re-encode video. Optionally, it also saves each extra track as an MP3 backup.

- **Shared as is:** I built this for my own streams and I'm sharing it in case it helps others. It's tested on one setup (portable OBS 32.2.2 with Aitum Vertical 1.6.6), and I can't offer much support. If anything fails, your original recording is kept; for streams that matter, turn on MP3 backups and check your first few recordings.
- **Source code:** the plugin is free software under GPL-2.0-or-later. Its source and build scripts are published with each release.

## Contents

- [Requirements](#requirements)
- [Install, update and uninstall](#install-update-and-uninstall)
- [Set up your tracks](#set-up-your-tracks)
- [Settings reference](#settings-reference)
- [How recording, pausing and clips work](#how-recording-pausing-and-clips-work)
- [MP3 backups](#mp3-backups)
- [Five habits](#five-habits)
- [Limitations](#limitations)
- [Resource use](#resource-use)
- [Troubleshooting and recovery](#troubleshooting-and-recovery)

## Requirements

| | Required | Notes |
|---|---|---|
| OBS | **32.2.2 or newer**, Windows x64 | Tested with portable OBS 32.2.2. On a newer OBS the plugin loads, and the Status box says it isn't tested with that version yet (see [Limitations](#limitations)). Older versions are refused. |
| Aitum Vertical | **1.6.6 or newer** (optional) | Needed only for vertical recordings and Backtrack clips. Tested with 1.6.6; a newer version works with a "not yet tested" note in the Status box. With an older version, vertical outputs get no extra tracks and the Status box says why. Main recordings are unaffected either way. |
| Output mode | **Advanced** | The encoder and MP3 settings are read from Advanced output settings. |
| Recording format | **MP4 or Hybrid MP4** | Extra tracks are added to the MP4 after it's finished. |
| Audio | Stereo | Your recording audio encoder must be an AAC encoder (CoreAudio AAC or FFmpeg AAC). |

## Install, update and uninstall

### Install or update

1. Close OBS.
2. Extract `obs-extra-audio-<version>-windows-x64.zip` into your OBS folder, the one that contains `bin`, `data` and `obs-plugins`:
   - for portable OBS, the folder you unpacked it to;
   - for installed OBS, usually `C:\Program Files\obs-studio`.

   When updating, let it replace the existing files.
3. Start OBS. **Tools > Plugin Manager** should list **OBS Extra Audio** as enabled, and the OBS log shows `[extra-audio] Loaded <version>`.

The ZIP adds only these files:

| File | Purpose |
|---|---|
| `obs-plugins\64bit\obs-extra-audio.dll` | the plugin |
| `bin\64bit\obs-extra-audio-mux.exe` | the helper that adds the extra tracks into each MP4 after Stop. It must sit here because it uses OBS's own FFmpeg. |
| `data\obs-plugins\obs-extra-audio\manifest.json` | name and version for **Tools > Plugin Manager** |
| `data\obs-plugins\obs-extra-audio\locale\en-US.ini` | the panel and status text |
| `data\obs-plugins\obs-extra-audio\README.md`, `LICENSE`, `SHA256SUMS.txt` | this guide, the license and the files' checksums |

The helper `.exe` isn't code-signed, so Windows SmartScreen or antivirus may warn about it.

### Go back to an earlier version

Close OBS and extract the earlier version's ZIP the same way. Every version reads the same saved settings.

Before going back, let any recording or clip finish embedding: wait until no `.extra-audio-job.json` file is left next to your videos. An older version can't finish a job file written by a newer one.

### Uninstall

Close OBS, then delete:

- `obs-plugins\64bit\obs-extra-audio.dll`;
- `bin\64bit\obs-extra-audio-mux.exe`;
- the `data\obs-plugins\obs-extra-audio` folder.

Your extra-track settings stay in the OBS profile (the `[ExtraAudio]` section of `basic.ini`) and are simply unused.

## Set up your tracks

1. Open **Tools > Extra Audio Tracks**.
2. In the **Extra tracks** row at the top, tick the tracks you want (7–16). Adding sources never creates tracks by itself.
3. On the **Tracks** tab, for each enabled track:
   - choose its **Audio Bitrate** (new tracks default to 160 kbps);
   - give it a **Name**, which shows as the stream's title in the MP4 (a blank name is labelled "Track 7", and so on);
   - choose where to embed it: **Main recordings / clips**, **Vertical recordings / clips**, or both.
4. Click **Apply and open Advanced Audio Properties**. Each enabled extra track has a column beside native Tracks 1–6. Tick the sources that belong in each extra track. These ticks save immediately. Extra tracks are recording-only: to hear a source on stream as well, also tick its Track 1 (OBS's stream track by default).
5. Keep at least one native Track 1–6 ticked for every source you use in an extra track. OBS only processes a source's audio (filters, volume, sync offset) when it's on a native track.
6. Click **OK** in the panel to save; **Cancel** throws away the panel's unsaved edits.

Live and VOD audio come from native tracks only:

- What's heard live is native **Track 1** (OBS's stream track by default; it's the **Audio Track** setting under **Settings > Output > Streaming**).
- If you use OBS's Twitch VOD track, the VOD gets whichever native track you chose there. Otherwise it gets the same audio as the stream.

Assign sources to those with the native checkboxes in Advanced Audio Properties. A source can be on a native track and an extra track at the same time.

## Settings reference

### Tools > Extra Audio Tracks

| Setting | Where | What it does |
|---|---|---|
| **Extra tracks** 7–16 | top row, above the tabs | Turns each extra track on or off. A disabled track keeps its name, bitrate, destinations and source ticks, and track numbers never shift. A new profile starts with none enabled. |
| Recording audio encoder | Tracks tab (information) | The encoder OBS uses for main recording audio (e.g. CoreAudio AAC). Extra tracks always use this encoder, each at its own bitrate. Change it in OBS Settings. |
| Streaming / VOD mix | Tracks tab (information) | Which native tracks are your live and VOD audio, read from OBS Settings. |
| **Audio Bitrate** | Tracks tab, per track | AAC bitrate for this track. The list is exactly what your encoder offers at OBS's sample rate; CoreAudio at 48 kHz offers 17 values from 32 to 320 kbps. A saved value the encoder doesn't offer shows "(not offered by this encoder)" and must be changed before saving. CoreAudio uses HE-AAC at its lowest bitrates; the plugin measures and corrects HE-AAC's extra delay. |
| **Name** | Tracks tab, per track | The stream title in the MP4, and the `{name}` in MP3 filenames. Optional. |
| **Main recordings / clips** | Tracks tab, per track | Embed this track in main OBS recordings and Replay Buffer clips. |
| **Vertical recordings / clips** | Tracks tab, per track | Embed this track in Aitum vertical recordings and Backtrack clips. |
| **Save each enabled extra track as MP3** | Backup tab | Optional MP3 copies of every enabled extra track; see [MP3 backups](#mp3-backups). Off by default. |
| **Filename format** | Backup tab | Name pattern for the MP3 files. Default `{recording}_Track {track} - {name}`. Below it, the tab shows a live example name and how many MP3s each recording gets. |
| **Status** | below the tabs, always visible | What the plugin is doing now; see [Troubleshooting](#troubleshooting-and-recovery). |
| **Apply and open Advanced Audio Properties** | bottom | Saves, then opens the source matrix. |
| **OK** / **Cancel** | bottom | Save, or discard the panel's edits. |

Track settings, names and source ticks can only be changed while nothing is running: no recording, streaming, Replay Buffer or Backtrack. The panel says "Stop OBS/Aitum outputs before saving track changes" if something is.

### Other places

- **Advanced Audio Properties**: the extra-track columns appear only for enabled tracks. Hovering one shows the track's name and bitrate.
- **Settings > Hotkeys > "Extra Audio: Apply extra-track configuration"**: optional. It reloads the extra-track settings, and its binding is kept across restarts.
- **Tools > Plugin Manager**: enable or disable the plugin; OBS needs a restart. All plugin settings stay in **Tools > Extra Audio Tracks**.

Settings are stored in the OBS profile (the `[ExtraAudio]` section of `basic.ini`), so each profile has its own extra tracks.

## How recording, pausing and clips work

**While recording**:

- The plugin captures each extra track's mix and encodes it with your selected AAC encoder on background threads, only for the destinations you ticked.
- No raw audio is written to disk.
- Each recording has a compressed `.extra-audio.m4a` file beside it.

**After Stop**:

- The helper appends the extra streams and a new index **directly to the original MP4**. The filename stays the same, there's no second copy of the video, and video and native audio are untouched.
- A small `.extra-audio-job.json` checkpoint exists until it finishes. Both side files then disappear; this took under 2 seconds in testing.
- The Status box then says "Complete: extra tracks are embedded in the original recording MP4."

**Pausing**:

- Pause and resume work normally. Main and vertical recordings can pause independently.
- Each recording's extra tracks skip exactly the time its own native audio skips.
- Every test matched the extra tracks to the native audio within one sample (usually 0), at the start, at each pause and at the end.

**Replay Buffer and Backtrack clips**:

- While either replay output runs, the extra tracks are kept compressed in RAM (nothing is written to disk until you save a clip).
- Save clips with your usual buttons or hotkeys.
- The saved MP4 gets the extra tracks for its own time window, including across a pause still inside the window.

**Shared encoders**:

- A recording and the replay output on the same native encoder share one set of extra-track encoders. With your 7 main and 6 vertical tracks, everything running at once uses 13 extra AAC encoders.
- The set stays loaded between recordings.
- It's released when you change extra-track or encoder settings, switch profile, or exit OBS.

**If something fails**:

- The original MP4 is never put at risk. Embedding is journalled and checked before the new index goes live.
- On failure, the original file stays as it was, the `.extra-audio.m4a` recovery audio is kept, and the Status box and log say what happened.

## MP3 backups

With **Save each enabled extra track as MP3** on (Backup tab):

| What | Where | Timing |
|---|---|---|
| Full recordings | `<Recording Path>\Extra Audio\Main\` and `...\Extra Audio\Vertical\` | One MP3 per enabled track, following that recording's own start, stop and pauses. Encoded live from the captured audio. |
| Replay Buffer and Backtrack clips | `<Recording Path>\Extra Audio\` | One MP3 per enabled track, made when the clip is saved, converted from the AAC in RAM. |

**Each recording gets its own set.** When the main and the Aitum vertical canvas both record, you get two sets of MP3s, one in `Main\` and one in `Vertical\`: with 7 enabled tracks, that's 14 files, about 1 GB per hour at 160 kbps. Each set matches its own video exactly: the vertical recording starts a moment later, and either canvas can pause, stop or restart on its own. So each MP3 drops straight onto its own video's timeline in an editor. Without Aitum there is only the main set.

**Filename tokens**:

| Token | Becomes |
|---|---|
| `{track}` | the track number, e.g. `7` |
| `{name}` | the track name, e.g. `Microphone` |
| `{recording}` | the recording's own filename, without `.mp4` |
| `%CCYY-%MM-%DD`, `%hh-%mm-%ss`, … | OBS date and time tokens, taken from the recording's start |

With the default pattern, `2026-10-15_MASTER_07-00-00.mp4` gives `2026-10-15_MASTER_07-00-00_Track 7 - Microphone.mp3`. The recording comes first, so each session's files sort together. A track with no name gives `2026-10-15_MASTER_07-00-00_Track 7.mp3`: the ` - ` next to an empty name is left out. The Backup tab shows an example built the same way as the real files, so you can see the effect of a pattern before you save it.

A profile that saved the older default, `Track {track} - {name}_{recording}`, keeps it. To switch to the new default, clear the field and click OK.

**How files are named and written**:

- Existing files are never overwritten; a clash gets ` (2)`.
- Invalid filename characters are replaced (`Discord/Comms` becomes `Discord_Comms`).
- Folders in the pattern are refused.
- `.mp3` is added for you.

**Other details**:

- MP3s use each track's bitrate, and are kept after the extra tracks are embedded.
- Audio louder than full scale is limited the same way OBS limits its own encoders.
- With backups on, every enabled track is encoded for every recording, even tracks not embedded there.
- With backups off, no MP3 work happens at all.

## Five habits

These avoid every known problem. None of them risks your native recording; at worst a file ends up without its extra tracks.

1. **Keep recording file splitting off.**
   - This covers automatic splitting (**Settings > Output > Recording**, by time or size) and the manual **Split Recording** button or hotkey, plus Aitum's own splitting if it has one.
   - The plugin adds extra tracks to one file per recording and refuses split files.
   - If a recording is split, you keep the original files without extra tracks, plus a recovery audio file to merge by hand.
2. **After Stop, wait a moment before touching the file.**
   - Wait until the `.extra-audio.m4a` and `.extra-audio-job.json` files beside the MP4 disappear, usually under 2 seconds. The Status box then says "Complete".
   - Moving, renaming or opening the MP4 in an editor before then stops the helper from finishing. You'd keep the original MP4 plus the files needed to retry.
   - The same applies to clips.
3. **Stop Aitum's vertical recording before switching scene collections.**
   - Aitum 1.6.6 leaves a running vertical recording stuck when you switch: it ignores Stop and OBS can't exit cleanly.
   - This happens without this plugin too. If it happens anyway, the plugin keeps the original vertical file and its extra audio.
4. **Resume a paused recording before saving a replay.**
   - OBS won't save a Replay Buffer clip while the recording sharing its encoders is paused, and shows "Cannot save replays while paused". Resume, then save.
   - The same applies to Backtrack while the vertical recording is paused. Aitum can still save a vertical clip while only the main recording is paused.
   - The reminder can also appear just from pausing with the Replay Buffer running. It's OBS's own notice; press OK.
5. **Change extra-track settings only when nothing is running.**
   - This covers the panel and the extra-track checkboxes in Advanced Audio Properties.
   - Stop recording, streaming, the Replay Buffer and Backtrack first. The panel enforces this.

## Limitations

- **Tested with OBS 32.2.2; Windows x64 only.** A newer OBS loads the plugin with a "not yet tested" note in Status. Two things can still change with an OBS update:
  - **Pause timing.** To cut pauses at the exact sample, the plugin reads OBS's own pause position, which isn't part of OBS's plugin interface. If a new OBS stores it differently, the plugin notices and places pauses from OBS's video clock instead: usually exact, at most a frame or two off. Status says when that happens. Audio after each pause stays aligned either way.
  - **A new FFmpeg or Qt inside OBS.** The plugin and its helper use OBS's own FFmpeg and Qt files. If an OBS update brings a new major version of either, the plugin can't load until it's rebuilt for that OBS; the OBS log says it failed to load, and your recordings are unaffected.
- **Tested with Aitum Vertical 1.6.6.** A newer Aitum gets vertical extra tracks with a "not yet tested" note in Status; an older one gets none. The plugin finds Aitum's recording and Backtrack by their internal names, so if a future Aitum renames them, vertical outputs simply get no extra tracks until the plugin is updated. Main recordings are unaffected, and a vertical recording keeps its original file if anything fails.
- **Two bugs in Aitum 1.6.6 itself,** both reproduced with this plugin removed:
  - a vertical recording running during a scene collection switch gets stuck (habit 3);
  - its WebSocket "unpause recording" request leaks one object at OBS exit when the recording isn't paused. This matters only to automation, such as a Stream Deck or Streamer.bot, that sends that request.
- **MP4 (including Hybrid MP4) only.** A clip in another format keeps its extra audio in a companion `.extra-audio.m4a` instead of being embedded.
- **Stereo only.** At most 10 extra tracks (7–16) and 32 assigned sources.
- **No recording file splitting** (habit 1).
- **Install it in the OBS folder itself**, not in `ProgramData\obs-studio\plugins`, because the helper is found relative to the plugin.
- **Extra tracks are recording-only.** Live and VOD audio always come from native Tracks 1–6.
- **Audio already mixed together can't be separated.** For example, if music plays through Desktop audio, an extra Music track can't remove it from Desktop. Isolate sources upstream.
- **Browser sources** need OBS's "Control audio via OBS" on to appear in an extra track. Status names any that don't.
- **Sources missing from the current scene collection record silence**, and Status shows how many are missing. A recording that continues across a collection switch records silence on those tracks from the switch.
- **A late start is possible in one case.** The first Aitum recording in an OBS session that starts on its own (not together with the main recording) can start capture up to about 100 ms late; the log says how much. When Aitum starts together with the main recording, this doesn't happen.
- **Not yet validated:**
  - multi-hour sessions;
  - importing into your editor;
  - Twitch's published VODs and clips;
  - recovery after a power cut;
  - OBS versions other than 32.2.2, and Aitum versions other than 1.6.6.

## Resource use

Measured on the development setup.

| | Cost |
|---|---|
| Idle, before the first recording or replay | Same memory and CPU as OBS without the plugin (about 157 vs 156 MB); capture doesn't run. |
| After first use | About 15 MB, 210 handles and 4 threads stay in use, because the extra-track encoders stay loaded for reuse. |
| Per recording after that | About 5 MB growth, the same as OBS on its own (OBS's CoreAudio encoders cause it). |
| While running | Audio history of about 4 MiB per enabled extra track (28 MiB for 7). Replay audio in RAM: about 29 MB per canvas for 7 tracks at 160 kbps with a 180-second replay. Compressed replay audio is capped at 256 MiB in total; past that, replay clips keep only their native tracks and the log says so, while recordings keep their extra tracks. |
| CPU, recording with both replays running | 58% of one core with the plugin, 36% without it, with four main and three vertical extra tracks. |
| Encoders | One per enabled track and canvas: 7 main and 6 vertical tracks use 13 extra AAC encoders with everything running. |
| Disk | About 72 MB per hour per track at 160 kbps, inside the recording. 13 extra streams (7 main, 6 vertical) come to about 936 MB/hour. MP3 backups, if on, add about 72 MB/hour per track per canvas. |

## Troubleshooting and recovery

**The Status box** (Tools > Extra Audio Tracks) is the first place to look. Common messages:

| Message | Meaning |
|---|---|
| Ready: N enabled extra tracks… | Everything is set up. |
| Audio capture is idle until a recording or replay output starts. | Normal when nothing is running. |
| N assigned sources are missing from this scene collection… | Those tracks record silence. Check the source ticks for this collection. |
| Aitum Vertical X isn't supported… | Your Aitum is older than 1.6.6. Vertical outputs get no extra tracks until Aitum is updated. |
| Not yet tested with Aitum Vertical X… | Your Aitum is newer than the version this plugin was tested with. Vertical recording works as usual; if something looks wrong, go back to the tested Aitum or update the plugin. |
| Not yet tested with OBS X… | Your OBS is newer than the version this plugin was tested with. Recording works as usual; if something looks wrong, go back to the tested OBS or update the plugin. |
| …pauses in the extra tracks are placed from OBS's video clock… | This OBS stores its pause position differently, so pauses are placed from the video clock: usually exact, at most a frame or two off. |
| Complete: extra tracks are embedded… | The last recording finished. |
| Extra audio was not added. Original MP4 restored; captured audio retained for retry. | Embedding failed safely; see below. |
| Stop OBS/Aitum outputs before saving track changes. | Habit 5. |

**Logs**: lines starting with `[extra-audio]` in the OBS log (Help > Log Files) say exactly what happened, including timing problems and failed embeds.

**Retrying a failed embed**: keep the MP4 and its `.extra-audio.m4a` and `.extra-audio-job.json` files. With OBS closed, run from OBS's `bin\64bit` folder:

```powershell
.\obs-extra-audio-mux.exe "<path to the .extra-audio-job.json>"
```

That restores the original MP4 if needed and retries. To only restore the original, add `--recover` before the path.

**The plugin doesn't load**:

- Check that **Tools > Plugin Manager** has it enabled.
- Check that OBS is 32.2.2 or newer; the log says if it refused to load.
- After an OBS update, the OBS log may say the plugin failed to load. That means the update brought a new FFmpeg or Qt, and the plugin needs a version built for that OBS.
- Restart OBS after any change.

**Something looks wrong after an update**: go back to the previous version (see [Go back to an earlier version](#go-back-to-an-earlier-version)), then compare.

Licensed under GPL-2.0-or-later; see `LICENSE`. The source and build scripts are published with each release.
