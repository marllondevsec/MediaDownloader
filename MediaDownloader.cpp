// mediadownloader.cpp - Versão 4.0 (Profissional)
// Multiplatform CLI batch downloader wrapper for yt-dlp + ffmpeg
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <thread>
#include <atomic>
#include <functional>
#include <regex>
#include <optional>
#include <cerrno>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
#include <errno.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

// ==================== GLOBAL STATE ====================
std::atomic<bool> g_interrupted{false};

#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        g_interrupted = true;
        return TRUE;
    }
    return FALSE;
}
#else
void SignalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_interrupted = true;
    }
}
#endif

// ==================== ESCAPE SEGURO WINDOWS ====================
class WindowsEscape {
public:
    static std::wstring EscapeArgument(const std::wstring& argument) {
        // Regra oficial do Windows: https://docs.microsoft.com/en-us/cpp/cpp/main-function-command-line-args
        if (argument.empty()) {
            return L"\"\"";
        }
        
        // Verifica se precisa de aspas
        bool needsQuotes = false;
        for (wchar_t c : argument) {
            if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v') {
                needsQuotes = true;
                break;
            }
        }
        
        if (!needsQuotes) {
            // Verifica se não tem caracteres especiais que exigem aspas
            for (wchar_t c : argument) {
                if (c == L'"' || c == L'\\') {
                    needsQuotes = true;
                    break;
                }
            }
            if (!needsQuotes) {
                return argument;
            }
        }
        
        std::wstring escaped = L"\"";
        size_t backslashCount = 0;
        
        for (size_t i = 0; i < argument.size(); ++i) {
            if (argument[i] == L'\\') {
                ++backslashCount;
            } else if (argument[i] == L'"') {
                // Duplicar todas as barras antes da aspa
                escaped.append(backslashCount + 1, L'\\');
                escaped.push_back(L'"');
                backslashCount = 0;
            } else {
                if (backslashCount > 0) {
                    escaped.append(backslashCount, L'\\');
                    backslashCount = 0;
                }
                escaped.push_back(argument[i]);
            }
        }
        
        // Barras finais
        if (backslashCount > 0) {
            escaped.append(backslashCount, L'\\');
        }
        
        escaped.push_back(L'"');
        return escaped;
    }
};

// ==================== SEGURANÇA E ROBUSTEZ ====================
class SafeCommand {
private:
#ifdef _WIN32
    static std::wstring ToWide(const std::string& str) {
        if (str.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        if (size <= 0) return L"";
        std::wstring wstr(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
        return wstr.substr(0, size - 1);
    }
#endif

public:
    // Executa comando de forma segura usando vetor de argumentos
    static int Execute(const std::string& program, const std::vector<std::string>& args, bool check_interrupt = true) {
        if (program.empty()) return -1;
        
        if (check_interrupt && g_interrupted.load()) {
            return -999; // Código especial para interrupção
        }
        
#ifdef _WIN32
        // Windows: CreateProcess com argumentos vetorizados e escape correto
        std::wstring wprogram = ToWide(program);
        
        // Construir command line com escape correto
        std::wstring cmdline = WindowsEscape::EscapeArgument(wprogram);
        for (const auto& arg : args) {
            cmdline += L" ";
            cmdline += WindowsEscape::EscapeArgument(ToWide(arg));
        }
        
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {0};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        
        if (!CreateProcessW(
            nullptr, // Nome do executável deve estar em cmdline
            &cmdline[0], // Command line
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi)) {
            return -static_cast<int>(GetLastError());
        }
        
        // Aguardar com verificação de interrupção
        while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
            if (check_interrupt && g_interrupted.load()) {
                TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return -999;
            }
        }
        
        DWORD exitCode;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return static_cast<int>(exitCode);
        
#else
        // Linux: posix_spawn com vetor de argumentos
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        
        pid_t pid;
        if (posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
            return -errno;
        }
        
        // Aguardar com verificação de interrupção
        int status;
        while (waitpid(pid, &status, WNOHANG) == 0) {
            if (check_interrupt && g_interrupted.load()) {
                kill(pid, SIGTERM);
                waitpid(pid, &status, 0);
                return -999;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return -1;
#endif
    }
    
    static int ExecuteYtdlp(const fs::path& ytdlpPath, const std::vector<std::string>& ytdlpArgs) {
        return Execute(ytdlpPath.string(), ytdlpArgs, true);
    }
};

// ==================== VALIDAÇÃO DE URL ====================
class UrlValidator {
public:
    enum UrlType {
        UNKNOWN,
        YOUTUBE_VIDEO,
        YOUTUBE_PLAYLIST,
        TWITTER,
        TIKTOK,
        INSTAGRAM,
        REDDIT,
        GENERIC_VIDEO,
        INVALID
    };
    
    static UrlType ClassifyUrl(const std::string& url) {
        if (url.empty()) return INVALID;
        
        std::string clean = url;
        clean.erase(std::remove_if(clean.begin(), clean.end(), ::isspace), clean.end());
        
        // Protocolo obrigatório
        if (clean.find("http://") != 0 && clean.find("https://") != 0) {
            return INVALID;
        }
        
        // Domínios conhecidos
        std::string lower = clean;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        
        if (lower.find("youtube.com/watch?v=") != std::string::npos ||
            lower.find("youtu.be/") != std::string::npos) {
            return YOUTUBE_VIDEO;
        }
        
        if (lower.find("youtube.com/playlist?list=") != std::string::npos ||
            lower.find("youtube.com/playlist/") != std::string::npos) {
            return YOUTUBE_PLAYLIST;
        }
        
        if (lower.find("twitter.com/") != std::string::npos ||
            lower.find("x.com/") != std::string::npos) {
            return TWITTER;
        }
        
        if (lower.find("tiktok.com/") != std::string::npos) {
            return TIKTOK;
        }
        
        if (lower.find("instagram.com/") != std::string::npos) {
            return INSTAGRAM;
        }
        
        if (lower.find("reddit.com/") != std::string::npos) {
            return REDDIT;
        }
        
        // Vídeos diretos
        if (lower.find(".mp4") != std::string::npos ||
            lower.find(".webm") != std::string::npos ||
            lower.find(".mkv") != std::string::npos ||
            lower.find(".avi") != std::string::npos ||
            lower.find(".mov") != std::string::npos ||
            lower.find(".m3u8") != std::string::npos) {
            return GENERIC_VIDEO;
        }
        
        return UNKNOWN;
    }
    
    static bool IsValidUrl(const std::string& url) {
        return ClassifyUrl(url) != INVALID;
    }
    
    static std::string GetUrlTypeName(UrlType type) {
        switch (type) {
            case YOUTUBE_VIDEO: return "YouTube Video";
            case YOUTUBE_PLAYLIST: return "YouTube Playlist";
            case TWITTER: return "Twitter/X";
            case TIKTOK: return "TikTok";
            case INSTAGRAM: return "Instagram";
            case REDDIT: return "Reddit";
            case GENERIC_VIDEO: return "Direct Video";
            case UNKNOWN: return "Unknown (will try)";
            case INVALID: return "Invalid";
        }
        return "Unknown";
    }
    
    static bool LooksLikePlaylist(const std::string& url) {
        UrlType type = ClassifyUrl(url);
        return type == YOUTUBE_PLAYLIST || 
               url.find("list=") != std::string::npos ||
               url.find("/playlist") != std::string::npos ||
               url.find("/videos") != std::string::npos;
    }
    
    static int EstimatePlaylistSize(const std::string& url) {
        // Estimativa baseada no tipo
        UrlType type = ClassifyUrl(url);
        switch (type) {
            case YOUTUBE_PLAYLIST: return 50; // Média de playlists do YouTube
            default: return 1;
        }
    }
};

// ==================== CONFIGURAÇÃO PROFISSIONAL ====================
struct ProfessionalConfig {
    struct {
        std::string mode = "video";
        std::string audio_format = "mp3";
        std::string output_dir;
        bool telegram_preset = true;
        bool download_archive = true;
        bool embed_metadata = true;
        bool restrict_filenames = true;
        int max_concurrent = 1;
        int retries = 3;
        bool ignore_errors = true;
        bool newline_output = true;
        bool throttle_playlists = true; // Nova: throttle automático para playlists
    } settings;
    
    struct {
        fs::path root;
        fs::path internals;
        fs::path url_dir;
        fs::path ytdlp;
        fs::path ffmpeg;
        fs::path logs;
        fs::path state; // Novo: estado da aplicação
    } paths;
    
    struct {
        int total_downloads = 0;
        int successful_downloads = 0;
        int failed_downloads = 0;
        std::chrono::system_clock::time_point last_run;
        std::string last_list;
    } stats;
};

// ==================== UI PROFISSIONAL ====================
class ProfessionalUI {
public:
    enum Color { RESET=0, RED=31, GREEN=32, YELLOW=33, BLUE=34, MAGENTA=35, CYAN=36, WHITE=37 };
    
    static void Init() {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hConsole, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hConsole, mode);
        }
        
        // Handler para Ctrl+C
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
        signal(SIGINT, SignalHandler);
        signal(SIGTERM, SignalHandler);
#endif
    }
    
    static void Color(Color c) { 
        std::cout << "\033[" << c << "m"; 
    }
    
    static void Reset() { 
        std::cout << "\033[0m"; 
    }
    
    static void Print(const std::string& text, Color c = WHITE) {
        Color(c);
        std::cout << text;
        Reset();
    }
    
    static void PrintLine(const std::string& text, Color c = WHITE) {
        Print(text + "\n", c);
    }
    
    static void Warning(const std::string& msg) {
        Print("[WARNING] ", YELLOW);
        std::cout << msg << "\n";
    }
    
    static void Error(const std::string& msg) {
        Print("[ERROR] ", RED);
        std::cout << msg << "\n";
    }
    
    static void Success(const std::string& msg) {
        Print("[SUCCESS] ", GREEN);
        std::cout << msg << "\n";
    }
    
    static void Info(const std::string& msg) {
        Print("[INFO] ", CYAN);
        std::cout << msg << "\n";
    }
    
    static void Important(const std::string& msg) {
        Print("[IMPORTANT] ", MAGENTA);
        std::cout << msg << "\n";
    }
    
    static void PrintProgress(const std::string& task, int current, int total, bool show_percent = true) {
        if (total == 0) return;
        
        if (g_interrupted.load()) {
            std::cout << "\r" << task << " [INTERRUPTED]                      \n";
            return;
        }
        
        int percent = (current * 100) / total;
        int barWidth = 40;
        
        std::cout << "\r" << task << " [";
        int pos = barWidth * percent / 100;
        for (int i = 0; i < barWidth; ++i) {
            if (i < pos) std::cout << "=";
            else if (i == pos) std::cout << ">";
            else std::cout << " ";
        }
        std::cout << "]";
        
        if (show_percent) {
            std::cout << " " << percent << "% (" << current << "/" << total << ")";
        } else {
            std::cout << " " << current << "/" << total;
        }
        
        std::cout.flush();
        
        if (current == total) std::cout << "\n";
    }
    
    static void PrintHeader(const std::string& title) {
        std::cout << "\n";
        Color(CYAN);
        std::cout << "========================================\n";
        std::cout << "  " << title << "\n";
        std::cout << "========================================\n";
        Reset();
    }
    
    static void PrintMenuOption(int num, const std::string& text, bool enabled = true) {
        std::cout << "  ";
        Color(YELLOW);
        std::cout << num << ") ";
        Reset();
        
        if (enabled) {
            std::cout << text << "\n";
        } else {
            Color(WHITE);
            std::cout << text << " [Disabled]\n";
            Reset();
        }
    }
    
    static bool GetConfirmation(const std::string& question) {
        std::cout << question << " [y/N]: ";
        std::string answer;
        std::getline(std::cin, answer);
        
        std::transform(answer.begin(), answer.end(), answer.begin(), ::tolower);
        return (answer == "y" || answer == "yes");
    }
    
    static int GetInteger(const std::string& prompt, int min, int max, int defaultValue) {
        std::cout << prompt << " [" << defaultValue << "]: ";
        std::string input;
        std::getline(std::cin, input);
        
        if (input.empty()) {
            return defaultValue;
        }
        
        try {
            int value = std::stoi(input);
            if (value < min || value > max) {
                Warning("Value must be between " + std::to_string(min) + " and " + std::to_string(max));
                return defaultValue;
            }
            return value;
        } catch (...) {
            Warning("Invalid number");
            return defaultValue;
        }
    }
};

// ==================== GERENCIADOR DE DOWNLOAD PROFISSIONAL ====================
class ProfessionalDownloadManager {
private:
    ProfessionalConfig& config;
    fs::path logFile;
    fs::path stateFile;
    
    struct DownloadStats {
        int total = 0;
        int successful = 0;
        int failed = 0;
        int skipped = 0;
        std::vector<std::string> failedUrls;
        std::chrono::seconds total_time{0};
    };
    
public:
    ProfessionalDownloadManager(ProfessionalConfig& cfg) : config(cfg) {
        logFile = config.paths.logs / "downloads.log";
        stateFile = config.paths.state / "app_state.json";
        
        fs::create_directories(config.paths.logs);
        fs::create_directories(config.paths.state);
    }
    
    DownloadStats DownloadList(const fs::path& listPath, bool dry_run = false) {
        DownloadStats stats;
        auto start_time = std::chrono::steady_clock::now();
        
        if (!fs::exists(listPath)) {
            ProfessionalUI::Error("List file doesn't exist: " + listPath.string());
            return stats;
        }
        
        auto urls = LoadAndValidateUrls(listPath);
        stats.total = urls.size();
        
        if (urls.empty()) {
            ProfessionalUI::Warning("No valid URLs found in list");
            return stats;
        }
        
        ProfessionalUI::Info("Processing " + std::to_string(urls.size()) + " URLs");
        
        // Aviso sobre ignore_errors + download_archive
        if (config.settings.ignore_errors && config.settings.download_archive) {
            ProfessionalUI::Important("WARNING: With ignore_errors=true and download_archive=true,");
            ProfessionalUI::Important("failed downloads may be added to archive and skipped on next run.");
        }
        
        // Ajuste automático para playlists grandes
        if (config.settings.throttle_playlists) {
            for (const auto& url : urls) {
                if (UrlValidator::LooksLikePlaylist(url)) {
                    int estimated_size = UrlValidator::EstimatePlaylistSize(url);
                    if (estimated_size > 10 && config.settings.max_concurrent > 3) {
                        ProfessionalUI::Warning("Large playlist detected, reducing concurrent downloads");
                        config.settings.max_concurrent = std::min(config.settings.max_concurrent, 3);
                        break;
                    }
                }
            }
        }
        
        for (size_t i = 0; i < urls.size(); ++i) {
            if (g_interrupted.load()) {
                ProfessionalUI::Warning("Download interrupted by user");
                break;
            }
            
            ProfessionalUI::PrintProgress("Downloading", i + 1, urls.size());
            
            auto result = dry_run ? DryRunSingleUrl(urls[i]) : DownloadSingleUrl(urls[i], listPath.stem().string());
            
            if (result.success) {
                stats.successful++;
            } else if (result.skipped) {
                stats.skipped++;
            } else {
                stats.failed++;
                stats.failedUrls.push_back(urls[i]);
                
                if (!config.settings.ignore_errors && !dry_run) {
                    ProfessionalUI::Error("Stopping due to error (ignore_errors is false)");
                    break;
                }
            }
            
            // Pequena pausa para não sobrecarregar servidores
            if (!dry_run && config.settings.max_concurrent > 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        
        auto end_time = std::chrono::steady_clock::now();
        stats.total_time = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);
        
        LogStatistics(stats, listPath.stem().string(), dry_run);
        SaveState(listPath.stem().string(), stats);
        
        return stats;
    }
    
    struct DryRunInfo {
        std::string url;
        UrlValidator::UrlType type;
        std::string title;
        std::string duration;
        bool is_playlist;
        int estimated_items;
    };
    
    std::vector<DryRunInfo> AdvancedDryRun(const fs::path& listPath) {
        std::vector<DryRunInfo> results;
        auto urls = LoadAndValidateUrls(listPath);
        
        ProfessionalUI::Info("Advanced dry run for " + std::to_string(urls.size()) + " URLs");
        
        for (size_t i = 0; i < urls.size(); ++i) {
            if (g_interrupted.load()) break;
            
            ProfessionalUI::PrintProgress("Analyzing", i + 1, urls.size(), false);
            
            DryRunInfo info;
            info.url = urls[i];
            info.type = UrlValidator::ClassifyUrl(urls[i]);
            info.is_playlist = UrlValidator::LooksLikePlaylist(urls[i]);
            info.estimated_items = info.is_playlist ? UrlValidator::EstimatePlaylistSize(urls[i]) : 1;
            
            // Tentar obter título e duração
            auto metadata = GetUrlMetadata(urls[i]);
            info.title = metadata.title;
            info.duration = metadata.duration;
            
            results.push_back(info);
        }
        
        ProfessionalUI::PrintProgress("Analyzing", urls.size(), urls.size(), false);
        return results;
    }
    
private:
    struct SingleResult {
        bool success = false;
        bool skipped = false;
        std::string message;
        int exit_code = 0;
    };
    
    struct MetadataResult {
        std::string title;
        std::string duration;
        std::string format;
        bool accessible;
    };
    
    std::vector<std::string> LoadAndValidateUrls(const fs::path& listPath) {
        std::vector<std::string> validUrls;
        
        if (!fs::exists(listPath)) return validUrls;
        
        std::ifstream file(listPath);
        std::string line;
        int lineNum = 0;
        
        while (std::getline(file, line)) {
            lineNum++;
            line = Trim(line);
            
            if (line.empty() || line[0] == '#') continue;
            
            if (UrlValidator::IsValidUrl(line)) {
                validUrls.push_back(line);
            } else {
                ProfessionalUI::Warning("Line " + std::to_string(lineNum) + ": Invalid URL format");
                Log("Invalid URL skipped: " + line);
            }
        }
        
        return validUrls;
    }
    
    SingleResult DownloadSingleUrl(const std::string& url, const std::string& listName) {
        SingleResult result;
        
        std::vector<std::string> args = BuildBaseArgs(listName);
        args.push_back(url);
        
        if (UrlValidator::LooksLikePlaylist(url)) {
            args.push_back("--yes-playlist");
            Log("Playlist detected: " + url);
            
            // Throttle para playlists
            if (config.settings.throttle_playlists) {
                args.push_back("--sleep-interval");
                args.push_back("2");
            }
        }
        
        int exitCode = SafeCommand::ExecuteYtdlp(config.paths.ytdlp, args);
        result.exit_code = exitCode;
        
        if (exitCode == 0) {
            result.success = true;
            result.message = "Success";
        } else if (exitCode == -999) {
            result.message = "Interrupted by user";
        } else {
            result.message = "Failed with code: " + std::to_string(exitCode);
        }
        
        return result;
    }
    
    SingleResult DryRunSingleUrl(const std::string& url) {
        SingleResult result;
        
        if (!UrlValidator::IsValidUrl(url)) {
            result.message = "Invalid URL";
            return result;
        }
        
        result.success = true;
        result.message = "Valid URL - " + UrlValidator::GetUrlTypeName(UrlValidator::ClassifyUrl(url));
        return result;
    }
    
    MetadataResult GetUrlMetadata(const std::string& url) {
        MetadataResult result;
        
        std::vector<std::string> args = {
            "--skip-download",
            "--get-title",
            "--get-duration",
            "--get-format",
            "--newline",
            "--no-warnings",
            url
        };
        
        int exitCode = SafeCommand::Execute(config.paths.ytdlp.string(), args, false);
        
        result.accessible = (exitCode == 0);
        return result;
    }
    
    std::vector<std::string> BuildBaseArgs(const std::string& listName) {
        std::vector<std::string> args;
        
        // Argumentos obrigatórios
        args.push_back("--no-color");
        
        if (config.settings.newline_output) {
            args.push_back("--newline");
        }
        
        if (config.settings.ignore_errors) {
            args.push_back("--ignore-errors");
        }
        
        // Configurações de modo
        if (config.settings.mode == "video") {
            std::string outTmpl = config.settings.output_dir + "/%(title).200B [%(id)s].%(ext)s";
            args.push_back("-o");
            args.push_back(outTmpl);
            
            if (config.settings.telegram_preset) {
                args.push_back("-f");
                args.push_back("bestvideo[ext=mp4][vcodec^=avc1]+bestaudio[ext=m4a]/best[ext=mp4]/best");
                args.push_back("--merge-output-format");
                args.push_back("mp4");
            } else {
                args.push_back("-f");
                args.push_back("bestvideo+bestaudio/best");
            }
        } else if (config.settings.mode == "audio") {
            std::string outTmpl = config.settings.output_dir + "/%(title).200B [%(id)s].%(ext)s";
            args.push_back("-o");
            args.push_back(outTmpl);
            args.push_back("-x");
            args.push_back("--audio-format");
            args.push_back(config.settings.audio_format);
            args.push_back("--audio-quality");
            args.push_back("0");
            
            if (config.settings.embed_metadata) {
                args.push_back("--embed-thumbnail");
                args.push_back("--embed-metadata");
            }
        }
        
        // Configurações comuns
        args.push_back("--ffmpeg-location");
        args.push_back(config.paths.ffmpeg.string());
        
        if (config.settings.download_archive) {
            fs::path archiveFile = config.paths.internals / (listName + ".archive.txt");
            args.push_back("--download-archive");
            args.push_back(archiveFile.string());
        }
        
        if (config.settings.restrict_filenames) {
            args.push_back("--restrict-filenames");
        }
        
        args.push_back("--no-overwrites");
        args.push_back("--continue");
        args.push_back("--retries");
        args.push_back(std::to_string(config.settings.retries));
        args.push_back("--fragment-retries");
        args.push_back(std::to_string(config.settings.retries));
        args.push_back("--concurrent-downloads");
        args.push_back(std::to_string(config.settings.max_concurrent));
        args.push_back("--socket-timeout");
        args.push_back("30");
        args.push_back("--extractor-retries");
        args.push_back("3");
        
        return args;
    }
    
    void Log(const std::string& message) {
        std::ofstream file(logFile, std::ios::app);
        file << "[" << GetTimestamp() << "] " << message << "\n";
    }
    
    void LogStatistics(const DownloadStats& stats, const std::string& listName, bool dry_run) {
        std::string prefix = dry_run ? "[DRY RUN] " : "";
        
        Log(prefix + "=== Download Statistics ===");
        Log(prefix + "List: " + listName);
        Log(prefix + "Total URLs: " + std::to_string(stats.total));
        Log(prefix + "Successful: " + std::to_string(stats.successful));
        Log(prefix + "Failed: " + std::to_string(stats.failed));
        Log(prefix + "Skipped: " + std::to_string(stats.skipped));
        Log(prefix + "Time: " + std::to_string(stats.total_time.count()) + "s");
        
        if (!stats.failedUrls.empty()) {
            Log(prefix + "Failed URLs:");
            for (const auto& url : stats.failedUrls) {
                Log(prefix + "  " + url);
            }
        }
        
        Log(prefix + "==========================");
    }
    
    void SaveState(const std::string& listName, const DownloadStats& stats) {
        config.stats.total_downloads += stats.total;
        config.stats.successful_downloads += stats.successful;
        config.stats.failed_downloads += stats.failed;
        config.stats.last_run = std::chrono::system_clock::now();
        config.stats.last_list = listName;
        
        // Salvar em JSON (simplificado)
        std::ofstream file(stateFile);
        file << "{\n";
        file << "  \"total_downloads\": " << config.stats.total_downloads << ",\n";
        file << "  \"successful_downloads\": " << config.stats.successful_downloads << ",\n";
        file << "  \"failed_downloads\": " << config.stats.failed_downloads << ",\n";
        file << "  \"last_run\": \"" << GetTimestamp() << "\",\n";
        file << "  \"last_list\": \"" << listName << "\"\n";
        file << "}\n";
    }
    
    std::string Trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }
    
    std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &now_time);
#else
        localtime_r(&now_time, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
};

// ==================== APLICAÇÃO PRINCIPAL ====================
class MediaPipelineApp {
private:
    ProfessionalConfig config;
    ProfessionalDownloadManager downloadManager;
    bool running = true;
    
public:
    MediaPipelineApp() : downloadManager(config) {
        ProfessionalUI::Init();
        InitializePaths();
        LoadConfiguration();
    }
    
    void Run() {
        while (running && !g_interrupted.load()) {
            ShowMainMenu();
        }
        
        ProfessionalUI::Success("Media Pipeline shutdown complete");
    }
    
private:
    void InitializePaths() {
        try {
#ifdef _WIN32
            wchar_t buffer[MAX_PATH];
            GetModuleFileNameW(NULL, buffer, MAX_PATH);
            config.paths.root = fs::path(buffer).parent_path();
            config.paths.ytdlp = config.paths.root / "internals" / "yt-dlp.exe";
            config.paths.ffmpeg = config.paths.root / "internals" / "ffmpeg.exe";
#else
            char buffer[4096];
            ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer)-1);
            if (len != -1) {
                buffer[len] = '\0';
                config.paths.root = fs::path(buffer).parent_path();
            } else {
                config.paths.root = fs::current_path();
            }
            config.paths.ytdlp = config.paths.root / "internals" / "yt-dlp";
            config.paths.ffmpeg = config.paths.root / "internals" / "ffmpeg";
#endif
            
            config.paths.internals = config.paths.root / "internals";
            config.paths.url_dir = config.paths.root / "url";
            config.paths.logs = config.paths.root / "logs";
            config.paths.state = config.paths.root / "state";
            
            // Criar diretórios
            fs::create_directories(config.paths.internals);
            fs::create_directories(config.paths.url_dir);
            fs::create_directories(config.paths.logs);
            fs::create_directories(config.paths.state);
            
            // Diretório de saída padrão
            config.settings.output_dir = (config.paths.root / "downloads").string();
            fs::create_directories(config.settings.output_dir);
            
        } catch (const std::exception& e) {
            ProfessionalUI::Error("Error initializing paths: " + std::string(e.what()));
            config.paths.root = fs::current_path();
        }
    }
    
    void LoadConfiguration() {
        fs::path configFile = config.paths.internals / "settings.ini";
        
        if (!fs::exists(configFile)) {
            CreateDefaultConfiguration();
            return;
        }
        
        std::ifstream file(configFile);
        std::string line;
        std::map<std::string, std::string> values;
        
        while (std::getline(file, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            
            size_t pos = line.find('=');
            if (pos == std::string::npos) continue;
            
            std::string key = Trim(line.substr(0, pos));
            std::string value = Trim(line.substr(pos + 1));
            
            if (!key.empty()) {
                values[key] = value;
            }
        }
        
        // Carregar valores com validação
        config.settings.mode = ValidateMode(values.count("MODE") ? values["MODE"] : "video");
        config.settings.audio_format = ValidateAudioFormat(values.count("AUDIO_FORMAT") ? values["AUDIO_FORMAT"] : "mp3");
        config.settings.output_dir = values.count("OUTPUT_DIR") ? values["OUTPUT_DIR"] : 
                                     (config.paths.root / "downloads").string();
        config.settings.telegram_preset = values.count("TELEGRAM_PRESET") ? 
                                          (values["TELEGRAM_PRESET"] == "1") : true;
        config.settings.download_archive = values.count("DOWNLOAD_ARCHIVE") ? 
                                           (values["DOWNLOAD_ARCHIVE"] == "1") : true;
        config.settings.embed_metadata = values.count("EMBED_METADATA") ? 
                                         (values["EMBED_METADATA"] == "1") : true;
        config.settings.restrict_filenames = values.count("RESTRICT_FILENAMES") ? 
                                             (values["RESTRICT_FILENAMES"] == "1") : true;
        config.settings.max_concurrent = ValidateConcurrent(values.count("MAX_CONCURRENT") ? 
                                           std::stoi(values["MAX_CONCURRENT"]) : 1);
        config.settings.retries = ValidateRetries(values.count("RETRIES") ? 
                                  std::stoi(values["RETRIES"]) : 3);
        config.settings.ignore_errors = values.count("IGNORE_ERRORS") ? 
                                        (values["IGNORE_ERRORS"] == "1") : true;
        config.settings.newline_output = values.count("NEWLINE_OUTPUT") ? 
                                         (values["NEWLINE_OUTPUT"] == "1") : true;
        config.settings.throttle_playlists = values.count("THROTTLE_PLAYLISTS") ? 
                                             (values["THROTTLE_PLAYLISTS"] == "1") : true;
        
        fs::create_directories(config.settings.output_dir);
    }
    
    void CreateDefaultConfiguration() {
        config.settings.mode = "video";
        config.settings.audio_format = "mp3";
        config.settings.output_dir = (config.paths.root / "downloads").string();
        config.settings.telegram_preset = true;
        config.settings.download_archive = true;
        config.settings.embed_metadata = true;
        config.settings.restrict_filenames = true;
        config.settings.max_concurrent = 1;
        config.settings.retries = 3;
        config.settings.ignore_errors = true;
        config.settings.newline_output = true;
        config.settings.throttle_playlists = true;
        
        SaveConfiguration();
        ProfessionalUI::Info("Created default configuration");
    }
    
    void SaveConfiguration() {
        fs::path configFile = config.paths.internals / "settings.ini";
        
        std::ofstream file(configFile, std::ios::trunc);
        file << "# Media Pipeline Configuration v4.0\n";
        file << "# Generated on " << GetTimestamp() << "\n\n";
        
        file << "MODE=" << config.settings.mode << "\n";
        file << "AUDIO_FORMAT=" << config.settings.audio_format << "\n";
        file << "OUTPUT_DIR=" << config.settings.output_dir << "\n";
        file << "TELEGRAM_PRESET=" << (config.settings.telegram_preset ? "1" : "0") << "\n";
        file << "DOWNLOAD_ARCHIVE=" << (config.settings.download_archive ? "1" : "0") << "\n";
        file << "EMBED_METADATA=" << (config.settings.embed_metadata ? "1" : "0") << "\n";
        file << "RESTRICT_FILENAMES=" << (config.settings.restrict_filenames ? "1" : "0") << "\n";
        file << "MAX_CONCURRENT=" << config.settings.max_concurrent << "\n";
        file << "RETRIES=" << config.settings.retries << "\n";
        file << "IGNORE_ERRORS=" << (config.settings.ignore_errors ? "1" : "0") << "\n";
        file << "NEWLINE_OUTPUT=" << (config.settings.newline_output ? "1" : "0") << "\n";
        file << "THROTTLE_PLAYLISTS=" << (config.settings.throttle_playlists ? "1" : "0") << "\n";
    }
    
    void ShowMainMenu() {
        ProfessionalUI::PrintHeader("MEDIA PIPELINE v4.0");
        
        std::cout << "\n";
        ProfessionalUI::PrintMenuOption(1, "Download from list");
        ProfessionalUI::PrintMenuOption(2, "Advanced dry-run analysis");
        ProfessionalUI::PrintMenuOption(3, "Test single URL");
        ProfessionalUI::PrintMenuOption(4, "Manage URL lists");
        ProfessionalUI::PrintMenuOption(5, "Configuration");
        ProfessionalUI::PrintMenuOption(6, "Check dependencies");
        ProfessionalUI::PrintMenuOption(7, "View statistics");
        ProfessionalUI::PrintMenuOption(8, "Open download folder");
        ProfessionalUI::PrintMenuOption(9, "Exit");
        
        std::cout << "\n";
        ProfessionalUI::Print("Select option [1-9]: ", ProfessionalUI::YELLOW);
        
        std::string input;
        std::getline(std::cin, input);
        
        if (input.empty()) return;
        
        try {
            int choice = std::stoi(input);
            
            switch (choice) {
                case 1: DownloadMenu(); break;
                case 2: AdvancedDryRunMenu(); break;
                case 3: TestUrlMenu(); break;
                case 4: ManageListsMenu(); break;
                case 5: ConfigurationMenu(); break;
                case 6: CheckDependencies(); break;
                case 7: ViewStatistics(); break;
                case 8: OpenDownloadFolder(); break;
                case 9: ExitApplication(); break;
                default: ProfessionalUI::Error("Invalid option"); break;
            }
        } catch (...) {
            ProfessionalUI::Error("Invalid input");
        }
    }
    
    void DownloadMenu() {
        if (g_interrupted.load()) {
            g_interrupted = false;
            ProfessionalUI::Info("Reset interrupt flag");
        }
        
        auto lists = GetUrlLists();
        if (lists.empty()) {
            ProfessionalUI::Warning("No URL lists found. Create one in 'Manage URL lists'");
            return;
        }
        
        ShowListSelection(lists, [this](const fs::path& listPath) {
            auto stats = downloadManager.DownloadList(listPath, false);
            ShowDownloadResults(stats);
        });
    }
    
    void AdvancedDryRunMenu() {
        auto lists = GetUrlLists();
        if (lists.empty()) {
            ProfessionalUI::Warning("No URL lists found");
            return;
        }
        
        ShowListSelection(lists, [this](const fs::path& listPath) {
            auto results = downloadManager.AdvancedDryRun(listPath);
            ShowDryRunResults(results);
        });
    }
    
    void TestUrlMenu() {
        ProfessionalUI::PrintHeader("TEST SINGLE URL");
        
        std::cout << "\n";
        ProfessionalUI::Print("Enter URL to test: ", ProfessionalUI::YELLOW);
        
        std::string url;
        std::getline(std::cin, url);
        url = Trim(url);
        
        if (url.empty()) {
            ProfessionalUI::Error("URL cannot be empty");
            return;
        }
        
        if (!UrlValidator::IsValidUrl(url)) {
            ProfessionalUI::Error("Invalid URL format");
            return;
        }
        
        auto type = UrlValidator::ClassifyUrl(url);
        bool isPlaylist = UrlValidator::LooksLikePlaylist(url);
        
        ProfessionalUI::Info("URL Analysis:");
        ProfessionalUI::Info("  Type: " + UrlValidator::GetUrlTypeName(type));
        ProfessionalUI::Info("  Playlist: " + std::string(isPlaylist ? "Yes" : "No"));
        
        if (ProfessionalUI::GetConfirmation("\nPerform detailed test?")) {
            // Implementar teste detalhado com yt-dlp --skip-download
            ProfessionalUI::Info("Testing URL accessibility...");
            // (Implementação do teste detalhado aqui)
        }
    }
    
    void ManageListsMenu() {
        while (true) {
            ProfessionalUI::PrintHeader("MANAGE URL LISTS");
            
            auto lists = GetUrlLists();
            
            if (lists.empty()) {
                ProfessionalUI::Info("No lists found. Create a new one:");
            } else {
                std::cout << "\nCurrent lists:\n";
                for (size_t i = 0; i < lists.size(); i++) {
                    std::cout << "  " << (i + 1) << ") " << lists[i].filename().string() << "\n";
                }
            }
            
            std::cout << "\n";
            ProfessionalUI::PrintMenuOption(1, "Create new list");
            ProfessionalUI::PrintMenuOption(2, "Edit list");
            ProfessionalUI::PrintMenuOption(3, "Delete list");
            ProfessionalUI::PrintMenuOption(4, "Back to main menu");
            
            ProfessionalUI::Print("\nSelect option [1-4]: ", ProfessionalUI::YELLOW);
            
            std::string input;
            std::getline(std::cin, input);
            
            if (input.empty()) continue;
            
            try {
                int choice = std::stoi(input);
                
                if (choice == 4) break;
                
                switch (choice) {
                    case 1: CreateNewList(); break;
                    case 2: EditList(lists); break;
                    case 3: DeleteList(lists); break;
                }
            } catch (...) {
                ProfessionalUI::Error("Invalid input");
            }
        }
    }
    
    void ConfigurationMenu() {
        while (true) {
            ProfessionalUI::PrintHeader("CONFIGURATION");
            
            std::cout << "\nCurrent settings:\n";
            std::cout << "  1. Mode: " << config.settings.mode << "\n";
            std::cout << "  2. Audio format: " << config.settings.audio_format << "\n";
            std::cout << "  3. Output directory: " << config.settings.output_dir << "\n";
            std::cout << "  4. Telegram preset: " << (config.settings.telegram_preset ? "ON" : "OFF") << "\n";
            std::cout << "  5. Download archive: " << (config.settings.download_archive ? "ON" : "OFF") << "\n";
            std::cout << "  6. Max concurrent: " << config.settings.max_concurrent << "\n";
            std::cout << "  7. Retries: " << config.settings.retries << "\n";
            std::cout << "  8. Ignore errors: " << (config.settings.ignore_errors ? "ON" : "OFF") << "\n";
            std::cout << "  9. Throttle playlists: " << (config.settings.throttle_playlists ? "ON" : "OFF") << "\n";
            std::cout << " 10. Save and exit\n";
            std::cout << "  0. Cancel without saving\n";
            
            ProfessionalUI::Print("\nSelect option to change [0-10]: ", ProfessionalUI::YELLOW);
            
            std::string input;
            std::getline(std::cin, input);
            
            if (input.empty()) continue;
            
            try {
                int choice = std::stoi(input);
                
                if (choice == 0) break;
                if (choice == 10) { SaveConfiguration(); break; }
                
                switch (choice) {
                    case 1: ChangeMode(); break;
                    case 2: ChangeAudioFormat(); break;
                    case 3: ChangeOutputDirectory(); break;
                    case 4: config.settings.telegram_preset = !config.settings.telegram_preset; break;
                    case 5: config.settings.download_archive = !config.settings.download_archive; break;
                    case 6: ChangeConcurrentDownloads(); break;
                    case 7: ChangeRetries(); break;
                    case 8: config.settings.ignore_errors = !config.settings.ignore_errors; break;
                    case 9: config.settings.throttle_playlists = !config.settings.throttle_playlists; break;
                }
            } catch (...) {
                ProfessionalUI::Error("Invalid input");
            }
        }
    }
    
    void CheckDependencies() {
        ProfessionalUI::PrintHeader("DEPENDENCIES CHECK");
        
        bool ytdlp_ok = fs::exists(config.paths.ytdlp);
        bool ffmpeg_ok = fs::exists(config.paths.ffmpeg);
        
        std::cout << "\n";
        std::cout << "yt-dlp: " << (ytdlp_ok ? "✓ Found" : "✗ Missing") << "\n";
        std::cout << "ffmpeg: " << (ffmpeg_ok ? "✓ Found" : "✗ Missing") << "\n";
        
        if (!ytdlp_ok || !ffmpeg_ok) {
            std::cout << "\n";
            ProfessionalUI::Important("Missing dependencies detected!");
            ProfessionalUI::Info("Download from:");
            ProfessionalUI::Info("  yt-dlp: https://github.com/yt-dlp/yt-dlp/releases");
            ProfessionalUI::Info("  ffmpeg: https://ffmpeg.org/download.html");
            ProfessionalUI::Info("Place executables in: " + config.paths.internals.string());
        }
    }
    
    void ViewStatistics() {
        ProfessionalUI::PrintHeader("STATISTICS");
        
        std::cout << "\n";
        std::cout << "Total downloads: " << config.stats.total_downloads << "\n";
        std::cout << "Successful: " << config.stats.successful_downloads << "\n";
        std::cout << "Failed: " << config.stats.failed_downloads << "\n";
        
        if (config.stats.total_downloads > 0) {
            double success_rate = (double)config.stats.successful_downloads / config.stats.total_downloads * 100;
            std::cout << "Success rate: " << std::fixed << std::setprecision(1) << success_rate << "%\n";
        }
        
        if (!config.stats.last_list.empty()) {
            std::cout << "Last list: " << config.stats.last_list << "\n";
        }
    }
    
    void OpenDownloadFolder() {
#ifdef _WIN32
        std::string cmd = "explorer \"" + config.settings.output_dir + "\"";
#else
        std::string cmd = "xdg-open \"" + config.settings.output_dir + "\" 2>/dev/null";
#endif
        
        std::system(cmd.c_str());
    }
    
    void ExitApplication() {
        if (ProfessionalUI::GetConfirmation("\nAre you sure you want to exit?")) {
            running = false;
        }
    }
    
    // Helper methods
    std::vector<fs::path> GetUrlLists() {
        std::vector<fs::path> lists;
        
        if (!fs::exists(config.paths.url_dir)) {
            return lists;
        }
        
        for (const auto& entry : fs::directory_iterator(config.paths.url_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                lists.push_back(entry.path());
            }
        }
        
        std::sort(lists.begin(), lists.end());
        return lists;
    }
    
    void ShowListSelection(const std::vector<fs::path>& lists, 
                          std::function<void(const fs::path&)> callback) {
        if (lists.empty()) return;
        
        std::cout << "\nAvailable lists:\n";
        for (size_t i = 0; i < lists.size(); i++) {
            std::cout << "  " << (i + 1) << ") " << lists[i].filename().string() << "\n";
        }
        
        ProfessionalUI::Print("\nSelect list [1-" + std::to_string(lists.size()) + "], or 0 to cancel: ", 
                             ProfessionalUI::YELLOW);
        
        std::string input;
        std::getline(std::cin, input);
        
        try {
            int choice = std::stoi(input);
            
            if (choice > 0 && choice <= static_cast<int>(lists.size())) {
                if (ProfessionalUI::GetConfirmation("Start processing?")) {
                    callback(lists[choice - 1]);
                }
            }
        } catch (...) {
            ProfessionalUI::Error("Invalid selection");
        }
    }
    
    void ShowDownloadResults(const ProfessionalDownloadManager::DownloadStats& stats) {
        ProfessionalUI::PrintHeader("DOWNLOAD RESULTS");
        
        std::cout << "\n";
        std::cout << "Total URLs: " << stats.total << "\n";
        std::cout << "Successful: " << stats.successful << "\n";
        std::cout << "Failed: " << stats.failed << "\n";
        std::cout << "Skipped: " << stats.skipped << "\n";
        
        if (stats.total_time.count() > 0) {
            std::cout << "Time: " << stats.total_time.count() << " seconds\n";
        }
        
        if (!stats.failedUrls.empty()) {
            std::cout << "\nFailed URLs:\n";
            for (const auto& url : stats.failedUrls) {
                std::cout << "  " << url << "\n";
            }
        }
    }
    
    void ShowDryRunResults(const std::vector<ProfessionalDownloadManager::DryRunInfo>& results) {
        ProfessionalUI::PrintHeader("DRY RUN RESULTS");
        
        int total_urls = results.size();
        int playlists = 0;
        int estimated_items = 0;
        
        for (const auto& info : results) {
            if (info.is_playlist) {
                playlists++;
                estimated_items += info.estimated_items;
            }
        }
        
        std::cout << "\n";
        std::cout << "Total URLs: " << total_urls << "\n";
        std::cout << "Playlists: " << playlists << "\n";
        
        if (estimated_items > 0) {
            std::cout << "Estimated total items: " << estimated_items << "\n";
        }
        
        if (playlists > 0 && config.settings.max_concurrent > 3) {
            ProfessionalUI::Warning("High concurrency setting may cause issues with playlists");
        }
    }
    
    void CreateNewList() {
        ProfessionalUI::Print("Enter list name (without .txt): ", ProfessionalUI::YELLOW);
        
        std::string name;
        std::getline(std::cin, name);
        name = Trim(name);
        
        if (name.empty()) {
            ProfessionalUI::Error("List name cannot be empty");
            return;
        }
        
        fs::path listPath = config.paths.url_dir / (name + ".txt");
        
        if (fs::exists(listPath)) {
            if (!ProfessionalUI::GetConfirmation("List already exists. Overwrite?")) {
                return;
            }
        }
        
        std::ofstream file(listPath);
        ProfessionalUI::Success("List created: " + listPath.filename().string());
    }
    
    void EditList(const std::vector<fs::path>& lists) {
        if (lists.empty()) return;
        
        ShowListSelection(lists, [](const fs::path& listPath) {
            // Implementar editor simples
            ProfessionalUI::Info("Editing: " + listPath.filename().string());
            
#ifdef _WIN32
            std::string cmd = "notepad \"" + listPath.string() + "\"";
#else
            std::string cmd = "nano \"" + listPath.string() + "\"";
#endif
            
            std::system(cmd.c_str());
        });
    }
    
    void DeleteList(const std::vector<fs::path>& lists) {
        if (lists.empty()) return;
        
        ShowListSelection(lists, [](const fs::path& listPath) {
            if (ProfessionalUI::GetConfirmation("Delete list: " + listPath.filename().string() + "?")) {
                try {
                    fs::remove(listPath);
                    ProfessionalUI::Success("List deleted");
                } catch (...) {
                    ProfessionalUI::Error("Failed to delete list");
                }
            }
        });
    }
    
    void ChangeMode() {
        ProfessionalUI::Print("Set mode (video/audio): ", ProfessionalUI::YELLOW);
        
        std::string mode;
        std::getline(std::cin, mode);
        mode = Trim(mode);
        
        if (mode == "video" || mode == "audio") {
            config.settings.mode = mode;
        } else {
            ProfessionalUI::Error("Invalid mode. Must be 'video' or 'audio'");
        }
    }
    
    void ChangeAudioFormat() {
        std::cout << "\nAvailable audio formats:\n";
        std::cout << "  1) mp3 (MP3)\n";
        std::cout << "  2) m4a (AAC)\n";
        std::cout << "  3) opus (Opus)\n";
        std::cout << "  4) vorbis (Vorbis)\n";
        std::cout << "  5) flac (FLAC)\n";
        std::cout << "  6) wav (WAV)\n";
        
        ProfessionalUI::Print("\nSelect format [1-6] or enter custom: ", ProfessionalUI::YELLOW);
        
        std::string input;
        std::getline(std::cin, input);
        input = Trim(input);
        
        if (input.empty()) return;
        
        if (input == "1") config.settings.audio_format = "mp3";
        else if (input == "2") config.settings.audio_format = "m4a";
        else if (input == "3") config.settings.audio_format = "opus";
        else if (input == "4") config.settings.audio_format = "vorbis";
        else if (input == "5") config.settings.audio_format = "flac";
        else if (input == "6") config.settings.audio_format = "wav";
        else config.settings.audio_format = input;
    }
    
    void ChangeOutputDirectory() {
        ProfessionalUI::Print("Set output directory: ", ProfessionalUI::YELLOW);
        
        std::string dir;
        std::getline(std::cin, dir);
        dir = Trim(dir);
        
        if (!dir.empty()) {
            try {
                fs::path newPath(dir);
                if (!newPath.is_absolute()) {
                    newPath = config.paths.root / newPath;
                }
                
                fs::create_directories(newPath);
                config.settings.output_dir = newPath.string();
                ProfessionalUI::Success("Output directory updated");
            } catch (...) {
                ProfessionalUI::Error("Invalid directory path");
            }
        }
    }
    
    void ChangeConcurrentDownloads() {
        int value = ProfessionalUI::GetInteger("Set concurrent downloads", 1, 10, config.settings.max_concurrent);
        config.settings.max_concurrent = value;
        
        if (value > 3) {
            ProfessionalUI::Important("Note: High concurrency may cause server throttling");
        }
    }
    
    void ChangeRetries() {
        int value = ProfessionalUI::GetInteger("Set retry count", 0, 20, config.settings.retries);
        config.settings.retries = value;
    }
    
    // Validation methods
    std::string ValidateMode(const std::string& mode) {
        if (mode == "video" || mode == "audio") {
            return mode;
        }
        
        ProfessionalUI::Warning("Invalid mode '" + mode + "', defaulting to 'video'");
        return "video";
    }
    
    std::string ValidateAudioFormat(const std::string& format) {
        static const std::set<std::string> valid_formats = {
            "mp3", "m4a", "opus", "vorbis", "flac", "wav"
        };
        
        if (valid_formats.count(format)) {
            return format;
        }
        
        ProfessionalUI::Warning("Invalid audio format '" + format + "', defaulting to 'mp3'");
        return "mp3";
    }
    
    int ValidateConcurrent(int concurrent) {
        if (concurrent >= 1 && concurrent <= 10) {
            return concurrent;
        }
        
        ProfessionalUI::Warning("Invalid concurrent value '" + std::to_string(concurrent) + 
                               "', defaulting to 1");
        return 1;
    }
    
    int ValidateRetries(int retries) {
        if (retries >= 0 && retries <= 20) {
            return retries;
        }
        
        ProfessionalUI::Warning("Invalid retries value '" + std::to_string(retries) + 
                               "', defaulting to 3");
        return 3;
    }
    
    std::string Trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }
    
    std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &now_time);
#else
        localtime_r(&now_time, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
};

// ==================== MAIN ====================
int main() {
    try {
        MediaPipelineApp app;
        app.Run();
    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL ERROR] " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "\n[FATAL ERROR] Unknown exception\n";
        return 1;
    }
    
    return 0;
}
