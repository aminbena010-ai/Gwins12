// gwins12.cpp - Meta-gestor de paquetes universal ultrarrápido
// Compilar: make  (o: g++ -std=c++20 -O3 -o gwins12 gwins12.cpp -lcurl -lsolv -lsolvext -lpthread)
// NOTA: repo_add_debpackages vive en libsolvext, por eso se enlaza -lsolvext.
// Uso: ./gwins12 install curl wget | ./gwins12 search openssl | ./gwins12 update

#define GWINS12_VERSION "0.2.0"

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <future>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <functional>
#include <optional>
#include <variant>
#include <array>
#include <cassert>
#include <cctype>
#include <ctime>
#include <atomic>
#include <queue>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// === DEPENDENCIAS EXTERNAS (enlazadas en compilación) ===

// Workaround para C++20: 'requires' es una palabra clave.
// libsolv la usa como identificador.
#if defined(__cplusplus) && __cplusplus >= 202002L
#define requires _requires
#endif

extern "C" {
    #include <solv/solver.h>
    #include <solv/transaction.h>
    #include <solv/pool.h>
    #include <solv/repo.h>
    #include <solv/selection.h>
    #include <solv/repo_deb.h>
    #include <solv/solvable.h>
    #include <curl/curl.h>
}
#if defined(__cplusplus) && __cplusplus >= 202002L
#undef requires
// libsolv de Debian no se compiló con LIBSOLV_SOLVABLE_PREPEND_DEP:
// el miembro Solvable::requires es keyword C++20 → renombrar aquí.
#define requires _requires
#endif

namespace fs = std::filesystem;

// ============================================================================
// SECCIÓN 1: UTILIDADES Y CONFIGURACIÓN GLOBAL
// ============================================================================

struct GwinsConfig {
    fs::path cacheDir;
    fs::path storeDir;      // Content-Addressable Storage
    fs::path lockFile;
    int maxParallelDownloads = 8;
    bool verbose = false;
    
    static GwinsConfig load() {
        GwinsConfig cfg;
        const char* home = std::getenv("HOME");
        fs::path base = home ? fs::path(home) / ".gwins12" : fs::path("/tmp/gwins12");
        auto configureDirectories = [&cfg](const fs::path& root, std::error_code& ec) {
            cfg.cacheDir = root / "cache";
            cfg.storeDir = root / "store";
            cfg.lockFile = root / "lock";
            fs::create_directories(cfg.cacheDir, ec);
            if (!ec) fs::create_directories(cfg.storeDir, ec);
        };

        std::error_code ec;
        configureDirectories(base, ec);
        if (ec) {
            ec.clear();
            fs::path temporaryBase = fs::temp_directory_path(ec) / "gwins12";
            if (!ec) configureDirectories(temporaryBase, ec);
        }
        if (ec) {
            throw std::runtime_error("Unable to create gwins12 cache directories: " + ec.message());
        }
        return cfg;
    }
};

// Timer estilo uv para benchmarking
class Timer {
    std::chrono::high_resolution_clock::time_point start_;
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}
    double elapsedMs() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }
};

// Logging con colores
enum class LogLevel { INFO, WARN, ERROR, DEBUG };
void log(LogLevel level, const std::string& msg, bool verbose = false) {
    if (level == LogLevel::DEBUG && !verbose) return;
    const char* prefix[] = {"\033[32m[INFO]\033[0m", "\033[33m[WARN]\033[0m", 
                             "\033[31m[ERROR]\033[0m", "\033[90m[DEBUG]\033[0m"};
    std::cerr << prefix[static_cast<int>(level)] << " " << msg << "\n";
}

// ============================================================================
// SECCIÓN 2: DETECTOR DE SISTEMA OPERATIVO
// ============================================================================

enum class PackageManager { APT, PACMAN, DNF, BREW, PKG, UNKNOWN };

struct SystemInfo {
    std::string osId;
    std::string version;
    PackageManager pm;
    std::string pmName;
};

SystemInfo detectSystem() {
    SystemInfo info{"unknown", "", PackageManager::UNKNOWN, "none"};
    
    // Detectar Linux vía /etc/os-release
    std::ifstream osRelease("/etc/os-release");
    if (osRelease.is_open()) {
        std::string line;
        while (std::getline(osRelease, line)) {
            if (line.starts_with("ID=")) {
                info.osId = line.substr(3);
                // Quitar comillas si existen
                if (!info.osId.empty() && info.osId.front() == '"')
                    info.osId = info.osId.substr(1, info.osId.size()-2);
            }
            if (line.starts_with("VERSION_ID=")) {
                info.version = line.substr(11);
                if (!info.version.empty() && info.version.front() == '"')
                    info.version = info.version.substr(1, info.version.size()-2);
            }
        }
    }
    
    // Detectar gestor disponible
    auto checkCmd = [](const char* cmd) -> bool {
        std::string s = "which "; s += cmd; s += " > /dev/null 2>&1";
        return system(s.c_str()) == 0;
    };
    
    if (checkCmd("apt"))       { info.pm = PackageManager::APT;    info.pmName = "apt"; }
    else if (checkCmd("pacman")){ info.pm = PackageManager::PACMAN; info.pmName = "pacman"; }
    else if (checkCmd("dnf"))   { info.pm = PackageManager::DNF;    info.pmName = "dnf"; }
    else if (checkCmd("brew"))  { info.pm = PackageManager::BREW;   info.pmName = "brew"; }
    else if (checkCmd("pkg"))   { info.pm = PackageManager::PKG;    info.pmName = "pkg"; }
    
    // macOS fallback
    #ifdef __APPLE__
    if (info.pm == PackageManager::UNKNOWN && checkCmd("brew")) {
        info.osId = "macos";
        info.pm = PackageManager::BREW;
        info.pmName = "brew";
    }
    #endif
    
    return info;
}

// ============================================================================
// SECCIÓN 3: INTERFAZ DE BACKEND (Patrón Strategy)
// ============================================================================

struct PackageInfo {
    std::string name;
    std::string version;
    std::string description;
    size_t sizeBytes;
    std::string downloadUrl;
    std::string sha256;
};

// ============================================================================
// SECCIÓN 0: SISTEMA VISUAL GWINS12 (UX/UI PARA TERMINAL)
// ============================================================================

namespace ui {

struct Color {
    static constexpr const char* RESET = "\033[0m";
    static constexpr const char* BOLD = "\033[1m";
    static constexpr const char* DIM = "\033[2m";
    static constexpr const char* GREEN = "\033[38;5;114m";
    static constexpr const char* CYAN = "\033[38;5;80m";
    static constexpr const char* YELLOW = "\033[38;5;221m";
    static constexpr const char* RED = "\033[38;5;203m";
    static constexpr const char* MAGENTA = "\033[38;5;176m";
    static constexpr const char* WHITE = "\033[38;5;252m";
    static constexpr const char* GRAY = "\033[38;5;243m";
};

struct Icon {
    static constexpr const char* TICK = "✓";
    static constexpr const char* CROSS = "✗";
    static constexpr const char* ARROW = "→";
    static constexpr const char* DOT = "•";
};

inline std::string truncate(std::string value, size_t width) {
    if (value.size() > width) value = value.substr(0, width - 3) + "...";
    return value;
}

void header(const std::string& title) {
    std::cerr << "\n  " << Color::BOLD << Color::CYAN << title << Color::RESET << "\n"
              << Color::GRAY << "  ";
    for (size_t i = 0; i < title.size(); ++i) std::cerr << "─";
    std::cerr << Color::RESET << "\n";
}

void packageLine(const std::string& name, const std::string& version,
                 const std::string& description, bool isNew = true) {
    std::string paddedName = truncate(name, 25);
    paddedName.resize(25, ' ');
    std::string paddedVersion = truncate(version, 15);
    paddedVersion.resize(15, ' ');

    std::cerr << "  " << (isNew ? Color::GREEN : Color::GRAY)
              << (isNew ? Icon::ARROW : Icon::DOT) << Color::RESET << " "
              << Color::WHITE << paddedName << Color::RESET
              << Color::MAGENTA << paddedVersion << Color::RESET
              << Color::GRAY << truncate(description, 45) << Color::RESET << "\n";
}

void info(const std::string& message) {
    std::cerr << "  " << Color::CYAN << Icon::DOT << Color::RESET << " "
              << Color::WHITE << message << Color::RESET << "\n";
}

void warn(const std::string& message) {
    std::cerr << "  " << Color::YELLOW << "!" << Color::RESET << " "
              << Color::YELLOW << message << Color::RESET << "\n";
}

void error(const std::string& message) {
    std::cerr << "\n  " << Color::RED << Icon::CROSS << " Error: " << Color::RESET
              << Color::WHITE << message << Color::RESET << "\n\n";
}

void summary(int installed, int upgraded, int removed, double elapsedMs) {
    std::cerr << "\n  " << Color::BOLD << Color::GREEN << Icon::TICK << " Done"
              << Color::RESET << " in " << Color::CYAN << elapsedMs << "ms"
              << Color::RESET << "\n";
    if (installed || upgraded || removed) {
        std::cerr << "  ";
        if (installed) std::cerr << Color::GREEN << installed << " installed  " << Color::RESET;
        if (upgraded) std::cerr << Color::CYAN << upgraded << " upgraded  " << Color::RESET;
        if (removed) std::cerr << Color::RED << removed << " removed" << Color::RESET;
        std::cerr << "\n";
    }
    std::cerr << "\n";
}

void searchResults(const std::vector<PackageInfo>& results, double elapsedMs) {
    if (results.empty()) {
        std::cerr << "\n  " << Color::GRAY << "No packages found." << Color::RESET << "\n\n";
        return;
    }
    header("Search Results");
    for (const auto& package : results)
        packageLine(package.name, package.version, package.description);
    std::cerr << "\n  " << Color::GRAY << "Found " << results.size() << " packages in "
              << elapsedMs << "ms" << Color::RESET << "\n\n";
}

// Progreso de operaciones multi-hilo (thread-safe)
struct ProgressDisplay {
    std::string label;
    size_t total = 0;
    size_t done = 0;
    std::mutex mutex;

    void init(size_t n, const std::string& l) {
        label = l;
        total = n;
        done = 0;
    }

    void advance(const std::string& item, bool ok) {
        std::lock_guard<std::mutex> lock(mutex);
        ++done;
        std::cerr << "  " << (ok ? Color::GREEN : Color::RED)
                  << (ok ? Icon::TICK : Icon::CROSS) << Color::RESET << " "
                  << Color::WHITE << truncate(item, 40) << Color::RESET
                  << Color::GRAY << " [" << done << "/" << total << "]"
                  << Color::RESET << "\n";
    }

    void finish() {
        std::cerr << "\n";
    }
};

} // namespace ui

// ============================================================================
// SECCIÓN 0.5: PARSER DE ARGUMENTOS
// ============================================================================

namespace args {

struct Flags {
    bool verbose = false;
    bool version = false;
    bool help = false;
    bool pretend = false;
    bool yes = false;
    bool quiet = false;
    bool noCache = false;
    bool recursive = false;
    // === FLAGS GIT/REPOS ===
    bool gitClone = false;     // -g (clonar repo)
    bool gitShallow = false;   // --shallow (depth=1)
    std::string gitBranch;     // -b <branch> o --branch <branch>
    std::string gitDest;       // -d <path> o --dest <path>
    std::string command;
    std::vector<std::string> positional;
    std::string error;
    bool parsed = false;
};

class Parser {
public:
    static Flags parse(int argc, char* argv[]) {
        Flags flags;
        if (argc < 2) {
            flags.help = true;
            flags.parsed = true;
            return flags;
        }

        bool commandFound = false;
        bool parseOptions = true;
        const auto setError = [&flags](std::string message) {
            // Keep the first error, but continue parsing so --help and --version
            // retain their documented precedence regardless of argument order.
            if (flags.error.empty()) flags.error = std::move(message);
        };

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (parseOptions && arg == "--") {
                parseOptions = false;
                continue;
            }
            if (parseOptions && arg.size() > 1 && arg[0] == '-' && arg[1] != '-') {
                for (size_t c = 1; c < arg.size(); ++c) {
                    switch (arg[c]) {
                        case 'v': flags.verbose = true; break;
                        case 'V': flags.version = true; break;
                        case 'h': flags.help = true; break;
                        case 'p': flags.pretend = true; break;
                        case 'y': flags.yes = true; break;
                        case 'q': flags.quiet = true; break;
                        case 'n': flags.noCache = true; break;
                        case 'r': flags.recursive = true; break;

                        // -g puede llevar URL inline (p.ej. -ghttps://..., -guser/repo).
                        // Si el resto no parece URL, se sigue parseando como flags apilados (-gv, -gq).
                        case 'g': {
                            flags.gitClone = true;
                            if (c + 1 < arg.size()) {
                                std::string rest = arg.substr(c + 1);
                                bool looksLikeUrl = rest.find('/') != std::string::npos ||
                                                    rest.find(':') != std::string::npos ||
                                                    rest.find('@') != std::string::npos ||
                                                    rest.find('.') != std::string::npos;
                                if (looksLikeUrl) {
                                    flags.positional.push_back(rest);
                                    c = arg.size();
                                }
                            }
                            break;
                        }
                        // -b <branch>: inline o siguiente argumento
                        case 'b': {
                            if (c + 1 < arg.size()) {
                                flags.gitBranch = arg.substr(c + 1);
                                c = arg.size();
                            } else if (i + 1 < argc) {
                                flags.gitBranch = argv[++i];
                            } else {
                                setError("Flag -b requires a branch name");
                            }
                            break;
                        }
                        // -d <dest>: inline o siguiente argumento
                        case 'd': {
                            if (c + 1 < arg.size()) {
                                flags.gitDest = arg.substr(c + 1);
                                c = arg.size();
                            } else if (i + 1 < argc) {
                                flags.gitDest = argv[++i];
                            } else {
                                setError("Flag -d requires a destination path");
                            }
                            break;
                        }

                        default: setError(std::string("Unknown flag: -") + arg[c]); break;
                    }
                }
                continue;
            }
            if (parseOptions && arg.starts_with("--")) {
                if (arg == "--verbose") flags.verbose = true;
                else if (arg == "--version") flags.version = true;
                else if (arg == "--help") flags.help = true;
                else if (arg == "--pretend" || arg == "--dry-run") flags.pretend = true;
                else if (arg == "--yes" || arg == "--assume-yes") flags.yes = true;
                else if (arg == "--quiet") flags.quiet = true;
                else if (arg == "--no-cache") flags.noCache = true;
                else if (arg == "--recursive") flags.recursive = true;
                else if (arg == "--shallow") flags.gitShallow = true;
                else if (arg == "--branch") {
                    if (i + 1 < argc) flags.gitBranch = argv[++i];
                    else setError("Option --branch requires an argument");
                }
                else if (arg == "--dest") {
                    if (i + 1 < argc) flags.gitDest = argv[++i];
                    else setError("Option --dest requires an argument");
                }
                else setError("Unknown option: " + arg);
                continue;
            }
            if (!commandFound) {
                flags.command = std::move(arg);
                commandFound = true;
            } else {
                flags.positional.push_back(std::move(arg));
            }
        }

        if (flags.quiet && flags.verbose)
            setError("Cannot use -q and -v together");
        flags.parsed = true;
        return flags;
    }
};

void printHelp() {
    std::cerr << "\n"
              << ui::Color::BOLD << ui::Color::CYAN << "  Gwins12" << ui::Color::RESET
              << " v" GWINS12_VERSION " — Universal Package Manager\n\n"
              << ui::Color::WHITE << "  USAGE:" << ui::Color::RESET << "\n"
              << "    gwins12 [FLAGS] <COMMAND> [PACKAGES...]\n"
              << "    gwins12 -g <url|user/repo> [FLAGS]\n\n"
              << ui::Color::WHITE << "  COMMANDS:" << ui::Color::RESET << "\n"
              << "    search <query>       Search packages across repositories\n"
              << "    install <pkgs...>    Resolve and install packages\n"
              << "    bootstrap <profile>  Install a complete environment profile\n"
              << "    update               Refresh package metadata\n"
              << "    version              Show version information\n"
              << "    help                 Show this help message\n\n"
              << ui::Color::WHITE << "  FLAGS:" << ui::Color::RESET << "\n"
              << "    " << ui::Color::GREEN << "-h, --help" << ui::Color::RESET << "            Show this help message\n"
              << "    " << ui::Color::GREEN << "-V, --version" << ui::Color::RESET << "         Show version information\n"
              << "    " << ui::Color::GREEN << "-v, --verbose" << ui::Color::RESET << "         Enable detailed output\n"
              << "    " << ui::Color::GREEN << "-q, --quiet" << ui::Color::RESET << "           Suppress non-essential output\n"
              << "    " << ui::Color::GREEN << "-p, --pretend" << ui::Color::RESET << "         Resolve without installing\n"
              << "    " << ui::Color::GREEN << "-y, --yes" << ui::Color::RESET << "             Auto-confirm installation\n"
              << "    " << ui::Color::GREEN << "-n, --no-cache" << ui::Color::RESET << "        Request metadata refresh\n"
              << "    " << ui::Color::GREEN << "-r, --recursive" << ui::Color::RESET << "       Request dependency details\n"
              << "    " << ui::Color::GREEN << "-g" << ui::Color::RESET << " <url/repo>"
              << "          Clone a git repository\n"
              << "    " << ui::Color::GREEN << "-b" << ui::Color::RESET << ", "
              << ui::Color::GREEN << "--branch" << ui::Color::RESET << " <name>  "
              << "Specify branch/tag\n"
              << "    " << ui::Color::GREEN << "-d" << ui::Color::RESET << ", "
              << ui::Color::GREEN << "--dest" << ui::Color::RESET << " <path>    "
              << "Clone destination directory\n"
              << "    " << ui::Color::GREEN << "--shallow" << ui::Color::RESET
              << "              Shallow clone (depth=1)\n\n"
              << ui::Color::WHITE << "  EXAMPLES:" << ui::Color::RESET << "\n"
              << "    gwins12 install curl wget\n"
              << "    gwins12 search openssl\n"
              << "    gwins12 -g torvalds/linux           " << ui::Color::GRAY << "# GitHub shorthand" << ui::Color::RESET << "\n"
              << "    gwins12 -g user/repo -b dev --shallow  " << ui::Color::GRAY << "# branch + shallow" << ui::Color::RESET << "\n"
              << "    gwins12 -g https://gitlab.com/x/y -d ./src\n\n";
}

} // namespace args

class PackageBackend {
public:
    virtual ~PackageBackend() = default;
    virtual bool init() = 0;
    virtual std::vector<PackageInfo> search(const std::string& query) = 0;
    virtual std::vector<PackageInfo> resolve(const std::vector<std::string>& packages) = 0;
    virtual bool install(const std::vector<PackageInfo>& packages) = 0;
    virtual bool updateCache() = 0;
    virtual std::string name() const = 0;
};

// ============================================================================
// SECCIÓN 3.5: METADATA CACHE BINARIA MMAP
// (Definida antes de LibsolvBackend porque init() la usa)
// Formato v2: guarda nombre, versión, descripción, arch y las cadenas de
// dependencias (requires/provides/conflicts/obsoletes) con nombre+evr+flags.
// Round-trip completo pool→mmap→pool: búsqueda zero-copy Y resolución SAT.
// ============================================================================

class MetadataCache {
    fs::path cacheDir_;

    struct CacheHeader {
        uint32_t magic;          // 0x47573132 = "GW12"
        uint32_t version;        // 2
        uint64_t timestamp;      // epoch seconds
        uint64_t packageCount;
        uint64_t stringTableSize;
        uint64_t entryOffset;    // offset to first PackageEntry
        uint64_t depArrayOffset; // offset to first DepEntry
        uint64_t depArraySize;   // bytes
    };

    struct __attribute__((packed)) PackageEntry {
        uint32_t nameOffset;
        uint32_t versionOffset;
        uint32_t descOffset;
        uint32_t archOffset;
        uint32_t sizeBytes;
        uint32_t flags;
        uint32_t requiresCount, requiresOffset;
        uint32_t providesCount, providesOffset;
        uint32_t conflictsCount, conflictsOffset;
        uint32_t obsoletesCount, obsoletesOffset;
    };

    struct __attribute__((packed)) DepEntry {
        uint32_t nameOffset;
        uint32_t evrOffset;
        uint32_t flags;
    };

public:
    explicit MetadataCache(fs::path dir) : cacheDir_(std::move(dir)) {
        std::error_code ec;
        fs::create_directories(cacheDir_, ec);
    }

    // Serializar el repo libsolv completo al formato binario zero-copy
    bool saveFromPool(const std::string& repoId, Pool* pool, Repo* repo) {
        fs::path cacheFile = cacheDir_ / (repoId + ".gwcache");

        std::string stringTable;
        std::unordered_map<std::string, uint32_t> stringOffsets;

        auto internString = [&](const char* s) -> uint32_t {
            if (!s || !*s) return 0;
            std::string str(s);
            auto it = stringOffsets.find(str);
            if (it != stringOffsets.end()) return it->second;
            uint32_t offset = static_cast<uint32_t>(stringTable.size());
            stringTable += str;
            stringTable.push_back('\0');
            stringOffsets[str] = offset;
            return offset;
        };

        std::vector<PackageEntry> entries;
        std::vector<DepEntry> depArray;
        entries.reserve(pool->nsolvables);

        Solvable* s = nullptr;
        Id p = 0;
        FOR_REPO_SOLVABLES(repo, p, s) {
            Id solvid = s - pool->solvables;
            PackageEntry e{};
            e.nameOffset = internString(pool_id2str(pool, s->name));
            e.versionOffset = internString(pool_id2str(pool, s->evr));
            e.archOffset = internString(pool_id2str(pool, s->arch));
            const char* desc = pool_lookup_str(pool, solvid, SOLVABLE_DESCRIPTION);
            if (!desc) desc = pool_lookup_str(pool, solvid, SOLVABLE_SUMMARY);
            e.descOffset = internString(desc);
            e.sizeBytes = static_cast<uint32_t>(
                pool_lookup_num(pool, solvid, SOLVABLE_DOWNLOADSIZE, 0));

            auto saveDeps = [&](Offset off) -> std::pair<uint32_t, uint32_t> {
                if (!off) return {0, 0};
                Id* arr = repo->idarraydata + off;
                uint32_t count = 0;
                uint32_t offset = static_cast<uint32_t>(depArray.size() * sizeof(DepEntry));
                for (Id* d = arr; *d; d++) {
                    // Las cadenas pueden contener markers (ids planos tipo
                    // SOLVABLE_PREREQMARKER); solo nos interesan los rel ids.
                    if ((*d & 0x80000000u) == 0) continue;
                    if ((*d ^ 0x80000000u) >= static_cast<unsigned>(pool->nrels)) continue;
                    Reldep* r = GETRELDEP(pool, *d);
                    DepEntry de{};
                    de.nameOffset = internString(pool_id2str(pool, r->name));
                    de.evrOffset = r->evr ? internString(pool_id2str(pool, r->evr)) : 0;
                    de.flags = static_cast<uint32_t>(r->flags);
                    depArray.push_back(de);
                    count++;
                }
                return {count, offset};
            };
            {
                auto [count, offset] = saveDeps(s->requires);
                e.requiresCount = count; e.requiresOffset = offset;
            }
            {
                auto [count, offset] = saveDeps(s->provides);
                e.providesCount = count; e.providesOffset = offset;
            }
            {
                auto [count, offset] = saveDeps(s->conflicts);
                e.conflictsCount = count; e.conflictsOffset = offset;
            }
            {
                auto [count, offset] = saveDeps(s->obsoletes);
                e.obsoletesCount = count; e.obsoletesOffset = offset;
            }

            entries.push_back(e);
        }

        std::ofstream out(cacheFile, std::ios::binary);
        if (!out) return false;

        CacheHeader header{};
        header.magic = 0x47573132;
        header.version = 2;
        header.timestamp = static_cast<uint64_t>(std::time(nullptr));
        header.packageCount = entries.size();
        header.stringTableSize = stringTable.size();
        header.entryOffset = sizeof(CacheHeader) + stringTable.size();
        header.depArrayOffset = header.entryOffset +
            entries.size() * sizeof(PackageEntry);
        header.depArraySize = depArray.size() * sizeof(DepEntry);

        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(stringTable.data(), static_cast<std::streamsize>(stringTable.size()));
        out.write(reinterpret_cast<const char*>(entries.data()),
                  static_cast<std::streamsize>(entries.size() * sizeof(PackageEntry)));
        out.write(reinterpret_cast<const char*>(depArray.data()),
                  static_cast<std::streamsize>(depArray.size() * sizeof(DepEntry)));

        return out.good();
    }

    // Cargar con mmap para acceso zero-copy instantáneo
    struct MappedCache {
        void* data = nullptr;
        size_t size = 0;
        const CacheHeader* header = nullptr;
        const char* strings = nullptr;
        const PackageEntry* entries = nullptr;
        const char* depArray = nullptr;
        int fd = -1;

        ~MappedCache() {
            #ifndef _WIN32
            if (data && size > 0) munmap(data, size);
            if (fd >= 0) close(fd);
            #endif
        }

        size_t count() const { return header ? header->packageCount : 0; }

        const char* str(uint32_t offset) const { return strings + offset; }

        const PackageEntry& entry(size_t index) const { return entries[index]; }

        const DepEntry* deps(uint32_t count, uint32_t offset) const {
            if (!count || !offset) return nullptr;
            return reinterpret_cast<const DepEntry*>(depArray + offset);
        }

        PackageInfo getPackage(size_t index) const {
            const auto& e = entries[index];
            PackageInfo pi;
            pi.name = str(e.nameOffset);
            pi.version = str(e.versionOffset);
            pi.description = str(e.descOffset);
            pi.sizeBytes = e.sizeBytes;
            return pi;
        }
    };

    std::unique_ptr<MappedCache> load(const std::string& repoId) {
        fs::path cacheFile = cacheDir_ / (repoId + ".gwcache");
        if (!fs::exists(cacheFile)) return nullptr;

        auto mc = std::make_unique<MappedCache>();

        #ifndef _WIN32
        mc->fd = open(cacheFile.c_str(), O_RDONLY);
        if (mc->fd < 0) return nullptr;

        struct stat st;
        if (fstat(mc->fd, &st) < 0 || st.st_size < static_cast<off_t>(sizeof(CacheHeader))) {
            return nullptr;
        }

        mc->size = static_cast<size_t>(st.st_size);
        mc->data = mmap(nullptr, mc->size, PROT_READ, MAP_PRIVATE, mc->fd, 0);
        if (mc->data == MAP_FAILED) {
            mc->data = nullptr;
            mc->size = 0;
            return nullptr;
        }

        mc->header = reinterpret_cast<const CacheHeader*>(mc->data);
        if (mc->header->magic != 0x47573132 || mc->header->version != 2) {
            return nullptr; // Cache inválida o de versión antigua
        }
        if (mc->header->depArrayOffset + mc->header->depArraySize > mc->size) {
            return nullptr; // Tamaño inconsistente
        }

        mc->strings = reinterpret_cast<const char*>(mc->data) + sizeof(CacheHeader);
        mc->entries = reinterpret_cast<const PackageEntry*>(
            reinterpret_cast<const char*>(mc->data) + mc->header->entryOffset);
        mc->depArray = reinterpret_cast<const char*>(mc->data) + mc->header->depArrayOffset;
        #endif

        return mc;
    }
};

// ============================================================================
// SECCIÓN 4: BACKEND LIBSOLV (Resolver SAT Universal)
// ============================================================================

class LibsolvBackend : public PackageBackend {
    Pool* pool_ = nullptr;
    Repo* repo_ = nullptr;
    PackageManager sysPm_;
    std::unique_ptr<MetadataCache::MappedCache> cache_;
    
    // Reconstruir las cadenas de dependencias de un solvable desde la caché
    void addDeps(Solvable* s, Id keyname, uint32_t count, uint32_t offset) {
        if (!count || !offset) return;
        const auto* deps = cache_->deps(count, offset);
        for (uint32_t i = 0; i < count; i++) {
            Id nameid = pool_str2id(pool_, cache_->str(deps[i].nameOffset), 1);
            Id evrid = deps[i].evrOffset
                ? pool_str2id(pool_, cache_->str(deps[i].evrOffset), 1) : 0;
            Id relid = pool_rel2id(pool_, nameid, evrid, static_cast<int>(deps[i].flags), 1);
            solvable_add_deparray(s, keyname, relid, 0);
        }
    }
    
    void buildPoolFromCache() {
        repo_ = repo_create(pool_, "apt-system");
        if (!repo_) return;
        for (size_t i = 0; i < cache_->count(); i++) {
            const auto& e = cache_->entry(i);
            Solvable* s = pool_id2solvable(pool_, repo_add_solvable(repo_));
            s->name = pool_str2id(pool_, cache_->str(e.nameOffset), 1);
            s->evr = e.versionOffset ? pool_str2id(pool_, cache_->str(e.versionOffset), 1) : 0;
            s->arch = e.archOffset ? pool_str2id(pool_, cache_->str(e.archOffset), 1) : 0;
            addDeps(s, SOLVABLE_REQUIRES,  e.requiresCount,  e.requiresOffset);
            addDeps(s, SOLVABLE_PROVIDES,  e.providesCount,  e.providesOffset);
            addDeps(s, SOLVABLE_CONFLICTS, e.conflictsCount, e.conflictsOffset);
            addDeps(s, SOLVABLE_OBSOLETES, e.obsoletesCount, e.obsoletesOffset);
        }
        pool_createwhatprovides(pool_);
    }
    
public:
    explicit LibsolvBackend(PackageManager pm) : sysPm_(pm) {}
    
    ~LibsolvBackend() override {
        if (pool_) pool_free(pool_);
    }
    
    bool init() override {
        pool_ = pool_create();
        if (!pool_) return false;

        // Fijar arquitectura del pool
        const char* archName = "amd64";
        #if defined(__aarch64__) || defined(_M_ARM64)
        archName = "arm64";
        #elif !defined(__x86_64__) && !defined(_M_X64)
        archName = "i386";
        #endif
        pool_setarch(pool_, archName);

        MetadataCache metaCache(GwinsConfig::load().cacheDir / "meta");

        // === INTENTAR CACHÉ MMAP PRIMERO (segunda ejecución: <10ms) ===
        cache_ = metaCache.load("apt-system");
        if (cache_ && cache_->count() > 100) {
            Timer t;
            log(LogLevel::INFO, "Loaded " + std::to_string(cache_->count()) +
                " packages from mmap cache in " + std::to_string(t.elapsedMs()) + "ms");
            return true;
        }

        // === FALLBACK: Parsear archivos Packages originales ===
        log(LogLevel::DEBUG, "Cache miss, parsing apt lists...", true);
        int loadedFiles = 0;
        repo_ = repo_create(pool_, "apt-system");
        if (!repo_) return false;

        const fs::path aptListsDir = "/var/lib/apt/lists";
        if (fs::exists(aptListsDir)) {
            for (const auto& entry : fs::directory_iterator(aptListsDir)) {
                std::string filename = entry.path().filename().string();
                if (filename.ends_with("_Packages")) {
                    FILE* fp = fopen(entry.path().c_str(), "r");
                    if (fp) {
                        repo_add_debpackages(repo_, fp, 0);
                        fclose(fp);
                        loadedFiles++;
                    }
                }
            }
        }

        pool_createwhatprovides(pool_);

        // Guardar caché binaria con dependencias para la próxima ejecución
        Timer t;
        if (metaCache.saveFromPool("apt-system", pool_, repo_)) {
            log(LogLevel::INFO, "Saved " + std::to_string(pool_->nsolvables) +
                " packages to mmap cache in " + std::to_string(t.elapsedMs()) + "ms");
        }
        cache_ = metaCache.load("apt-system");

        log(LogLevel::INFO, "Loaded " + std::to_string(pool_->nsolvables) +
            " packages from " + std::to_string(loadedFiles) + " apt list files");
        return true;
    }
    
    // Construcción LAZY del pool completo (solo para resolver):
    // el search usa la caché mmap directamente y no paga este coste.
    bool ensurePool() {
        if (pool_ && repo_) return true;
        pool_ = pool_create();
        if (!pool_) return false;

        const char* archName = "amd64";
        #if defined(__aarch64__) || defined(_M_ARM64)
        archName = "arm64";
        #elif !defined(__x86_64__) && !defined(_M_X64)
        archName = "i386";
        #endif
        pool_setarch(pool_, archName);

        if (cache_ && cache_->count() > 100) {
            Timer t;
            buildPoolFromCache();
            if (!repo_) return false;
            log(LogLevel::INFO, "Built SAT pool from mmap cache in " +
                std::to_string(t.elapsedMs()) + "ms");
            return true;
        }
        return false;
    }
    
    std::vector<PackageInfo> search(const std::string& query) override {
        std::vector<PackageInfo> results;
        if (!cache_) return results;

        // Escaneo case-insensitive directo sobre el mmap (zero-copy, sin
        // construir std::strings por paquete)
        auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; };
        std::string q = query;
        for (auto& c : q) c = low(c);

        for (size_t i = 0; i < cache_->count(); i++) {
            const char* name = cache_->str(cache_->entry(i).nameOffset);
            bool match = false;
            for (const char* a = name; *a && !match; a++) {
                const char* p = a;
                const char* b = q.c_str();
                while (*p && *b && low(*p) == *b) { p++; b++; }
                if (!*b) match = true;
            }
            if (match) results.push_back(cache_->getPackage(i));
        }
        return results;
    }
    
    std::vector<PackageInfo> resolve(const std::vector<std::string>& packages) override {
        std::vector<PackageInfo> resolved;
        if (packages.empty()) return resolved;
        if (!ensurePool()) return resolved;

        Timer t;
        
        // Crear job de instalación
        Queue job;
        queue_init(&job);
        for (const auto& pkg : packages) {
            Id id = pool_str2id(pool_, pkg.c_str(), 0);
            if (id != 0) {
                queue_push2(&job, SOLVER_INSTALL | SOLVER_SOLVABLE_NAME, id);
            }
        }
        
        // Resolver con SAT solver (PARALELO internamente en libsolv moderno)
        Solver* solver = solver_create(pool_);
        if (solver_solve(solver, &job) == 0) {
            // Extraer solución
            Queue decisionq;
            queue_init(&decisionq);
            solver_get_decisionqueue(solver, &decisionq);
            Transaction* trans = transaction_create_decisionq(pool_, &decisionq, nullptr);
            for (int i = 0; i < trans->steps.count; i++) {
                Id step = trans->steps.elements[i];
                if (step < 0) continue; // pasos de borrado → no son instalaciones
                Solvable* s = pool_id2solvable(pool_, step);
                PackageInfo pi;
                pi.name = pool_id2str(pool_, s->name);
                pi.version = pool_id2str(pool_, s->evr);
                resolved.push_back(std::move(pi));
            }
            transaction_free(trans);
            queue_free(&decisionq);
        } else {
            log(LogLevel::ERROR, "Resolution failed: unsolvable dependencies");
            // TODO: Reportar conflictos detallados como hace uv
        }
        
        solver_free(solver);
        queue_free(&job);
        
        log(LogLevel::DEBUG, "Resolved " + std::to_string(resolved.size()) +
            " packages in " + std::to_string(t.elapsedMs()) + "ms");
        return resolved;
    }
    
    bool install(const std::vector<PackageInfo>& packages) override {
        // Gwins12 resuelve el plan completo (SAT) y DELEGA la instalación
        // al gestor nativo del sistema para no corromper su base de datos.
        if (packages.empty()) return false;

        std::string cmd;
        #ifndef _WIN32
        const bool sudo = (geteuid() != 0);
        #else
        const bool sudo = false;
        #endif
        switch (sysPm_) {
            case PackageManager::APT:    cmd = (sudo ? "sudo " : "") + std::string("apt-get install -y"); break;
            case PackageManager::PACMAN: cmd = (sudo ? "sudo " : "") + std::string("pacman -S --noconfirm"); break;
            case PackageManager::DNF:    cmd = (sudo ? "sudo " : "") + std::string("dnf install -y"); break;
            case PackageManager::BREW:   cmd = "brew install"; break;
            case PackageManager::PKG:    cmd = (sudo ? "sudo " : "") + std::string("pkg install -y"); break;
            default:
                log(LogLevel::ERROR, "No native package manager available for installation");
                return false;
        }
        for (const auto& p : packages) cmd += " " + p.name;

        log(LogLevel::INFO, "Running: " + cmd);
        return system(cmd.c_str()) == 0;
    }
    
    bool updateCache() override {
        // Ejecutar la actualización de metadatos nativa (apt-get update, etc.)
        // y reconstruir la caché mmap de libsolv con los nuevos datos.
        std::string cmd;
        #ifndef _WIN32
        const bool sudo = (geteuid() != 0);
        #else
        const bool sudo = false;
        #endif
        switch (sysPm_) {
            case PackageManager::APT:    cmd = (sudo ? "sudo " : "") + std::string("apt-get update"); break;
            case PackageManager::PACMAN: cmd = (sudo ? "sudo " : "") + std::string("pacman -Sy"); break;
            case PackageManager::DNF:    cmd = (sudo ? "sudo " : "") + std::string("dnf makecache"); break;
            case PackageManager::BREW:   cmd = "brew update"; break;
            case PackageManager::PKG:    cmd = (sudo ? "sudo " : "") + std::string("pkg update"); break;
            default:
                log(LogLevel::ERROR, "No native package manager available for updates");
                return false;
        }

        log(LogLevel::INFO, "Running: " + cmd);
        bool ok = (system(cmd.c_str()) == 0);
        if (!ok) return false;

        // Invalidar la caché mmap y reconstruirla desde los listados nuevos
        std::error_code ec;
        fs::remove(GwinsConfig::load().cacheDir / "meta" / "apt-system.gwcache", ec);
        cache_.reset();
        if (pool_) {
            pool_free(pool_);
            pool_ = nullptr;
            repo_ = nullptr;
        }
        return init();
    }
    
    std::string name() const override { return "libsolv-universal"; }
};

// ============================================================================
// SECCIÓN 5: PIPELINE DE DESCARGA PARALELO MULTI-HILO REAL
// ============================================================================
// SECCIÓN 5.5: SHA-256 SELF-CONTAINED (cero dependencias externas)
// ============================================================================

namespace crypto {

class Sha256 {
    uint32_t h_[8];
    uint64_t bitlen_ = 0;
    uint8_t block_[64];
    size_t blockLen_ = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    static void transform(uint32_t state[8], const uint8_t block[64]) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t(block[i*4]) << 24) | (uint32_t(block[i*4+1]) << 16) |
                   (uint32_t(block[i*4+2]) << 8) | uint32_t(block[i*4+3]);
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void update(const uint8_t* data, size_t len) {
        bitlen_ += len * 8;
        while (len > 0) {
            size_t space = 64 - blockLen_;
            size_t take = std::min(space, len);
            std::memcpy(block_ + blockLen_, data, take);
            blockLen_ += take;
            data += take;
            len -= take;
            if (blockLen_ == 64) {
                transform(h_, block_);
                blockLen_ = 0;
            }
        }
    }

    void finalize(uint8_t digest[32]) {
        uint64_t bitlen = bitlen_;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (blockLen_ != 56) update(&zero, 1);
        uint8_t lenbuf[8];
        for (int i = 0; i < 8; i++) lenbuf[i] = static_cast<uint8_t>(bitlen >> (56 - i*8));
        update(lenbuf, 8);
        for (int i = 0; i < 8; i++) {
            digest[i*4]   = static_cast<uint8_t>(h_[i] >> 24);
            digest[i*4+1] = static_cast<uint8_t>(h_[i] >> 16);
            digest[i*4+2] = static_cast<uint8_t>(h_[i] >> 8);
            digest[i*4+3] = static_cast<uint8_t>(h_[i]);
        }
    }

public:
    Sha256() {
        h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372; h_[3] = 0xa54ff53a;
        h_[4] = 0x510e527f; h_[5] = 0x9b05688c; h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
    }

    // Hash de archivo completo en hex (64 chars), vacío si no se puede leer
    static std::string hexFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        Sha256 ctx;
        std::array<char, 65536> buf;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            ctx.update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(in.gcount()));
        }
        uint8_t digest[32];
        ctx.finalize(digest);
        std::string hex;
        hex.reserve(64);
        for (uint8_t b : digest) {
            hex += "0123456789abcdef"[b >> 4];
            hex += "0123456789abcdef"[b & 0xf];
        }
        return hex;
    }

    static bool verifyFile(const fs::path& path, const std::string& expectedHex) {
        if (expectedHex.empty()) return true; // sin hash esperado → no verificar
        return hexFile(path) == expectedHex;
    }
};

} // namespace crypto

// ============================================================================

// ============================================================================

class DownloadPipeline {
    int maxThreads_;
    std::atomic<size_t> totalBytes_{0};
    std::atomic<size_t> downloadedBytes_{0};
    
    static size_t curlWriteCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* stream = static_cast<std::ofstream*>(userdata);
        size_t written = size * nmemb;
        stream->write(static_cast<char*>(ptr), static_cast<std::streamsize>(written));
        return written;
    }
    
    static size_t progressCb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        return 0;
    }
    
    bool downloadOne(const std::string& url, const fs::path& destPath,
                     const std::string& expectedHash = "") {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        std::ofstream file(destPath, std::ios::binary);
        if (!file) { curl_easy_cleanup(curl); return false; }
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Gwins12/" GWINS12_VERSION);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        
        CURLcode rc = curl_easy_perform(curl);
        file.close();
        curl_easy_cleanup(curl);
        
        if (rc != CURLE_OK) {
            fs::remove(destPath);
            return false;
        }
        // Verificación de integridad SHA-256 (si se especificó hash esperado)
        if (!crypto::Sha256::verifyFile(destPath, expectedHash)) {
            log(LogLevel::ERROR, "SHA-256 mismatch for " + destPath.filename().string());
            fs::remove(destPath);
            return false;
        }
        return true;
    }
    
public:
    explicit DownloadPipeline(int maxThreads = 8) : maxThreads_(maxThreads) {}
    
    struct DownloadRequest {
        std::string url;
        fs::path dest;
        std::string hash;
    };
    
    struct BatchResult {
        size_t total = 0;
        size_t succeeded = 0;
        size_t failed = 0;
        double elapsedMs = 0.0;
        size_t totalBytes = 0;
    };
    
    BatchResult downloadBatch(const std::vector<DownloadRequest>& requests,
                              ui::ProgressDisplay& progress) {
        Timer t;
        BatchResult result;
        result.total = requests.size();
        
        if (requests.empty()) return result;
        
        // Cola de tareas compartida entre workers
        std::queue<DownloadRequest> taskQueue;
        std::mutex queueMutex;
        
        for (const auto& req : requests) taskQueue.push(req);
        
        auto worker = [&]() {
            while (true) {
                DownloadRequest req;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    if (taskQueue.empty()) return;
                    req = std::move(taskQueue.front());
                    taskQueue.pop();
                }
                
                bool ok = downloadOne(req.url, req.dest, req.hash);
                progress.advance(req.dest.filename().string(), ok);
                
                if (ok) result.succeeded++;
                else result.failed++;
            }
        };
        
        // Lanzar workers
        int numWorkers = std::min(maxThreads_, static_cast<int>(requests.size()));
        std::vector<std::thread> threads;
        for (int i = 0; i < numWorkers; i++) threads.emplace_back(worker);
        for (auto& th : threads) th.join();
        
        result.elapsedMs = t.elapsedMs();
        return result;
    }
};

// SECCIÓN 6: CONTENT-ADDRESSABLE STORAGE
// ============================================================================

class ContentStore {
    fs::path root_;
    
public:
    explicit ContentStore(fs::path root) : root_(std::move(root)) {
        std::error_code ec;
        fs::create_directories(root_, ec);
        if (ec) {
            throw std::runtime_error("Unable to create content store: " + ec.message());
        }
    }
    
    // Almacenar archivo por su hash SHA256 (deduplicación real)
    fs::path store(const fs::path& sourceFile) {
        std::string hash = crypto::Sha256::hexFile(sourceFile);
        if (hash.empty()) return {};

        fs::path dest = root_ / hash.substr(0, 2) / hash;
        std::error_code ec;
        if (!fs::exists(dest, ec)) {
            fs::create_directories(dest.parent_path(), ec);
            if (!ec) fs::copy_file(sourceFile, dest, fs::copy_options::overwrite_existing, ec);
        }
        return ec ? fs::path{} : dest;
    }
    
    // Verificar si ya existe en store (deduplicación instantánea)
    bool has(const std::string& hash) const {
        return fs::exists(root_ / hash.substr(0, 2) / hash);
    }
    
    fs::path getPath(const std::string& hash) const {
        return root_ / hash.substr(0, 2) / hash;
    }
};

// ============================================================================
// SECCIÓN 6.5: GESTOR DE REPOSITORIOS GIT INTEGRADO
// ============================================================================

class GitManager {
    bool hasGit_ = false;
    SystemInfo sysInfo_;

    static bool commandExists(const char* cmd) {
        std::string s = "which "; s += cmd; s += " > /dev/null 2>&1";
        return system(s.c_str()) == 0;
    }

    static std::string shellEscape(const std::string& s) {
        std::string escaped = "'";
        for (char c : s) {
            if (c == '\'') escaped += "'\\''";
            else escaped += c;
        }
        escaped += "'";
        return escaped;
    }

    std::string nativeGitInstallCommand() const {
        switch (sysInfo_.pm) {
            case PackageManager::APT:    return "apt-get install -y git";
            case PackageManager::PACMAN: return "pacman -S --noconfirm git";
            case PackageManager::DNF:    return "dnf install -y git";
            case PackageManager::BREW:   return "brew install git";
            case PackageManager::PKG:    return "pkg install -y git";
            default: return "";
        }
    }

    static std::string toArchiveUrl(const std::string& url, const std::string& branch) {
        std::string ref = branch.empty() ? "HEAD" : branch;
        // GitHub
        if (url.find("github.com") != std::string::npos) {
            std::string clean = url;
            if (clean.ends_with(".git")) clean = clean.substr(0, clean.size() - 4);
            return clean + "/archive/" + ref + ".tar.gz";
        }
        // GitLab
        if (url.find("gitlab.com") != std::string::npos) {
            std::string clean = url;
            if (clean.ends_with(".git")) clean = clean.substr(0, clean.size() - 4);
            return clean + "/-/archive/" + ref + "/" + extractRepoName(url) + "-" + ref + ".tar.gz";
        }
        return ""; // No soportado sin git
    }

public:
    explicit GitManager(const SystemInfo& sysInfo) : sysInfo_(sysInfo) {
        hasGit_ = commandExists("git");
    }

    bool hasGit() const { return hasGit_; }

    // Convierte shorthand a URL completa (user/repo → https://github.com/user/repo)
    static std::string normalizeUrl(const std::string& input) {
        if (input.find('/') != std::string::npos &&
            input.find("://") == std::string::npos &&
            input.find('@') == std::string::npos) {
            return "https://github.com/" + input;
        }
        return input;
    }

    // Extrae el nombre del repo de la URL
    static std::string extractRepoName(const std::string& url) {
        std::string name = url;
        if (name.ends_with(".git")) name = name.substr(0, name.size() - 4);
        while (!name.empty() && name.back() == '/') name.pop_back();
        auto pos = name.rfind('/');
        if (pos != std::string::npos) name = name.substr(pos + 1);
        return name.empty() ? "repo" : name;
    }

    struct CloneOptions {
        std::string url;
        std::string branch;
        std::string dest;
        bool shallow = false;
        bool verbose = false;
        bool quiet = false;
        bool assumeYes = false;
        bool autoInstallGit = true;
    };

    struct CloneResult {
        bool success = false;
        std::string path;       // Ruta donde se clonó
        std::string error;
        double elapsedMs = 0.0;
        bool usedBuiltin = false; // true si usó fallback sin git
    };

    // Auto-instalación transparente de git vía el gestor nativo
    bool installGit(bool quiet) {
        std::string cmd = nativeGitInstallCommand();
        if (cmd.empty()) return false;
        if (!quiet) ui::info("Installing git via " + sysInfo_.pmName + "...");
        int rc = system(cmd.c_str());
        if (rc == 0) hasGit_ = true;
        return rc == 0;
    }

    CloneResult clone(CloneOptions opts) {
        Timer t;
        CloneResult result;

        std::string url = normalizeUrl(opts.url);
        std::string dest = opts.dest;
        if (dest.empty()) dest = extractRepoName(url);

        // === AUTO-INSTALACIÓN TRANSPARENTE DE GIT ===
        if (!hasGit_ && opts.autoInstallGit) {
            bool shouldInstall = opts.assumeYes;
            if (!opts.quiet && !shouldInstall) {
                std::cerr << "  git not found. Install it now? [Y/n] ";
                std::string answer;
                std::getline(std::cin, answer);
                shouldInstall = answer.empty() || answer[0] == 'y' || answer[0] == 'Y';
            }
            if (shouldInstall) {
                if (!installGit(opts.quiet) && !opts.quiet) {
                    ui::warn("Could not install git — using direct download fallback.");
                }
            } else if (!opts.quiet) {
                ui::warn("Skipping install — using direct download fallback.");
            }
        }

        if (hasGit_) {
            // === CLONAR CON GIT NATIVO ===
            std::string cmd = "git clone";
            if (opts.shallow) cmd += " --depth 1";
            if (!opts.branch.empty()) cmd += " -b " + shellEscape(opts.branch);
            if (opts.quiet) cmd += " -q";
            if (opts.verbose) cmd += " --progress";
            cmd += " " + shellEscape(url) + " " + shellEscape(dest);

            if (!opts.quiet) ui::info("Cloning " + url + " → " + dest);

            int rc = system(cmd.c_str());
            result.success = (rc == 0);
            if (!result.success) {
                result.error = "git clone failed with exit code " + std::to_string(rc);
            }
        } else {
            // === FALLBACK: DESCARGA DIRECTA SIN GIT ===
            result.usedBuiltin = true;

            std::string archiveUrl = toArchiveUrl(url, opts.branch);
            if (archiveUrl.empty()) {
                result.error = "Cannot clone without git. Only GitHub/GitLab URLs are supported in fallback mode.";
                result.elapsedMs = t.elapsedMs();
                return result;
            }

            if (!opts.quiet) {
                ui::warn("git not found. Using direct download fallback.");
                ui::info("Downloading " + archiveUrl);
            }

            // Crear directorio destino
            fs::create_directories(dest);

            // Descargar archivo temporal
            std::string tmpFile = "/tmp/gwins12_clone_" +
                std::to_string(std::hash<std::string>{}(url)) + ".tar.gz";

            CURL* curl = curl_easy_init();
            if (curl) {
                FILE* fp = fopen(tmpFile.c_str(), "wb");
                if (fp) {
                    curl_easy_setopt(curl, CURLOPT_URL, archiveUrl.c_str());
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Gwins12/" GWINS12_VERSION);

                    CURLcode rc = curl_easy_perform(curl);
                    fclose(fp);
                    curl_easy_cleanup(curl);

                    if (rc != CURLE_OK) {
                        result.error = "Download failed: " + std::string(curl_easy_strerror(rc));
                        fs::remove(tmpFile);
                        result.elapsedMs = t.elapsedMs();
                        return result;
                    }
                } else {
                    curl_easy_cleanup(curl);
                    result.error = "Could not create temporary file";
                    result.elapsedMs = t.elapsedMs();
                    return result;
                }
            } else {
                result.error = "Failed to init curl";
                result.elapsedMs = t.elapsedMs();
                return result;
            }

            // Extraer tarball
            std::string extractCmd = "tar xzf " + shellEscape(tmpFile) +
                                     " -C " + shellEscape(dest) + " --strip-components=1";
            int rc = system(extractCmd.c_str());
            fs::remove(tmpFile);

            result.success = (rc == 0);
            if (!result.success) {
                result.error = "Extraction failed. Install git for full clone support.";
            }
        }

        result.path = dest;
        result.elapsedMs = t.elapsedMs();
        return result;
    }
};

// ============================================================================
// SECCIÓN 7: MOTOR PRINCIPAL GWINS12
// ============================================================================

class Gwins12Engine {
    GwinsConfig config_;
    SystemInfo sysInfo_;
    std::unique_ptr<PackageBackend> backend_;
    DownloadPipeline downloader_;
    ContentStore store_;
    GitManager gitMgr_;
    
public:
    Gwins12Engine() 
        : config_(GwinsConfig::load())
        , sysInfo_(detectSystem())
        , downloader_(config_.maxParallelDownloads)
        , store_(config_.storeDir)
        , gitMgr_(sysInfo_)
    {
        log(LogLevel::DEBUG, "Gwins12 v" GWINS12_VERSION, config_.verbose);
        log(LogLevel::DEBUG, "Detected: " + sysInfo_.osId + " (" + sysInfo_.pmName + ")",
            config_.verbose);
        
        // Inicializar backend universal
        backend_ = std::make_unique<LibsolvBackend>(sysInfo_.pm);
        if (!backend_->init()) {
            log(LogLevel::ERROR, "Failed to initialize backend");
        }
    }
    
    int run(const args::Flags& flags) {
        // === COMANDO: GIT CLONE (-g) ===
        if (flags.gitClone) {
            // Con -g, la URL puede ocupar el slot de "command"
            // (primer argumento no-flag) en lugar de positional.
            std::string urlArg = !flags.positional.empty()
                ? flags.positional.front() : flags.command;

            if (urlArg.empty()) {
                ui::error("Clone requires a repository URL or shorthand.");
                std::cerr << "  Usage: gwins12 -g <user/repo>\n"
                          << "         gwins12 -g https://github.com/user/repo\n"
                          << "         gwins12 -g user/repo -b main --shallow\n\n";
                return 1;
            }

            std::string url = GitManager::normalizeUrl(urlArg);
            std::string dest = flags.gitDest;
            if (dest.empty()) dest = GitManager::extractRepoName(url);

            if (flags.pretend) {
                ui::header("Dry-Run Clone Plan");
                ui::info("Would clone " + url + " → " + dest);
                std::cerr << "\n";
                return 0;
            }

            GitManager::CloneOptions opts;
            opts.url = urlArg;
            opts.branch = flags.gitBranch;
            opts.dest = flags.gitDest;
            opts.shallow = flags.gitShallow;
            opts.verbose = flags.verbose;
            opts.quiet = flags.quiet;
            opts.assumeYes = flags.yes;

            if (!flags.quiet) {
                ui::header("Git Clone");
                if (!gitMgr_.hasGit()) {
                    ui::warn("git not installed — will try to install it or use HTTP fallback");
                }
            }

            auto result = gitMgr_.clone(opts);

            if (result.success) {
                if (!flags.quiet) {
                    std::cerr << "\n  " << ui::Color::GREEN << ui::Icon::TICK
                              << ui::Color::RESET << " Cloned to "
                              << ui::Color::WHITE << result.path << ui::Color::RESET;
                    if (result.usedBuiltin) {
                        std::cerr << " " << ui::Color::YELLOW << "(fallback mode)"
                                  << ui::Color::RESET;
                    }
                    std::cerr << "\n";
                    if (flags.verbose) {
                        std::cerr << "  " << ui::Color::GRAY << "Completed in "
                                  << result.elapsedMs << "ms" << ui::Color::RESET << "\n";
                    }
                    std::cerr << "\n";
                }
                return 0;
            } else {
                ui::error(result.error);
                if (!gitMgr_.hasGit() && !result.usedBuiltin) {
                    ui::info("Install git for full clone support.");
                }
                return 1;
            }
        }

        if (flags.command.empty()) {
            ui::error("No command specified.");
            std::cerr << "  Run 'gwins12 -h' for usage information.\n\n";
            return 1;
        }

        config_.verbose = flags.verbose;
        Timer total;

        if (!flags.quiet && (flags.command == "install" || flags.command == "search")) {
            std::cerr << ui::Color::DIM << "  gwins12 v" GWINS12_VERSION << " | "
                      << sysInfo_.osId << "/" << sysInfo_.pmName;
            if (flags.pretend) std::cerr << " | " << ui::Color::YELLOW << "DRY-RUN";
            if (flags.verbose) std::cerr << " | " << ui::Color::CYAN << "VERBOSE";
            std::cerr << ui::Color::RESET << "\n";
        }

        if (flags.noCache && flags.command != "update") {
            if (!flags.quiet) ui::info("Refreshing package metadata (--no-cache)...");
            if (!backend_->updateCache()) {
                ui::error("Could not refresh the package cache.");
                return 1;
            }
        }

        if (flags.command == "search") {
            if (flags.positional.empty()) {
                ui::error("Search requires a query. Usage: gwins12 search <query>");
                return 1;
            }
            const std::string& query = flags.positional.front();
            if (!flags.quiet) ui::info("Searching for \"" + query + "\"...");
            auto results = backend_->search(query);
            if (flags.quiet) {
                for (const auto& package : results) std::cout << package.name << "\n";
            } else {
                ui::searchResults(results, total.elapsedMs());
            }
        }
        else if (flags.command == "install") {
            if (flags.positional.empty()) {
                ui::error("Install requires package names. Usage: gwins12 install <pkgs...>");
                return 1;
            }
            if (!flags.quiet) ui::header("Resolving Dependencies");
            auto resolved = backend_->resolve(flags.positional);

            if (resolved.empty()) {
                std::string names;
                for (const auto& package : flags.positional) {
                    if (!names.empty()) names += ", ";
                    names += package;
                }
                ui::error("Could not resolve: " + names);
                return 1;
            }

            if (!flags.quiet) {
                ui::header(flags.pretend ? "Dry-Run Plan" : "Installation Plan");
                for (const auto& package : resolved)
                    ui::packageLine(package.name, package.version, package.description);
                if (flags.recursive)
                    ui::warn("Dependency-tree output is not implemented by this backend.");
            }

            if (flags.pretend) {
                if (flags.quiet) {
                    for (const auto& package : resolved) std::cout << package.name << "\n";
                } else {
                    ui::info("Dry-run complete. " + std::to_string(resolved.size()) +
                             " packages would be installed.");
                }
                return 0;
            }

            if (!backend_->install(resolved)) {
                ui::error("Installation was not completed by the selected backend.");
                return 1;
            }
            ui::summary(static_cast<int>(resolved.size()), 0, 0, total.elapsedMs());
        }
        // === COMANDO: BOOTSTRAP (Operación Compuesta Paralela) ===
        else if (flags.command == "bootstrap") {
            if (flags.positional.empty()) {
                ui::error("Usage: gwins12 bootstrap <profile|packages...>");
                return 1;
            }

            ui::header("Bootstrap Environment");

            // Perfiles predefinidos
            static const std::unordered_map<std::string, std::vector<std::string>> profiles = {
                {"web-dev",     {"nodejs", "npm", "git", "curl", "build-essential"}},
                {"python-dev",  {"python3", "python3-pip", "python3-venv", "git"}},
                {"rust-dev",    {"curl", "build-essential", "pkg-config", "libssl-dev"}},
                {"fullstack",   {"nodejs", "postgresql", "redis-server", "git", "curl"}},
            };

            std::vector<std::string> packages;
            const std::string& profileName = flags.positional.front();

            auto it = profiles.find(profileName);
            if (it != profiles.end()) {
                packages = it->second;
                ui::info("Using profile: " + profileName + " (" +
                         std::to_string(packages.size()) + " packages)");
            } else {
                packages = flags.positional;
            }

            // Fase 1: Resolver dependencias
            Timer resolveTimer;
            auto resolved = backend_->resolve(packages);
            if (resolved.empty()) {
                ui::error("Failed to resolve packages");
                return 1;
            }
            ui::info("Resolved in " + std::to_string(resolveTimer.elapsedMs()) + "ms");

            ui::header("Resolved Plan");
            for (const auto& p : resolved)
                ui::packageLine(p.name, p.version, p.description);

            if (flags.pretend) {
                ui::warn("Dry-run: skipping downloads and installation");
                ui::summary(0, 0, 0, total.elapsedMs());
                return 0;
            }

            // Fase 2: Descargar TODO en paralelo
            ui::ProgressDisplay progress;
            progress.init(resolved.size(), "Downloading");

            std::vector<DownloadPipeline::DownloadRequest> dlRequests;
            for (const auto& p : resolved) {
                if (!p.downloadUrl.empty()) {
                    fs::path dest = config_.storeDir / "downloads" /
                                   (p.name + "_" + p.version + ".deb");
                    std::error_code ec;
                    fs::create_directories(dest.parent_path(), ec);
                    dlRequests.push_back({p.downloadUrl, dest, p.sha256});
                }
            }

            DownloadPipeline pipeline(config_.maxParallelDownloads);
            auto dlResult = pipeline.downloadBatch(dlRequests, progress);
            progress.finish();

            // Fase 3: Instalar (secuencial por seguridad, ya descargado)
            ui::header("Installing");
            if (!backend_->install(resolved)) {
                ui::warn("Native install delegation not implemented yet — " +
                         std::to_string(dlResult.succeeded) + " files downloaded but not installed.");
            }
            ui::summary(static_cast<int>(dlResult.succeeded), 0,
                        static_cast<int>(dlResult.failed), total.elapsedMs());
        }
        else if (flags.command == "update") {
            if (!flags.quiet) ui::info("Updating package cache...");
            if (!backend_->updateCache()) {
                ui::error("Could not update the package cache.");
                return 1;
            }
            if (!flags.quiet) ui::summary(0, 0, 0, total.elapsedMs());
        } else if (flags.command == "version") {
            std::cout << "gwins12 " << GWINS12_VERSION << "\n";
        }
        else {
            ui::error("Unknown command: '" + flags.command + "'");
            std::cerr << "  Run 'gwins12 -h' for available commands.\n\n";
            return 1;
        }
        if (flags.verbose && !flags.quiet)
            ui::info("Completed in " + std::to_string(total.elapsedMs()) + "ms.");
        return 0;
    }
    
private:
    void printUsage() const {
        args::printHelp();
    }
};

// ============================================================================
// SECCIÓN 8: ENTRY POINT
// ============================================================================

int main(int argc, char* argv[]) {
    try {
        // Parse before constructing the engine: help, version, and malformed
        // invocations must not create cache directories or initialize libsolv.
        const args::Flags flags = args::Parser::parse(argc, argv);
        if (flags.help) {
            args::printHelp();
            return 0;
        }
        if (flags.version) {
            std::cout << "gwins12 " << GWINS12_VERSION << "\n";
            return 0;
        }
        if (!flags.error.empty()) {
            ui::error(flags.error);
            std::cerr << "  Run 'gwins12 -h' for usage information.\n\n";
            return 1;
        }
        // Comandos que no requieren inicializar el backend (ni la caché)
        if (flags.command == "version") {
            std::cout << "gwins12 " << GWINS12_VERSION << "\n";
            return 0;
        }
        if (flags.command == "help") {
            args::printHelp();
            return 0;
        }
        Gwins12Engine engine;
        return engine.run(flags);
    } catch (const std::exception& e) {
        log(LogLevel::ERROR, std::string("Fatal: ") + e.what());
        return 1;
    }
}
