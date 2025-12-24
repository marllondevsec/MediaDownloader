# StreamHarvester

## Quick Summary

**StreamHarvester** is a single-file **C++17 CLI tool** to manage, download, and optionally convert media (video/audio) using **yt-dlp** and **ffmpeg**.

It organizes downloads into **named URL lists**, automatically installs core tools into `internals/` (when possible), shows **live download progress** parsed from yt-dlp, and **removes items from lists once successfully downloaded**.

Primary goals:
- Simple list-based workflows  
- Repeatable downloads  
- Easy conversion (mp4 / mp3) with minimal setup  

---

## Key Features

- **Named URL lists**  
  Stored in `internals/lists/<listname>.txt` — manage multiple lists and reuse them.

- **Automatic cleanup**  
  URLs are removed from the list when a download completes successfully.

- **Automatic tool bootstrap (best-effort)**  
  Downloads `yt-dlp` and attempts to install `ffmpeg` into `internals/`.

- **Persistent configuration**  
  Stored at `internals/config.cfg` (mode, quality, target format).

- **Progress UI**  
  Parses yt-dlp output to show percentage, ETA, and a running spinner.

- **Cross-platform (Linux, Windows)**  
  ANSI-aware banner and screen clearing with safe fallbacks.

- **Conversion support**  
  - `--recode-video mp4`
  - `-x --audio-format mp3`  
  via yt-dlp + ffmpeg.

---

## Project Layout (generated at runtime)

./StreamHarvester # compiled binary (suggested name)
./MediaDownloader0.3.cpp # source (original file)
downloads/ # final downloaded & converted media
internals/
yt-dlp # yt-dlp executable (or yt-dlp.exe on Windows)
ffmpeg # ffmpeg executable (or ffmpeg.exe on Windows)
config.cfg # persistent settings
lists/
movies.txt # example list file with URLs
podcasts.txt

yaml
Copy code

---

## Build & Requirements

### Requirements

- C++17-capable compiler  
  (`g++`, `clang++`, or MinGW on Windows)
- `curl`, `tar` (Linux) recommended for automatic installers
- On Windows, **PowerShell** available for automatic ffmpeg installer

### Build

#### Linux / macOS / WSL
```bash
g++ -std=c++17 -O2 -Wall MediaDownloader0.3.cpp -o StreamHarvester
Windows (MSYS2 / MinGW)
bash
Copy code
g++ -std=c++17 -O2 -Wall MediaDownloader0.3.cpp -o StreamHarvester.exe
First Run / Startup Behavior
On first execution, StreamHarvester will:

Try to enable ANSI output (for colored banner)

Create:

internals/

internals/lists/

downloads/

Attempt to download yt-dlp into internals/

Attempt a best-effort fetch/extract of a static ffmpeg

Show an ASCII banner and enter the interactive menu

If automatic installs fail, use Menu option 5 — Ensure tools or install binaries manually into internals/.

Usage (Interactive)
Run:

bash
Copy code
./StreamHarvester
Main Menu Options
Manage lists
Create, inspect, or delete named lists.
List names are sanitized into safe filenames.

Add URL to a list
Choose a list and append URL(s).

Show lists and counts
Overview of lists and number of URLs.

Settings

Mode: video or audio

Quality: best, 720, 1080, etc.

Target format: original, mp4, mp3
Settings persist to internals/config.cfg.

Ensure tools
Retry download/install of yt-dlp and ffmpeg.

Start downloads for a list
Downloads sequentially and removes successful URLs automatically.

Example Workflow
Start the program:

bash
Copy code
./StreamHarvester
Create a list:

css
Copy code
Main menu → 1 → n → my_series
Add URLs:

css
Copy code
Main menu → 2 → select my_series → paste URL
Configure:

css
Copy code
Main menu → 4 → choose video/audio, quality, format
Ensure tools (if needed):

css
Copy code
Main menu → 5
Start downloads:

css
Copy code
Main menu → 6 → pick my_series
Successful entries are automatically removed from:

bash
Copy code
internals/lists/my_series.txt
Configuration File
internals/config.cfg

ini
Copy code
mode=video
quality=best
format=original
Options
mode: video | audio

quality: best | numeric (720, 1080) | custom

format: original | mp4 | mp3

Behavior Details & Notes
Merging tracks (video + audio)
Requires ffmpeg. Without it, yt-dlp may download separate tracks.

Target format mp4
Uses --recode-video mp4.
If ffmpeg is missing, falls back to --merge-output-format mp4.

Target format mp3
Uses -x --audio-format mp3. Some conversions require ffmpeg.

Progress parsing
Extracts [download] percentage and ETA from yt-dlp output.
If parsing fails, download still continues normally.

Automatic removal
URLs are removed only when yt-dlp exits with code 0.

Troubleshooting
ffmpeg missing / merge failing
Run Menu → 5 (Ensure tools)

Or install manually:

Linux
bash
Copy code
sudo apt install ffmpeg
Windows
Download a build

Place ffmpeg.exe in internals/ or add to PATH

yt-dlp missing
Linux:

bash
Copy code
curl -L -o internals/yt-dlp \
"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp"
chmod +x internals/yt-dlp
Windows:

Download yt-dlp.exe

Place it in internals/

Progress / ETA not shown
Some sites output different formats.
Download usually still works — progress UI is best-effort.

Legal / Responsible Use
Use StreamHarvester only for media you have rights to download or that providers explicitly permit.
Respect copyright laws and platform Terms of Service (e.g., YouTube TOS).

StreamHarvester does not bypass DRM.

Extensibility & Roadmap
Parallel / multi-threaded downloads

Non-interactive batch mode:

css
Copy code
--list mylist --start
Cookies / authentication integration

Optional logs (internals/logs/)

Improved TUI (ncurses or similar)

Contributing
Fork the repository

Make focused changes

Open a PR with description and tests if possible

Document any new dependency or installer behavior changes.

License
MIT (suggested)
Add a LICENSE file if you adopt the repository.

FAQ
Q: Can I run multiple lists concurrently?
A: Not yet. Downloads are sequential per list.

Q: Can I run it via CLI flags (batch mode)?
A: Not in this release — interactive menu only.

Q: Where are downloaded files saved?
A: ./downloads/
