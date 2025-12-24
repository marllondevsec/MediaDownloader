// MediaDownloader_fixed.cpp
// Versão aprimorada: baixa yt-dlp e ffmpeg automaticamente (quando possível)
// Usa --ffmpeg-location internals para permitir merge de formatos.

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

#ifdef _WIN32
    #define EXE_EXT ".exe"
#else
    #define EXE_EXT ""
#endif

// -------------------- Helpers --------------------
static void ensure_dir(const std::string &d) {
    try { fs::create_directories(d); } catch(...) {}
}

static bool file_exists(const std::string &p) {
    return fs::exists(p) && fs::is_regular_file(p);
}

static std::string exec_output(const std::string &cmd) {
    std::string out;
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return "";
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
    pclose(pipe);
    return out;
}

// -------------------- Config (simples) --------------------
struct Config {
    std::string mode = "video";    // video | audio
    std::string quality = "best";  // best | 720 | 1080 ...
};

static Config load_config(const std::string &path) {
    Config c;
    std::ifstream f(path);
    if (!f) return c;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("mode=",0)==0) c.mode = line.substr(5);
        if (line.rfind("quality=",0)==0) c.quality = line.substr(8);
    }
    return c;
}

static void save_config(const std::string &path, const Config &c) {
    std::ofstream f(path);
    if (!f) return;
    f << "mode=" << c.mode << "\n";
    f << "quality=" << c.quality << "\n";
}

// -------------------- Tool downloader --------------------
class ToolInstaller {
public:
    ToolInstaller() {
        ensure_dir("internals");
    }

    std::string yt_dlp_path() const {
        return std::string("internals/yt-dlp") + EXE_EXT;
    }
    std::string ffmpeg_path() const {
        return std::string("internals/ffmpeg") + EXE_EXT;
    }

    bool ensure_yt_dlp() {
        std::string dest = yt_dlp_path();
        if (file_exists(dest)) {
            std::cout << "[OK] yt-dlp already in " << dest << "\n";
            make_executable(dest);
            return true;
        }
        std::cout << "[*] Downloading yt-dlp to " << dest << " ...\n";
#ifdef _WIN32
        std::string cmd = "powershell -Command \"Invoke-WebRequest -Uri 'https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe' -OutFile '" + dest + "'\"";
#else
        std::string cmd = "curl -L -s -o \"" + dest + "\" \"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp\" && chmod +x \"" + dest + "\"";
#endif
        int r = std::system(cmd.c_str());
        if (r == 0 && file_exists(dest)) {
            std::cout << "[OK] yt-dlp downloaded\n";
            make_executable(dest);
            return true;
        }
        std::cerr << "[WARN] Failed to download yt-dlp (cmd exited " << r << ")\n";
        return false;
    }

    bool ensure_ffmpeg() {
        std::string dest = ffmpeg_path();
        if (file_exists(dest)) {
            std::cout << "[OK] ffmpeg already in " << dest << "\n";
            make_executable(dest);
            return true;
        }

        // Select URL based on platform
#ifndef _WIN32
        // Linux x86_64: johnvansickle static build
        // NOTE: if your distro forbids downloading from this site, you can change the URL.
        std::string url = "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz";
        std::string tmp = "/tmp/ffmpeg_dl.tar.xz";
        std::string workdir = "internals/ffmpeg_tmp";
        ensure_dir(workdir);
        std::cout << "[*] Downloading ffmpeg (linux static) ...\n";
        std::string cmd = "curl -L -s -o \"" + tmp + "\" \"" + url + "\" && mkdir -p \"" + workdir + "\" && tar -xJf \"" + tmp + "\" -C \"" + workdir + "\" --strip-components=1 2>/dev/null";
        int r = std::system(cmd.c_str());
        if (r != 0) {
            std::cerr << "[WARN] ffmpeg download/extract failed (curl/tar exit " << r << ")\n";
            try { fs::remove(tmp); fs::remove_all(workdir); } catch(...) {}
            return false;
        }
        // find ffmpeg binary inside workdir
        std::string found;
        for (auto &p : fs::recursive_directory_iterator(workdir)) {
            if (p.path().filename() == "ffmpeg") {
                found = p.path().string();
                break;
            }
        }
        if (found.empty()) {
            std::cerr << "[WARN] ffmpeg binary not found inside archive\n";
            try { fs::remove(tmp); fs::remove_all(workdir); } catch(...) {}
            return false;
        }
        // copy to internals/ffmpeg
        try {
            fs::copy_file(found, dest, fs::copy_options::overwrite_existing);
            make_executable(dest);
            std::cout << "[OK] ffmpeg installed to " << dest << "\n";
            // cleanup
            fs::remove(tmp);
            fs::remove_all(workdir);
            return true;
        } catch (const std::exception &e) {
            std::cerr << "[WARN] copy failed: " << e.what() << "\n";
            try { fs::remove(tmp); fs::remove_all(workdir); } catch(...) {}
            return false;
        }

#else
        // Windows: try to download a minimal build (use gyan's release page suggestion)
        std::cout << "[*] Attempting ffmpeg download for Windows (requires PowerShell)\n";
        // Use gyan builds - user will still need to extract; try to download "ffmpeg-release-essentials.zip"
        std::string url = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";
        std::string tmp = "internals/ffmpeg_release.zip";
        std::string extractDir = "internals/ffmpeg_extract";
        std::string psCmd = "powershell -Command \"";
        psCmd += "Invoke-WebRequest -Uri '" + url + "' -OutFile '" + tmp + "'; ";
        psCmd += "Add-Type -AssemblyName System.IO.Compression.FileSystem; ";
        psCmd += "[System.IO.Compression.ZipFile]::ExtractToDirectory('" + tmp + "', '" + extractDir + "');";
        psCmd += "\"";
        int r = std::system(psCmd.c_str());
        if (r != 0) {
            std::cerr << "[WARN] PowerShell download/extract failed (exit " << r << ")\n";
            return false;
        }
        // search for ffmpeg.exe inside extractDir
        std::string found;
        for (auto &p : fs::recursive_directory_iterator(extractDir)) {
            if (p.path().filename().string() == "ffmpeg.exe") {
                found = p.path().string();
                break;
            }
        }
        if (found.empty()) {
            std::cerr << "[WARN] ffmpeg.exe not found inside extracted zip\n";
            return false;
        }
        try {
            fs::copy_file(found, dest, fs::copy_options::overwrite_existing);
            std::cout << "[OK] ffmpeg copied to " << dest << "\n";
            return true;
        } catch (const std::exception &e) {
            std::cerr << "[WARN] copy failed: " << e.what() << "\n";
            return false;
        }
#endif
    }

private:
    static void make_executable(const std::string &path) {
#ifndef _WIN32
        std::string cmd = "chmod +x \"" + path + "\"";
        std::system(cmd.c_str());
#endif
    }
};

// -------------------- URLs manager --------------------
static std::vector<std::string> load_urls(const std::string &path) {
    std::vector<std::string> v;
    std::ifstream f(path);
    if (!f) return v;
    std::string line;
    while (std::getline(f, line)) {
        if (line.size()==0) continue;
        // trim simple
        while (!line.empty() && isspace((unsigned char)line.front())) line.erase(line.begin());
        while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
        if (line.empty() || line[0]=='#') continue;
        v.push_back(line);
    }
    return v;
}
static bool append_url(const std::string &path, const std::string &url) {
    std::ofstream f(path, std::ios::app);
    if (!f) return false;
    f << url << "\n";
    return true;
}

// -------------------- Download worker --------------------
static void download_list(const std::vector<std::string> &urls, const Config &cfg, const std::string &ytdlp, const std::string &ffmpeg) {
    if (urls.empty()) {
        std::cout << "[!] No URLs\n"; return;
    }
    ensure_dir("downloads");
    for (const auto &u : urls) {
        std::string cmd = "\"" + ytdlp + "\" ";
        // If user requested ffmpeg location, tell yt-dlp where it is
        if (!ffmpeg.empty()) {
            // yt-dlp expects --ffmpeg-location which can be a directory containing ffmpeg
            cmd += "--ffmpeg-location \"internals\" ";
        }
        if (cfg.mode == "audio") {
            cmd += "-x --audio-format mp3 ";
        } else { // video
            // use combined format request
            if (cfg.quality == "best") {
                cmd += "-f \"bestvideo+bestaudio/best\" ";
            } else {
                cmd += "-f \"bestvideo[height<=" + cfg.quality + "]+bestaudio/best[height<=" + cfg.quality + "]\" ";
            }
            // if ffmpeg present and merge desired, yt-dlp will merge automatically
        }
        cmd += "-o \"downloads/%(title)s.%(ext)s\" ";
        cmd += "--no-warnings --ignore-errors --no-playlist ";
        cmd += "\"" + u + "\"";

        std::cout << "\n[*] Running: " << cmd << "\n";
        int r = std::system(cmd.c_str());
        if (r != 0) {
            std::cerr << "[!] yt-dlp exited with code " << r << " for URL: " << u << "\n";
        } else {
            std::cout << "[+] Done: " << u << "\n";
        }
        // tiny pause to avoid hammering
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

// -------------------- Interactive CLI --------------------
static void show_menu() {
    std::cout << "\n=== MediaDownloader ===\n";
    std::cout << "1) Add URL to internals/urls.txt\n";
    std::cout << "2) Show URLs\n";
    std::cout << "3) Settings (mode/quality)\n";
    std::cout << "4) Ensure tools (yt-dlp/ffmpeg)\n";
    std::cout << "5) Start downloads\n";
    std::cout << "0) Exit\n";
    std::cout << "> ";
}

int main(int argc, char** argv) {
    ensure_dir("internals");
    ensure_dir("downloads");

    std::string urlfile = "internals/urls.txt";
    std::string cfgfile = "internals/config.cfg";

    // load config
    Config cfg = load_config(cfgfile);

    ToolInstaller ti;
    // Try to ensure yt-dlp at startup (non-interactive)
    ti.ensure_yt_dlp();

    while (true) {
        show_menu();
        int choice = -1;
        if (!(std::cin >> choice)) {
            std::cin.clear();
            std::string dummy; std::getline(std::cin, dummy);
            continue;
        }
        std::string dummy; std::getline(std::cin, dummy);

        if (choice == 0) break;

        if (choice == 1) {
            std::cout << "Enter URL (or empty to cancel): ";
            std::string u; std::getline(std::cin, u);
            if (!u.empty()) {
                append_url(urlfile, u);
                std::cout << "[+] Added\n";
            }
        } else if (choice == 2) {
            auto urls = load_urls(urlfile);
            if (urls.empty()) { std::cout << "(no urls)\n"; }
            for (size_t i=0;i<urls.size();++i) std::cout << i << ": " << urls[i] << "\n";
        } else if (choice == 3) {
            std::cout << "Current mode: " << cfg.mode << " (video|audio). Change? (enter to keep): ";
            std::string m; std::getline(std::cin, m);
            if (!m.empty()) cfg.mode = m;
            std::cout << "Current quality: " << cfg.quality << " (best|720|1080...). Change? (enter to keep): ";
            std::string q; std::getline(std::cin, q);
            if (!q.empty()) cfg.quality = q;
            save_config(cfgfile, cfg);
            std::cout << "[+] Settings saved\n";
        } else if (choice == 4) {
            std::cout << "[*] Ensuring yt-dlp ...\n";
            bool y = ti.ensure_yt_dlp();
            std::cout << "[*] Ensuring ffmpeg ...\n";
            bool f = ti.ensure_ffmpeg();
            if (!f) std::cout << "[!] ffmpeg not available — video/audio merging will not occur\n";
            else std::cout << "[OK] ffmpeg ready\n";
        } else if (choice == 5) {
            auto urls = load_urls(urlfile);
            if (urls.empty()) { std::cout << "[!] No URLs\n"; continue; }
            std::string ytdlp = ti.yt_dlp_path();
            if (!file_exists(ytdlp)) {
                std::cerr << "[ERR] yt-dlp missing. Run option 4 to install.\n"; continue;
            }
            std::string ff = ti.ffmpeg_path();
            if (!file_exists(ff)) ff.clear();

            std::cout << "[*] Starting downloads (" << urls.size() << ")\n";
            download_list(urls, cfg, ytdlp, ff);
            std::cout << "[✓] All done\n";
        }
    }

    std::cout << "Bye\n";
    return 0;
}
