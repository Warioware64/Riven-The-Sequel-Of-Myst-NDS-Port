#include "riven/FFmpeg.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <signal.h>
#    include <spawn.h>
#    include <sys/wait.h>
#    include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

namespace riven
{
namespace
{
#ifdef _WIN32
    constexpr const char *kExeSuffix = ".exe";
    constexpr char kPathSep = ';';
#else
    constexpr const char *kExeSuffix = "";
    constexpr char kPathSep = ':';
#endif

    /// A unique-enough name for a stderr capture. Not mkstemp: the same routine
    /// has to work on Windows, and this file is only ever written by a child we
    /// just launched, into a directory only this process names.
    fs::path tempErrPath()
    {
        static std::atomic<unsigned> counter{0};
        const unsigned n = counter.fetch_add(1);
        char name[64];
        std::snprintf(name, sizeof(name), "riven-ffmpeg-%llu-%u.log",
                      static_cast<unsigned long long>(
#ifdef _WIN32
                          GetCurrentProcessId()
#else
                          static_cast<unsigned long long>(getpid())
#endif
                              ),
                      n);
        std::error_code ec;
        return fs::temp_directory_path(ec) / name;
    }

    std::string slurp(const fs::path &p)
    {
        std::ifstream in(p, std::ios::binary);
        if (!in)
            return {};
        std::string s((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
        // Long ffmpeg errors are not more useful than short ones, and this ends
        // up inside a one-line progress message.
        constexpr std::size_t kMax = 800;
        if (s.size() > kMax)
            s.resize(kMax);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        return s;
    }

    bool isExecutableFile(const fs::path &p)
    {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec))
            return false;
#ifdef _WIN32
        return true;
#else
        return ::access(p.c_str(), X_OK) == 0;
#endif
    }

    /// Look for `name` on PATH. Empty if it is not there.
    fs::path onPath(const std::string &name)
    {
        const char *path = std::getenv("PATH");
        if (path == nullptr)
            return {};
        const std::string all(path);
        std::size_t at = 0;
        while (at <= all.size())
        {
            const std::size_t end = all.find(kPathSep, at);
            const std::string dir =
                all.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (!dir.empty())
            {
                const fs::path candidate = fs::path(dir) / (name + kExeSuffix);
                if (isExecutableFile(candidate))
                    return candidate;
            }
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
        return {};
    }

#ifdef _WIN32
    /// argv -> a CreateProcess command line, with the quoting rules the CRT
    /// parses back (2N backslashes before a quote become N, and so on).
    std::wstring buildCommandLine(const fs::path &exe,
                                  const std::vector<std::string> &argv)
    {
        const auto quote = [](const std::wstring &arg) {
            if (!arg.empty()
                && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
                return arg;
            std::wstring out = L"\"";
            for (std::size_t i = 0;; ++i)
            {
                std::size_t slashes = 0;
                while (i < arg.size() && arg[i] == L'\\')
                {
                    ++i;
                    ++slashes;
                }
                if (i == arg.size())
                {
                    out.append(slashes * 2, L'\\');
                    break;
                }
                if (arg[i] == L'"')
                {
                    out.append(slashes * 2 + 1, L'\\');
                    out.push_back(L'"');
                }
                else
                {
                    out.append(slashes, L'\\');
                    out.push_back(arg[i]);
                }
            }
            out.push_back(L'"');
            return out;
        };

        std::wstring line = quote(exe.wstring());
        for (const std::string &a : argv)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(),
                                              static_cast<int>(a.size()), nullptr, 0);
            std::wstring w(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, a.c_str(), static_cast<int>(a.size()),
                                w.data(), n);
            line.push_back(L' ');
            line.append(quote(w));
        }
        return line;
    }
#endif
} // namespace

// ---------------------------------------------------------------------------
// Locating the binaries
// ---------------------------------------------------------------------------

fs::path findOnPath(const std::string &name)
{
    return onPath(name);
}

FFmpegPaths findFFmpeg(const fs::path &override)
{
    FFmpegPaths out;
    std::error_code ec;

    if (!override.empty())
    {
        // An explicit path that does not resolve is an ERROR, not a hint. Falling
        // back to PATH here would mean a user who pointed at the wrong ffmpeg got
        // a successful conversion from a different one and no way to tell.
        out.overrideFailed = true;
        out.tried = override;

        if (fs::is_directory(override, ec))
        {
            const fs::path a = override / (std::string("ffmpeg") + kExeSuffix);
            const fs::path b = override / (std::string("ffprobe") + kExeSuffix);
            if (isExecutableFile(a))
            {
                out.ffmpeg = a;
                out.overrideFailed = false;
            }
            if (isExecutableFile(b))
                out.ffprobe = b;
        }
        else if (isExecutableFile(override))
        {
            out.ffmpeg = override;
            out.overrideFailed = false;
            // ffprobe usually lives beside it; a build that ships only ffmpeg
            // falls through to PATH below.
            const fs::path beside =
                override.parent_path() / (std::string("ffprobe") + kExeSuffix);
            if (isExecutableFile(beside))
                out.ffprobe = beside;
        }

        if (out.overrideFailed)
            return out;
    }

    if (out.ffmpeg.empty())
        out.ffmpeg = onPath("ffmpeg");
    if (out.ffprobe.empty())
        out.ffprobe = onPath("ffprobe");
    return out;
}

bool probeFFmpeg(const FFmpegPaths &paths, std::string &version, std::string &error)
{
    version.clear();
    error.clear();

    if (paths.overrideFailed)
    {
        error = "no ffmpeg at " + paths.tried.string();
        return false;
    }
    if (paths.ffmpeg.empty())
    {
        error = "ffmpeg was not found on PATH";
        return false;
    }
    if (paths.ffprobe.empty())
    {
        error = "ffprobe was not found on PATH (it ships with ffmpeg)";
        return false;
    }

    std::string out;
    std::string err;
    if (!runCapture(paths.ffmpeg, {"-hide_banner", "-version"}, out, err))
    {
        error = "ffmpeg will not run: " + err;
        return false;
    }
    version = out.substr(0, out.find('\n'));
    while (!version.empty() && (version.back() == '\r' || version.back() == ' '))
        version.pop_back();

    if (!runCapture(paths.ffprobe, {"-hide_banner", "-version"}, out, err))
    {
        error = "ffprobe will not run: " + err;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Subprocess
// ---------------------------------------------------------------------------

Subprocess::~Subprocess()
{
    kill();
    wait();
    cleanup();
}

void Subprocess::cleanup()
{
    if (!errPath_.empty())
    {
        std::error_code ec;
        fs::remove(errPath_, ec);
        errPath_.clear();
    }
}

#ifdef _WIN32

bool Subprocess::start(const fs::path &exe, const std::vector<std::string> &argv,
                       std::string &error)
{
    kill();
    wait();
    cleanup();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
    {
        error = "could not create a pipe for ffmpeg";
        return false;
    }
    // Only the child gets the write end; leaving it inheritable in the parent is
    // what makes a read block forever after the child exits.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    errPath_ = tempErrPath();
    HANDLE errFile = CreateFileW(errPath_.wstring().c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (errFile == INVALID_HANDLE_VALUE)
    {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        error = "could not create a log file for ffmpeg";
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = writeEnd;
    si.hStdError = errFile;

    auto *pi = new PROCESS_INFORMATION{};
    std::wstring line = buildCommandLine(exe, argv);
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, pi);
    CloseHandle(writeEnd);
    CloseHandle(errFile);
    if (!ok)
    {
        delete pi;
        CloseHandle(readEnd);
        cleanup();
        error = "could not run " + exe.string();
        return false;
    }

    handle_ = pi;
    outHandle_ = readEnd;
    running_ = true;
    exitCode_ = -1;
    return true;
}

std::size_t Subprocess::read(void *dst, std::size_t n)
{
    if (outHandle_ == nullptr || n == 0)
        return 0;
    DWORD got = 0;
    if (!ReadFile(outHandle_, dst, static_cast<DWORD>(n), &got, nullptr))
        return 0;
    return got;
}

void Subprocess::kill()
{
    if (!running_ || handle_ == nullptr)
        return;
    auto *pi = static_cast<PROCESS_INFORMATION *>(handle_);
    TerminateProcess(pi->hProcess, 1);
}

int Subprocess::wait()
{
    if (handle_ == nullptr)
        return exitCode_;

    auto *pi = static_cast<PROCESS_INFORMATION *>(handle_);
    if (outHandle_ != nullptr)
    {
        CloseHandle(outHandle_);
        outHandle_ = nullptr;
    }
    WaitForSingleObject(pi->hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi->hProcess, &code);
    CloseHandle(pi->hProcess);
    CloseHandle(pi->hThread);
    delete pi;
    handle_ = nullptr;
    running_ = false;
    exitCode_ = static_cast<int>(code);
    stderr_ = slurp(errPath_);
    return exitCode_;
}

#else // POSIX

bool Subprocess::start(const fs::path &exe, const std::vector<std::string> &argv,
                       std::string &error)
{
    kill();
    wait();
    cleanup();

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0)
    {
        error = "could not create a pipe for ffmpeg";
        return false;
    }

    errPath_ = tempErrPath();
    const int errFd = ::open(errPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (errFd < 0)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        cleanup();
        error = "could not create a log file for ffmpeg";
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errFd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addclose(&actions, errFd);
    // ffmpeg reads stdin for its interactive keys unless told not to; the
    // callers pass -nostdin, and closing it here means even a build that ignores
    // that cannot eat the CLI's own input.
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    std::vector<std::string> owned;
    owned.reserve(argv.size() + 1);
    owned.push_back(exe.string());
    owned.insert(owned.end(), argv.begin(), argv.end());

    std::vector<char *> cargv;
    cargv.reserve(owned.size() + 1);
    for (std::string &s : owned)
        cargv.push_back(s.data());
    cargv.push_back(nullptr);

    pid_t pid = -1;
    const int rc =
        ::posix_spawn(&pid, exe.c_str(), &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    ::close(errFd);

    if (rc != 0)
    {
        ::close(fds[0]);
        cleanup();
        error = "could not run " + exe.string() + ": " + std::strerror(rc);
        return false;
    }

    handle_ = reinterpret_cast<void *>(static_cast<std::intptr_t>(pid));
    outFd_ = fds[0];
    running_ = true;
    exitCode_ = -1;
    return true;
}

std::size_t Subprocess::read(void *dst, std::size_t n)
{
    if (outFd_ < 0 || n == 0)
        return 0;
    for (;;)
    {
        const ssize_t got = ::read(outFd_, dst, n);
        if (got >= 0)
            return static_cast<std::size_t>(got);
        if (errno == EINTR)
            continue; // a signal, not the end of the movie
        return 0;
    }
}

void Subprocess::kill()
{
    if (!running_ || handle_ == nullptr)
        return;
    const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle_));
    ::kill(pid, SIGKILL);
}

int Subprocess::wait()
{
    if (handle_ == nullptr)
        return exitCode_;

    const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle_));
    if (outFd_ >= 0)
    {
        // Closing the read end first: a killed child may still have a writer
        // blocked, and this is what lets it die.
        ::close(outFd_);
        outFd_ = -1;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    handle_ = nullptr;
    running_ = false;
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    stderr_ = slurp(errPath_);
    return exitCode_;
}

#endif

bool Subprocess::readExact(void *dst, std::size_t n, bool &eof)
{
    eof = false;
    auto *p = static_cast<std::uint8_t *>(dst);
    std::size_t at = 0;
    while (at < n)
    {
        const std::size_t got = read(p + at, n - at);
        if (got == 0)
        {
            // Nothing at all means a clean end of output; a partial frame means
            // the child died mid-picture, which is an error.
            eof = (at == 0);
            return false;
        }
        at += got;
    }
    return true;
}

bool runCapture(const fs::path &exe, const std::vector<std::string> &argv,
                std::string &out, std::string &error)
{
    out.clear();
    error.clear();

    Subprocess p;
    if (!p.start(exe, argv, error))
        return false;

    char buf[4096];
    for (;;)
    {
        const std::size_t got = p.read(buf, sizeof(buf));
        if (got == 0)
            break;
        out.append(buf, got);
    }

    const int code = p.wait();
    if (code != 0)
    {
        error = p.stderrText();
        if (error.empty())
            error = exe.filename().string() + " exited with status "
                  + std::to_string(code);
        return false;
    }
    return true;
}

} // namespace riven
