// MediaDownloader0.3.cpp by Marllondevsec.
// Previous features kept. Added cross-platform clear screen + ASCII banner with startup animation.

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>
#include <regex>
#include <cstdio>      // fileno
#ifndef _WIN32
#include <unistd.h>    // isatty
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
  #define EXE_EXT ".exe"
  #include <windows.h>
  // Some older Windows SDKs might not define this; provide a safe fallback
  #ifndef DISABLE_NEWLINE_AUTO_RETURN
  #define DISABLE_NEWLINE_AUTO_RETURN 0
  #endif
#else
  #define EXE_EXT ""
#endif

// ---------- Terminal utilities ----------
static bool g_ansi_enabled = false;

static void enable_virtual_terminal() {
#ifdef _WIN32
    // Try to enable ANSI escape processing on Windows 10+
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) { g_ansi_enabled = false; return; }
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) { g_ansi_enabled = false; return; }
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(hOut, dwMode)) {
        // fallback: don't use ANSI
        g_ansi_enabled = false;
    } else {
        g_ansi_enabled = true;
    }
#else
    // On POSIX terminals we assume ANSI is ok if stdout is a tty
    g_ansi_enabled = isatty(fileno(stdout));
#endif
}

static void clear_screen() {
    if (g_ansi_enabled) {
        // ANSI clear & move cursor home
        std::cout << "\x1b[2J\x1b[H";
        std::cout.flush();
        return;
    }
    // Fallback: use system commands (less desirable but cross-platform)
#ifdef _WIN32
    std::system("cls");
#else
    std::system("clear");
#endif
}

// ---------- Banner ----------
static const std::vector<std::string> BANNER_LINES = {
"            __",
"           /(`o",
"     ,-,  //  \\\\",
"    (,,,) ||   V",
"   (,,,,)\\//",
"   (,,,/w)-'",
"   \\,,/w)",
"   `V/uu",
"     / |",
"     | |",
"     o o",
"     \\ |",
"\\,/  ,\\|,.  \\,/",
" __                             by marllondevsec",
"(_ _|_ __ _  _ __    |_| _  __    _  _ _|_ _  __",
"__) |_ | (/_(_||||   | |(_| | \\_/(/__>  |_(/_ | "
};


static void print_banner(bool use_color = true) {
    if (g_ansi_enabled && use_color) {
        // simple cyan banner top
        std::cout << "\x1b[1;36m"; // bright cyan
        for (const auto &ln : BANNER_LINES) std::cout << ln << "\n";
        std::cout << "\x1b[0m\n"; // reset
    } else {
        for (const auto &ln : BANNER_LINES) std::cout << ln << "\n";
        std::cout << "\n";
    }
}

static void animate_banner_startup() {
    // small animation printing banner line by line with a tiny delay
    if (!g_ansi_enabled) {
        // no animation if no ANSI (to avoid weird output), just print banner
        print_banner(false);
        return;
    }
    // clear then animate
    clear_screen();
    for (const auto &ln : BANNER_LINES) {
        std::cout << "\x1b[1;36m" << ln << "\x1b[0m" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    // small divider
    std::cout << "\n";
}

// ---------- Helpers ----------
static void ensure_dir(const std::string &d) {
    try { fs::create_directories(d); } catch(...) {}
}
static bool file_exists(const std::string &p) {
    return fs::exists(p) && fs::is_regular_file(p);
}
static std::string trim(const std::string &s) {
    size_t a=0, b=s.size();
    while (a<b && std::isspace((unsigned char)s[a])) ++a;
    while (b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b-a);
}
static int exec_system(const std::string &cmd) {
    return std::system(cmd.c_str());
}
static std::string sanitize_name(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum((unsigned char)c) || c=='_' || c=='-') out.push_back(c);
        else if (std::isspace((unsigned char)c)) out.push_back('_');
    }
    if (out.empty()) out = "list";
    return out;
}

// ---------- Config ----------
struct Config {
    std::string mode = "video";      // "video" or "audio"
    std::string quality = "best";    // "best", "720", "1080", ...
    std::string targetFormat = "original"; // "original", "mp4", "mp3"
};

static Config load_config(const std::string &path) {
    Config c;
    std::ifstream f(path);
    if (!f) return c;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.rfind("mode=",0)==0) c.mode = line.substr(5);
        if (line.rfind("quality=",0)==0) c.quality = line.substr(8);
        if (line.rfind("format=",0)==0) c.targetFormat = line.substr(7);
    }
    return c;
}
static void save_config(const std::string &path, const Config &c) {
    std::ofstream f(path);
    if (!f) return;
    f << "mode=" << c.mode << "\n";
    f << "quality=" << c.quality << "\n";
    f << "format=" << c.targetFormat << "\n";
}

// ---------- Tool Installer (kept) ----------
class ToolInstaller {
public:
    ToolInstaller() { ensure_dir("internals"); }
    std::string yt_dlp_path() const { return std::string("internals/yt-dlp") + EXE_EXT; }
    std::string ffmpeg_path() const { return std::string("internals/ffmpeg") + EXE_EXT; }

    bool ensure_yt_dlp() {
        std::string dest = yt_dlp_path();
        if (file_exists(dest)) { make_executable(dest); std::cout << "[OK] yt-dlp at " << dest << "\n"; return true; }
        std::cout << "[*] Downloading yt-dlp -> " << dest << "\n";
#ifdef _WIN32
        std::string cmd = "powershell -Command \"Invoke-WebRequest -Uri 'https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe' -OutFile '" + dest + "'\"";
#else
        std::string cmd = "curl -L -s -o \"" + dest + "\" \"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp\" && chmod +x \"" + dest + "\"";
#endif
        int r = exec_system(cmd);
        if (r==0 && file_exists(dest)) { make_executable(dest); std::cout << "[OK] yt-dlp installed\n"; return true; }
        std::cerr << "[WARN] yt-dlp download failed (exit " << r << ")\n";
        return false;
    }

    bool ensure_ffmpeg() {
        std::string dest = ffmpeg_path();
        if (file_exists(dest)) { make_executable(dest); std::cout << "[OK] ffmpeg at " << dest << "\n"; return true; }
#ifndef _WIN32
        std::string url = "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz";
        std::string tmp = "/tmp/ffmpeg_dl.tar.xz";
        std::string work = "internals/ffmpeg_tmp";
        ensure_dir(work);
        std::cout << "[*] Downloading static ffmpeg (this may take a bit)...\n";
        std::string cmd = "curl -L -s -o \"" + tmp + "\" \"" + url + "\" && mkdir -p \"" + work + "\" && tar -xJf \"" + tmp + "\" -C \"" + work + "\" --strip-components=1 2>/dev/null";
        int r = exec_system(cmd);
        if (r != 0) {
            try{ fs::remove(tmp); fs::remove_all(work);}catch(...){} 
            std::string alt = "https://github.com/BtbN/FFmpeg-Builds/releases/latest/download/ffmpeg-master-latest-linux64-gpl.tar.xz";
            cmd = "curl -L -s -o \"" + tmp + "\" \"" + alt + "\" && mkdir -p \"" + work + "\" && tar -xJf \"" + tmp + "\" -C \"" + work + "\" --strip-components=1 2>/dev/null";
            r = exec_system(cmd);
            if (r != 0) { try{ fs::remove(tmp); fs::remove_all(work);}catch(...){} return false; }
        }
        std::string found;
        for (auto &p : fs::recursive_directory_iterator(work)) {
            if (p.path().filename()=="ffmpeg") { found = p.path().string(); break; }
        }
        if (found.empty()) { try{ fs::remove(tmp); fs::remove_all(work);}catch(...){} return false; }
        try {
            fs::copy_file(found, dest, fs::copy_options::overwrite_existing);
            make_executable(dest);
            fs::remove(tmp);
            fs::remove_all(work);
            std::cout << "[OK] ffmpeg installed to " << dest << "\n";
            return true;
        } catch (...) { try{ fs::remove(tmp); fs::remove_all(work);}catch(...){} return false; }
#else
        std::string url = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";
        std::string tmp = "internals/ffmpeg_release.zip";
        std::string extract = "internals/ffmpeg_extract";
        std::string ps = "powershell -Command \"Invoke-WebRequest -Uri '" + url + "' -OutFile '" + tmp + "'; Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::ExtractToDirectory('" + tmp + "', '" + extract + "')\"";
        int r = exec_system(ps);
        if (r!=0) return false;
        std::string found;
        for (auto &p : fs::recursive_directory_iterator(extract)) {
            if (p.path().filename()=="ffmpeg.exe") { found = p.path().string(); break; }
        }
        if (found.empty()) return false;
        try { fs::copy_file(found, dest, fs::copy_options::overwrite_existing); std::cout << "[OK] ffmpeg copied to " << dest << "\n"; return true; }
        catch (...) { return false; }
#endif
    }

private:
    static void make_executable(const std::string &path) {
#ifndef _WIN32
        std::string cmd = "chmod +x \"" + path + "\"";
        exec_system(cmd);
#endif
    }
};

// ---------- Lists Manager ----------
static std::string lists_dir() { ensure_dir("internals/lists"); return "internals/lists"; }

static std::vector<std::string> list_names() {
    std::vector<std::string> names;
    ensure_dir(lists_dir());
    for (auto &p : fs::directory_iterator(lists_dir())) {
        if (p.is_regular_file()) {
            std::string n = p.path().filename().string();
            if (n.size() > 4 && n.substr(n.size()-4)==".txt") n = n.substr(0, n.size()-4);
            names.push_back(n);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

static std::string list_path(const std::string &name) { return lists_dir() + "/" + name + ".txt"; }

static std::vector<std::string> load_list(const std::string &name) {
    std::vector<std::string> v; std::ifstream f(list_path(name)); if (!f) return v;
    std::string line;
    while (std::getline(f,line)) { line = trim(line); if (line.empty() || line[0]=='#') continue; v.push_back(line); }
    return v;
}
static bool save_list(const std::string &name, const std::vector<std::string> &v) {
    std::ofstream f(list_path(name)); if (!f) return false; for (auto &s : v) f << s << "\n"; return true;
}
static bool append_to_list(const std::string &name, const std::string &url) {
    std::ofstream f(list_path(name), std::ios::app); if (!f) return false; f << url << "\n"; return true;
}
static bool delete_list(const std::string &name) { try { fs::remove(list_path(name)); return true; } catch(...) { return false; } }

// ---------- Progress executor ----------
static int exec_with_progress(const std::string &cmd) {
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) { std::cerr << "[ERR] failed to run command\n"; return -1; }

    std::string line;
    char buf[512];
    std::regex percentRe(R"(\[download\].*?([0-9]{1,3}(?:\.[0-9])?)%)");
    std::regex etaRe(R"(ETA\s+([0-9]{2}:[0-9]{2}:[0-9]{2}|[0-9]{2}:[0-9]{2}))");
    std::smatch m1, m2;
    std::string lastProgress;
    const char *spinner = "|/-\\";
    int spin = 0;

    auto lastPrint = std::chrono::steady_clock::now();
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        line = buf;
        if (std::regex_search(line, m1, percentRe)) {
            std::string pct = m1[1].str();
            std::string etaStr;
            if (std::regex_search(line, m2, etaRe)) etaStr = m2[1].str();
            lastProgress = "[DOWNLOAD] " + pct + "%";
            if (!etaStr.empty()) lastProgress += " ETA " + etaStr;
            std::cout << "\r" << lastProgress << "    " << std::flush;
            lastPrint = std::chrono::steady_clock::now();
        } else {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() > 300) {
                std::string out = "[RUNNING] ";
                if (!lastProgress.empty()) out = lastProgress + " ";
                std::cout << "\r" << out << spinner[spin % 4] << "    " << std::flush;
                spin++;
                lastPrint = now;
            }
        }
        if (line.rfind("[info]",0)==0 || line.rfind("[ffmpeg]",0)==0 || line.rfind("ERROR",0)==0) {
            std::cout << "\n" << line << std::flush;
        }
    }

    int rc = pclose(pipe);
    std::cout << "\n";
    return rc;
}

// ---------- Build command ----------
static bool build_yt_dlp_cmd(const Config &cfg, const std::string &ytdlp, const std::string &ffmpeg, const std::string &url, std::string &out_cmd) {
    out_cmd.clear();
    out_cmd += "\"" + ytdlp + "\" ";
    if (!ffmpeg.empty()) out_cmd += "--ffmpeg-location \"internals\" ";
    if (cfg.mode == "audio") {
        if (cfg.targetFormat == "mp3") out_cmd += "-x --audio-format mp3 ";
        else out_cmd += "-x ";
    } else {
        if (cfg.quality == "best") out_cmd += "-f \"bestvideo+bestaudio/best\" ";
        else out_cmd += "-f \"bestvideo[height<=" + cfg.quality + "]+bestaudio/best[height<=" + cfg.quality + "]\" ";
        if (cfg.targetFormat == "mp4") {
            if (!ffmpeg.empty()) out_cmd += "--recode-video mp4 ";
            else out_cmd += "--merge-output-format mp4 ";
        }
    }
    out_cmd += "-o \"downloads/%(title)s.%(ext)s\" ";
    out_cmd += "--no-warnings --ignore-errors --no-playlist --restrict-filenames ";
    out_cmd += "\"" + url + "\"";
    return true;
}

// ---------- Download + cleanup ----------
static void download_and_cleanup(const std::string &listname, Config &cfg, ToolInstaller &ti) {
    std::vector<std::string> urls = load_list(listname);
    if (urls.empty()) { std::cout << "[!] List '" << listname << "' is empty\n"; return; }
    std::string ytdlp = ti.yt_dlp_path();
    if (!file_exists(ytdlp)) { std::cerr << "[ERR] yt-dlp missing, run Ensure tools first.\n"; return; }
    std::string ff = ti.ffmpeg_path();
    if (!file_exists(ff)) ff.clear();

    std::cout << "[*] Starting downloads for list '" << listname << "': " << urls.size() << " URLs\n";
    std::vector<std::string> remaining;
    for (size_t i=0;i<urls.size();++i) {
        std::string url = urls[i];
        std::cout << "\n--- (" << (i+1) << "/" << urls.size() << ") " << url << " ---\n";
        std::string cmd;
        build_yt_dlp_cmd(cfg, ytdlp, ff, url, cmd);
        std::cout << "[CMD] " << cmd << "\n";
        int rc = exec_with_progress(cmd);
        if (rc == 0) {
            std::cout << "[OK] Download succeeded, removing from list\n";
        } else {
            std::cerr << "[FAIL] yt-dlp exit " << rc << " -> keeping URL for retry\n";
            remaining.push_back(url);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (!save_list(listname, remaining)) std::cerr << "[WARN] Failed to update list file\n";
    else std::cout << "[INFO] List updated: " << remaining.size() << " URLs remain\n";
}

// ---------- Menus (numeric) ----------
static void show_main_menu() {
    clear_screen();
    print_banner(true);
    std::cout << "\n1) Manage lists (create / choose / delete)\n";
    std::cout << "2) Add URL to a list\n";
    std::cout << "3) Show lists and counts\n";
    std::cout << "4) Settings (mode / quality / format)\n";
    std::cout << "5) Ensure tools (yt-dlp / ffmpeg)\n";
    std::cout << "6) Start downloads for a list\n";
    std::cout << "0) Exit\n";
    std::cout << "> ";
}

// (manage_lists_menu, add_url_flow, show_lists_and_counts implementations follow...)

static void manage_lists_menu() {
    while (true) {
        auto names = list_names();
        std::cout << "\n--- Lists Manager ---\n";
        std::cout << "Existing lists:\n";
        for (size_t i=0;i<names.size();++i) std::cout << "  " << (i+1) << ") " << names[i] << " (" << load_list(names[i]).size() << " urls)\n";
        std::cout << "\nOptions:\n  n) Create new list\n  d) Delete a list\n  b) Back\nChoice: ";
        std::string choice; std::getline(std::cin, choice);
        if (choice=="b"||choice=="B") break;
        if (choice=="n"||choice=="N") {
            std::cout << "New list name: ";
            std::string name; std::getline(std::cin, name); name = trim(name);
            if (name.empty()) { std::cout << "Canceled\n"; continue; }
            std::string sanitized = sanitize_name(name);
            if (fs::exists(list_path(sanitized))) { std::cout << "[!] List already exists\n"; continue; }
            save_list(sanitized, {}); std::cout << "[OK] Created list '" << sanitized << "'\n";
            continue;
        }
        if (choice=="d"||choice=="D") {
            std::cout << "Enter number of list to delete: ";
            std::string num; std::getline(std::cin, num);
            int idx=0; try{ idx = std::stoi(num);}catch(...){ std::cout<<"Invalid\n"; continue; }
            auto names2 = list_names();
            if (idx<1||idx>(int)names2.size()){ std::cout<<"Invalid\n"; continue; }
            std::string name = names2[idx-1];
            std::cout<<"Confirm delete '"<<name<<"' (y/N): ";
            std::string conf; std::getline(std::cin, conf);
            if (!conf.empty()&&(conf[0]=='y'||conf[0]=='Y')) { delete_list(name); std::cout<<"Deleted\n"; }
            continue;
        }
        try {
            int idx = std::stoi(choice);
            auto names2 = list_names();
            if (idx>=1 && idx <= (int)names2.size()) {
                auto urls = load_list(names2[idx-1]);
                std::cout << "\nList '" << names2[idx-1] << "' (" << urls.size() << "):\n";
                for (size_t i=0;i<urls.size();++i) std::cout << "  " << i << ": " << urls[i] << "\n";
                std::cout << "Press Enter..."; std::string tmp; std::getline(std::cin, tmp);
            } else std::cout<<"Invalid selection\n";
        } catch(...) { std::cout << "Unknown option\n"; }
    }
}

static void add_url_flow() {
    auto names = list_names();
    std::cout << "\nChoose list to add URL:\n";
    for (size_t i=0;i<names.size();++i) std::cout << "  " << (i+1) << ") " << names[i] << "\n";
    std::cout << "  0) Create new list\nChoice (number): ";
    std::string ch; std::getline(std::cin, ch);
    int sel=-1; try{ sel = std::stoi(ch);}catch(...){ sel=-1;}
    std::string targetList;
    if (sel==0) {
        std::cout<<"New list name: "; std::string name; std::getline(std::cin,name); name=trim(name);
        if (name.empty()) { std::cout<<"Canceled\n"; return; }
        targetList = sanitize_name(name); save_list(targetList, {}); std::cout<<"Created '"<<targetList<<"'\n";
    } else if (sel>=1 && sel <= (int)names.size()) targetList = names[sel-1];
    else { std::cout<<"Invalid\n"; return; }
    std::cout<<"Enter URL: "; std::string url; std::getline(std::cin,url); url=trim(url);
    if (url.empty()) { std::cout<<"Canceled\n"; return; }
    if (append_to_list(targetList, url)) std::cout << "[OK]\n"; else std::cout << "[ERR]\n";
}

static void show_lists_and_counts() {
    auto names = list_names();
    if (names.empty()) { std::cout << "(no lists)\n"; return; }
    std::cout << "\nLists:\n";
    for (size_t i=0;i<names.size();++i) std::cout << " " << (i+1) << ") " << names[i] << " - " << load_list(names[i]).size() << " URLs\n";
}

// ---------- Main ----------
int main() {
    enable_virtual_terminal();          // try to activate ANSI on windows
    ensure_dir("internals");
    ensure_dir("internals/lists");
    ensure_dir("downloads");

    std::string cfgfile = "internals/config.cfg";
    Config cfg = load_config(cfgfile);
    ToolInstaller ti;

    // startup animation (only once)
    animate_banner_startup();

    std::cout << "[STARTUP] Ensuring core tools (yt-dlp + ffmpeg) are present...\n";
    bool y_ok = ti.ensure_yt_dlp();
    bool f_ok = ti.ensure_ffmpeg();
    std::cout << "[STARTUP] yt-dlp: " << (y_ok ? "ok" : "missing") << " ; ffmpeg: " << (f_ok ? "ok" : "missing") << "\n";

    while (true) {
        show_main_menu();
        std::string choice; std::getline(std::cin, choice);
        choice = trim(choice);
        if (choice.empty()) continue;
        if (choice == "0") break;
        if (choice == "1") { manage_lists_menu(); continue; }
        if (choice == "2") { add_url_flow(); continue; }
        if (choice == "3") { show_lists_and_counts(); continue; }
        if (choice == "4") {
            // numeric settings menu
            clear_screen(); print_banner(true);
            std::cout << "\nMode (1=video, 2=audio). Current: " << cfg.mode << "\nChoice: ";
            std::string m; std::getline(std::cin,m); m = trim(m);
            if (!m.empty()) { if (m=="1") cfg.mode="video"; else if (m=="2") cfg.mode="audio"; }
            std::cout << "Quality options:\n 1) best\n 2) 720\n 3) 1080\n 4) 1440\n 5) 2160\n 6) custom\nCurrent: " << cfg.quality << "\nChoice: ";
            std::string q; std::getline(std::cin,q); q = trim(q);
            if (!q.empty()) {
                if (q=="1") cfg.quality="best";
                else if (q=="2") cfg.quality="720";
                else if (q=="3") cfg.quality="1080";
                else if (q=="4") cfg.quality="1440";
                else if (q=="5") cfg.quality="2160";
                else if (q=="6") { std::cout<<"Enter custom quality (e.g. 480): "; std::string c; std::getline(std::cin,c); if (!trim(c).empty()) cfg.quality = trim(c); }
            }
            std::cout << "Target format (1 original, 2 mp4, 3 mp3). Current: " << cfg.targetFormat << "\nChoice: ";
            std::string f; std::getline(std::cin,f); f = trim(f);
            if (!f.empty()) {
                if (f=="1") cfg.targetFormat="original";
                else if (f=="2") cfg.targetFormat="mp4";
                else if (f=="3") cfg.targetFormat="mp3";
            }
            save_config(cfgfile, cfg);
            std::cout << "[OK] Settings saved\n";
            continue;
        }
        if (choice == "5") {
            std::cout << "[*] Ensuring yt-dlp... "; bool y = ti.ensure_yt_dlp(); std::cout << (y?"OK":"FAILED") << "\n";
            std::cout << "[*] Ensuring ffmpeg... "; bool f = ti.ensure_ffmpeg(); std::cout << (f?"OK":"(not installed)") << "\n";
            if (!f) std::cout << "[WARN] ffmpeg not available: conversions requiring ffmpeg may fail\n";
            continue;
        }
        if (choice == "6") {
            auto names = list_names();
            if (names.empty()) { std::cout << "[!] No lists available.\n"; continue; }
            std::cout << "Select list to download:\n";
            for (size_t i=0;i<names.size();++i) std::cout << "  " << (i+1) << ") " << names[i] << " (" << load_list(names[i]).size() << " urls)\n";
            std::cout << "Choice number: ";
            std::string c; std::getline(std::cin,c);
            int idx = -1; try { idx = std::stoi(trim(c)); } catch(...) { idx = -1; }
            if (idx < 1 || idx > (int)names.size()) { std::cout << "Invalid\n"; continue; }
            std::string listname = names[idx-1];
            download_and_cleanup(listname, cfg, ti);
            continue;
        }
        std::cout << "Unknown option\n";
    }

    std::cout << "Goodbye\n";
    return 0;
}
