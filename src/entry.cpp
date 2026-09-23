#include "entry.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <gsl-lite.hpp>

extern "C" {
    #include <dirent.h>
    #include <grp.h>
    #include <libgen.h>
    #include <pwd.h>
    #include <sys/stat.h>
    #include <sys/sysmacros.h>
    #include <sys/xattr.h>
    #include <unistd.h>
    #include <stb_sprintf.h>

    #ifdef __linux__
        #include <linux/xattr.h>
    #elif __APPLE__
        #include <sys/types.h>
        #include <sys/acl.h>

        #include <sys/param.h>
        #include <sys/ucred.h>
        #include <sys/mount.h>
    #endif

    #ifdef USE_GIT
        #include <git2.h>
    #endif
}

ParsedFormat parsed_format;
time_t now = 0;

namespace {

// LS_COLORS split by kind so lookups are hash hits instead of a linear
// glob scan over every entry for every file.
struct ColorTable {
    // two letter type keys (di, ln, ...) -> "\033[...m" or "target"
    std::unordered_map<std::string, std::string> types;
    // "*<literal>" patterns keyed by literal suffix
    std::unordered_map<std::string, std::string> suffixes;
    // distinct suffix lengths, longest first (most specific match wins)
    std::vector<size_t> suffix_lens;
    // anything else containing a '*'
    std::vector<std::pair<std::string, std::string>> globs;

    std::string fallback;
};

ColorTable color_table;
const std::string empty_string;

// Colored single permission characters, built once from the settings.
std::array<std::string, 128> perm_chars;

// uid/gid -> colored name, shared by all threads.
std::shared_mutex id_mutex;
std::unordered_map<uid_t, std::string> uid_cache;
std::unordered_map<gid_t, std::string> gid_cache;

#ifdef __linux__
struct MountEntry {
    dev_t dev;
    std::string fsname;
};

std::once_flag mounts_once;
std::vector<MountEntry> mounts;

// "\040" style escapes used by the kernel in mountinfo
std::string unescapeMount(const char *s, size_t len)
{
    std::string out;
    out.reserve(len);

    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 3 < len &&
                s[i + 1] >= '0' && s[i + 1] <= '7') {
            out += static_cast<char>(
                       ((s[i + 1] - '0') << 6) |
                       ((s[i + 2] - '0') << 3) |
                       (s[i + 3] - '0')
                   );
            i += 3;
        } else {
            out += s[i];
        }
    }

    return out;
}

// Parse /proc/self/mountinfo once. It carries the device number of every
// mount, so nothing has to be stat():ed (which can hang on dead network
// mounts) and the table is shared by all entries.
void loadMounts()
{
    FILE *fp = fopen("/proc/self/mountinfo", "re");

    if (fp == nullptr) {
        return;
    }

    char *line = nullptr;
    size_t cap = 0;

    while (getline(&line, &cap, fp) > 0) {
        unsigned int major = 0;
        unsigned int minor = 0;

        if (sscanf(line, "%*u %*u %u:%u", &major, &minor) != 2) {
            continue;
        }

        const char *sep = strstr(line, " - ");

        if (sep == nullptr) {
            continue;
        }

        sep += 3;
        const char *type_end = strchr(sep, ' ');

        if (type_end == nullptr) {
            continue;
        }

        if (strncmp(sep, "autofs", type_end - sep) == 0 &&
                type_end - sep == 6) {
            continue;
        }

        const char *src = type_end + 1;
        const char *src_end = strchr(src, ' ');

        if (src_end == nullptr) {
            src_end = src + strlen(src);
        }

        mounts.push_back({
            makedev(major, minor),
            unescapeMount(src, src_end - src)
        });
    }

    free(line);
    fclose(fp);
}
#endif

inline void appendInt(std::string &out, int value)
{
    char buf[16];
    int len = stbsp_snprintf(&buf[0], sizeof(buf), "%d", value);
    out.append(&buf[0], len);
}

inline void appendEscape(std::string &out, color_t color)
{
    if (color.fg >= 0) {
        out += "\033[38;5;";
        appendInt(out, color.fg);
        out += 'm';
    }

    if (color.bg >= 0) {
        out += "\033[48;5;";
        appendInt(out, color.bg);
        out += 'm';
    }

    if (color.bg < 0 && color.fg < 0) {
        out += "\033[0m";
    }
}

color_t permColor(char c)
{
    switch (c) {
        case 'b':
            return settings.color.perm.block;

        case 'c':
            return settings.color.perm.special;

        case 's':
            return settings.color.perm.sticky;

        case 'l':
            return settings.color.perm.link;

        case 'd':
            return settings.color.perm.dir;

        case '-': case '0':
            return settings.color.perm.none;

        case 'r': case '4':
            return settings.color.perm.read;

        case '7':
            return settings.color.perm.full;

        case '6':
            return settings.color.perm.readwrite;

        case '5':
            return settings.color.perm.readexec;

        case '3':
            return settings.color.perm.writeexec;

        case 'w': case '2':
            return settings.color.perm.write;

        case 'x':
        case 't': case '1':
            return settings.color.perm.exec;

        case '?':
            return settings.color.perm.unknown;

        default:
            return settings.color.perm.other;
    }
}

} // namespace

void parseFormat(const std::string &format)
{
    parsed_format = ParsedFormat();

    for (size_t i = 0; i < format.length(); i++) {
        FormatToken token = { format[i], true, false, -1 };

        if (format[i] == '@' && i + 1 < format.length()) {
            i++;

            if (format[i] == '^' && i + 1 < format.length()) {
                token.right = true;
                i++;
            }

            token.c = format[i];

            if (token.c != '@') {
                size_t slot = parsed_format.slots.find(token.c);

                if (slot == std::string::npos) {
                    slot = parsed_format.slots.length();
                    parsed_format.slots += token.c;
                }

                token.literal = false;
                token.slot = static_cast<int>(slot);
            }
        }

        if (token.literal) {
            parsed_format.literal_len++;
        }

        parsed_format.tokens.push_back(token);
    }
}

void initColors()
{
    const char *ls_colors = std::getenv("LS_COLORS");

    if (ls_colors == nullptr) {
        ls_colors = "";
    }

    std::string_view rest(ls_colors);

    while (!rest.empty()) {
        size_t end = rest.find(':');
        std::string_view token = rest.substr(0, end);
        rest = (end == std::string_view::npos) ?
               std::string_view() : rest.substr(end + 1);

        size_t pos = token.find('=');

        if (pos == std::string_view::npos || pos == 0) {
            continue;
        }

        std::string key(token.substr(0, pos));
        std::string_view value = token.substr(pos + 1);
        std::string esc = (value == "target") ?
                          std::string(value) :
                          "\033[" + std::string(value) + "m";

        if (key[0] == '*' &&
                key.find('*', 1) == std::string::npos) {
            color_table.suffixes[key.substr(1)] = esc;
        } else if (key.find('*') != std::string::npos) {
            color_table.globs.emplace_back(key, esc);
        } else {
            color_table.types[key] = esc;
        }
    }

    for (const auto &s : color_table.suffixes) {
        if (std::find(
                    color_table.suffix_lens.begin(),
                    color_table.suffix_lens.end(),
                    s.first.length()
                ) == color_table.suffix_lens.end()) {
            color_table.suffix_lens.push_back(s.first.length());
        }
    }

    std::sort(
        color_table.suffix_lens.begin(),
        color_table.suffix_lens.end(),
        std::greater<>()
    );

    auto fi = color_table.types.find(SLK_FILE);

    color_table.fallback = (fi != color_table.types.end()) ?
                           fi->second : "\033[0m";
}

void initTables()
{
    for (size_t c = 0; c < perm_chars.size(); c++) {
        perm_chars.at(c) = Entry::colorize(
                               std::string(1, static_cast<char>(c)),
                               permColor(static_cast<char>(c))
                           );
    }
}

std::string Entry::colorize(std::string_view input, color_t color)
{
    if (settings.colors) {
        std::string output;
        output.reserve(input.length() + 32);

        appendEscape(output, color);
        output += input;

        if (color.bg >= 0 && color.fg >= 0) {
            output += "\033[0m";
        }

        return output;
    }

    return std::string(input);
}

// Visible length: escape sequences are skipped and each UTF-8 code point
// counts as one column.
uint32_t Entry::cleanlen(std::string_view input)
{
    uint32_t len = 0;
    size_t i = 0;
    const size_t n = input.length();

    while (i < n) {
        auto c = static_cast<unsigned char>(input[i]);

        if (c == '\033') {
            size_t j = i + 1;

            if (j < n && input[j] == '[') {
                j++;
            }

            while (j < n && ((input[j] >= '0' && input[j] <= '9') ||
                             input[j] == ';' || input[j] == ':')) {
                j++;
            }

            if (j < n && input[j] == 'm') {
                i = j + 1;
                continue;
            }
        }

        if ((c & 0xC0u) != 0x80u) {
            len++;
        }

        i++;
    }

    return len;
}

std::string Entry::isMountpoint(const struct stat *st,
                                const struct stat *parent)
{
    if (settings.resolve_mounts && settings.list) {
        struct stat pst = {0};

        if (parent == nullptr) {
            std::string copy = fullpath;

            if (stat(dirname(&copy[0]), &pst) != 0) {
                parent = nullptr;
            } else {
                parent = &pst;
            }
        }

        if (parent != nullptr &&
                (st->st_dev != parent->st_dev ||
                 st->st_ino == parent->st_ino)) {
            #ifdef __linux__

            std::call_once(mounts_once, loadMounts);

            for (const auto &mnt : mounts) {
                if (mnt.dev != st->st_dev) {
                    continue;
                }

                struct stat target = {0};

                this->islink = true;
                this->target = mnt.fsname;

                if (stat(mnt.fsname.c_str(), &target) == 0) {
                    this->target_color = getColor(mnt.fsname, target.st_mode);
                } else {
                    this->target_color = findColor(SLK_CHR);
                }

                return colorize(
                           settings.symbols.suffix.mountpoint,
                           settings.color.suffix.mountpoint
                       );
            }

            #elif __APPLE__

            struct statfs *mnts;
            int num = getmntinfo(&mnts, MNT_NOWAIT);

            for (int i = 0; i < num; i++) {
                if (st->st_dev == mnts[i].f_fsid.val[0]) {
                    struct stat target = {0};

                    this->islink = true;
                    this->target = mnts[i].f_mntfromname;

                    if (stat(mnts[i].f_mntfromname, &target) == 0) {
                        this->target_color = getColor(
                                                 mnts[i].f_mntfromname,
                                                 target.st_mode
                                             );
                    } else {
                        this->target_color = findColor(SLK_CHR);
                    }

                    return colorize(
                               settings.symbols.suffix.mountpoint,
                               settings.color.suffix.mountpoint
                           );
                }
            }

            #endif
        }
    }

    return colorize(
               settings.symbols.suffix.dir,
               settings.color.suffix.dir
           );
}

static std::string resolveId(unsigned int id, bool isgroup)
{
    auto &cache = isgroup ? gid_cache : uid_cache;
    color_t color = isgroup ? settings.color.user.group :
                    settings.color.user.user;

    {
        std::shared_lock<std::shared_mutex> lock(id_mutex);
        auto it = cache.find(id);

        if (it != cache.end()) {
            return it->second;
        }
    }

    char buf[PATH_MAX] = {0};
    const char *name = nullptr;

    struct passwd pw = {};
    struct passwd *pwp = nullptr;
    struct group gr = {};
    struct group *grp = nullptr;

    if (!settings.numeric_id) {
        if (isgroup) {
            if (getgrgid_r(id, &gr, &buf[0], sizeof(buf), &grp) == 0 &&
                    grp != nullptr && grp->gr_name != nullptr &&
                    grp->gr_name[0] != '\0') {
                name = grp->gr_name;
            }
        } else {
            if (getpwuid_r(id, &pw, &buf[0], sizeof(buf), &pwp) == 0 &&
                    pwp != nullptr && pwp->pw_name != nullptr &&
                    pwp->pw_name[0] != '\0') {
                name = pwp->pw_name;
            }
        }
    }

    char idbuf[16] = {0};

    if (name == nullptr) {
        stbsp_snprintf(&idbuf[0], sizeof(idbuf), "%u", id);
        name = &idbuf[0];
    }

    std::string result = Entry::colorize(name, color);

    std::unique_lock<std::shared_mutex> lock(id_mutex);
    cache.emplace(id, result);
    return result;
}

Entry::Entry(
    const std::string &file,
    const char *fullpath,
    const struct stat *st,
    const struct stat *parent
) :
    file(file),
    fullpath(fullpath),
    git(1, ' '), // NOLINT
    suffix(1, ' ') // NOLINT
{
    this->islink = false;
    this->totlen = 0;
    this->isdir = false;

    if (st == nullptr) {
        this->user = colorize("????", settings.color.user.user); // NOLINT
        this->group = colorize("????", settings.color.user.group); // NOLINT
        this->mode = 0;
        this->modified = 0;
        this->bsize = 0;

        this->color = findColor(SLK_ORPHAN);
    } else {
        this->color = getColor(file, st->st_mode);

        #ifdef S_ISLNK

        if (S_ISLNK(st->st_mode) && !settings.resolve_links) { // NOLINT
            char target[PATH_MAX];
            ssize_t len = readlink(fullpath, &target[0], sizeof(target) - 1);

            if (len >= 0) {
                target[len] = '\0';

                if (settings.list) {
                    this->suffix = colorize(
                                       settings.symbols.suffix.link,
                                       settings.color.suffix.link
                                   );
                }

                this->islink = true;
                this->target.assign(&target[0], len);

                std::string fpath;

                if (target[0] != '/') {
                    std::string copy = this->fullpath;
                    // NOLINTNEXTLINE
                    fpath = std::string(dirname(&copy[0])) + "/" + this->target;
                } else {
                    fpath = this->target;
                }

                struct stat tst = {0};

                if ((lstat(fpath.c_str(), &tst)) < 0) {
                    this->color = findColor(SLK_ORPHAN);
                    this->target_color = findColor(SLK_MISSING);
                } else {
                    this->color = getColor(file, tst.st_mode);
                    this->target_color = getColor(this->target, tst.st_mode);
                }
            }
        }

        #endif /* S_ISLNK */

        if (parsed_format.uses('u') || parsed_format.uses('U')) {
            this->user = resolveId(st->st_uid, false);
        }

        if (parsed_format.uses('g') || parsed_format.uses('U')) {
            this->group = resolveId(st->st_gid, true);
        }

        this->modified = st->st_mtime;
        this->bsize = st->st_size;
        this->mode = st->st_mode;

        if (S_ISDIR(st->st_mode)) { // NOLINT
            this->isdir = true;
            this->suffix = isMountpoint(st, parent);
        } else if ((st->st_mode & S_IEXEC) != 0 && !islink) { // NOLINT
            this->suffix = colorize(
                               settings.symbols.suffix.exec,
                               settings.color.suffix.exec
                           );
        }
    }

    if ((settings.sort & SORT_TYPE) == SORT_TYPE) {
        if (isdir) {
            extension = "directory";
        } else {
            std::string::size_type idx = file.rfind('.');

            if (idx != std::string::npos) {
                extension = file.substr(idx + 1);
            } else {
                extension = "unknown";
            }
        }
    }

    if (settings.colors) {
        this->file += "\033[0m";
    }
}

void Entry::setGit(unsigned int flags)
{
    #ifdef USE_GIT

    if (flags == NO_FLAGS ||
            !(settings.resolve_repos || settings.resolve_in_repos)) {
        return;
    }

    const std::string *symbol = nullptr;
    color_t color = {0};

    if (S_ISDIR(mode)) { // NOLINT
        if ((flags & GIT_ISREPO) != 0) {
            if ((flags & GIT_DIR_DIRTY) != 0) {
                color = settings.color.git.repo_dirty;
                symbol = &settings.symbols.git.repo_dirty;
            } else if ((flags & GIT_DIR_BARE) != 0) {
                color = settings.color.git.repo_bare;
                symbol = &settings.symbols.git.repo_bare;
            } else {
                color = settings.color.git.repo_clean;
                symbol = &settings.symbols.git.repo_clean;
            }

            if (settings.override_git_repo_color) {
                this->color = colorize(*symbol, color);
            } else {
                this->git = colorize(*symbol, color);
            }
        } else {
            if ((flags & GIT_DIR_DIRTY) != 0) {
                color = settings.color.git.dir_dirty;
                symbol = &settings.symbols.git.dir_dirty;
            } else if ((flags & GIT_STATUS_IGNORED) != 0) {
                color = settings.color.git.ignore;
                symbol = &settings.symbols.git.ignore;
            } else if ((flags & GIT_ISTRACKED) != 0) {
                color = settings.color.git.dir_clean;
                symbol = &settings.symbols.git.dir_clean;
            } else {
                color = settings.color.git.untracked;
                symbol = &settings.symbols.git.untracked;
            }

            if (settings.override_git_dir_color) {
                this->color = colorize(*symbol + file, color);
            } else {
                this->git = colorize(*symbol, color);
            }
        }
    } else {
        if ((flags & GIT_STATUS_IGNORED) != 0) {
            color = settings.color.git.ignore;
            symbol = &settings.symbols.git.ignore;
        } else if ((flags & GIT_STATUS_CONFLICTED) != 0) {
            color = settings.color.git.conflict;
            symbol = &settings.symbols.git.conflict;
        } else if ((flags & GIT_STATUS_WT_MODIFIED) != 0) {
            color = settings.color.git.modified;
            symbol = &settings.symbols.git.modified;
        } else if ((flags & GIT_STATUS_WT_RENAMED) != 0) {
            color = settings.color.git.renamed;
            symbol = &settings.symbols.git.renamed;
        } else if ((flags & GIT_STATUS_INDEX_NEW) != 0) {
            color = settings.color.git.added;
            symbol = &settings.symbols.git.added;
        } else if ((flags & GIT_STATUS_WT_TYPECHANGE) != 0) {
            color = settings.color.git.typechange;
            symbol = &settings.symbols.git.typechange;
        } else if ((flags & GIT_STATUS_WT_UNREADABLE) != 0) {
            color = settings.color.git.unreadable;
            symbol = &settings.symbols.git.unreadable;
        } else if ((flags & GIT_ISTRACKED) != 0) {
            color = settings.color.git.unchanged;
            symbol = &settings.symbols.git.unchanged;
        } else {
            color = settings.color.git.untracked;
            symbol = &settings.symbols.git.untracked;
        }

        this->git = colorize(*symbol, color);
    }

    #else
    (void)flags;
    #endif
}

std::string Entry::colorperms(std::string_view input)
{
    std::string output;
    output.reserve(input.length() * 16);

    for (auto c : input) {
        auto idx = static_cast<unsigned char>(c);

        if (idx < perm_chars.size()) {
            output += perm_chars.at(idx);
        } else {
            output += colorize(std::string_view(&c, 1), permColor(c));
        }
    }

    return output;
}

std::string Entry::format(char c, DateFormat *rel, DateFormat *iso)
{
    std::string output;

    switch (c) {
        case 'p': {
            output = lsPerms(mode);
            break;
        }

        case 'P': {
            output = chmodPerms(mode);
            break;
        }

        case 'u': {
            output = user;
            break;
        }

        case 'g': {
            output = group;
            break;
        }

        case 'U': {
            output = user + colorize(
                         settings.symbols.user.separator,
                         settings.color.user.separator
                     ) + group;
            break;
        }

        case 'r':
        case 't': {
            if (rel->first.empty() && rel->second.empty()) {
                *rel = relativeTime(modified);
            }

            output = (c == 'r') ? rel->first : rel->second;
            break;
        }

        case 'D':
        case 'T': {
            if (iso->first.empty() && iso->second.empty()) {
                *iso = isoTime(modified);
            }

            output = (c == 'D') ? iso->first : iso->second;
            break;
        }

        case 's': {
            output = unitConv(static_cast<float>(bsize));
            break;
        }

        case 'G': {
            #ifdef USE_GIT
            if (settings.resolve_repos || settings.resolve_in_repos) {
                output = git;
            }
            #endif
            break;
        }

        case 'f': {
            output.reserve(color.length() + file.length() + suffix.length() +
                           target_color.length() + target.length() + 4);
            output += color;
            output += file;
            output += suffix;
            output += target_color;
            output += target;
            break;
        }

        case 'F': {
            output.reserve(color.length() + file.length() +
                           suffix.length() + 4);
            output += color;
            output += file;
            output += suffix;
            break;
        }

        default: {
            output = std::string(1, c); // NOLINT
        }
    }

    if (settings.colors) {
        output += "\033[0m";
    }

    return output;
}

void Entry::postprocess()
{
    DateFormat rel;
    DateFormat iso;

    processed.resize(parsed_format.slots.length());

    for (size_t slot = 0; slot < parsed_format.slots.length(); slot++) {
        auto &seg = processed[slot];
        seg.first = format(parsed_format.slots[slot], &rel, &iso);
        seg.second = static_cast<int>(cleanlen(seg.first));
    }

    totlen = parsed_format.literal_len;

    for (const auto &token : parsed_format.tokens) {
        if (!token.literal) {
            totlen += processed[token.slot].second;
        }
    }
}

void Entry::print(std::string &output, const Lengths &maxlens) const
{
    for (const auto &token : parsed_format.tokens) {
        if (token.literal) {
            output += token.c;
            continue;
        }

        const auto &s = processed[token.slot];
        int pad = maxlens[token.slot] - s.second;

        if (token.right) {
            output.append(std::max(pad, 0), ' ');
            output += s.first;
        } else {
            output += s.first;
            output.append(std::max(pad, 0), ' ');
        }
    }
}

// Color for a LS_COLORS type key (di, ln, or, ...)
const std::string &Entry::findColor(const char *type)
{
    if (!settings.colors) {
        return empty_string;
    }

    auto c = color_table.types.find(type);

    if (c != color_table.types.end() && c->second != "target") {
        return c->second;
    }

    return color_table.fallback;
}

// Color for a file name from the LS_COLORS glob patterns
const std::string &Entry::findNameColor(const std::string &file)
{
    if (!settings.colors) {
        return empty_string;
    }

    // Common case, "*.ext" style patterns: one hash lookup per distinct
    // suffix length instead of a glob match per LS_COLORS entry.
    for (size_t len : color_table.suffix_lens) {
        if (len > file.length()) {
            continue;
        }

        auto c = color_table.suffixes.find(file.substr(file.length() - len));

        if (c != color_table.suffixes.end() && c->second != "target") {
            return c->second;
        }
    }

    for (const auto &g : color_table.globs) {
        if (g.second != "target" && wildcmp(g.first.c_str(), file.c_str())) {
            return g.second;
        }
    }

    return color_table.fallback;
}

const std::string &Entry::getColor(const std::string &file, uint32_t mode)
{
    if ((mode & S_ISUID) != 0) { // NOLINT
        return findColor(SLK_SUID);
    }

    if ((mode & S_ISGID) != 0) { // NOLINT
        return findColor(SLK_SGID);
    }

    if ((mode & S_ISVTX) != 0) { // NOLINT
        return findColor((mode & S_IWOTH) ? SLK_OWT : SLK_STICKY); // NOLINT
    }

    if (S_ISDIR(mode)) { // NOLINT
        return findColor((mode & S_IWOTH) ? SLK_OWR : SLK_DIR); // NOLINT
    }

    if (S_ISBLK(mode)) { // NOLINT
        return findColor(SLK_BLK);
    }

    if (S_ISCHR(mode)) { // NOLINT
        return findColor(SLK_CHR);
    }

    #ifdef S_ISFIFO

    if (S_ISFIFO(mode)) { // NOLINT
        return findColor(SLK_FIFO);
    }

    #endif /* S_ISFIFO */

    #ifdef S_ISLNK

    if (S_ISLNK(mode)) { // NOLINT
        return findColor(SLK_LNK);
    }

    #endif /* S_ISLNK */

    #ifdef S_ISSOCK

    if (S_ISSOCK(mode)) { // NOLINT
        return findColor(SLK_SOCK);
    }

    #endif /* S_ISSOCK */

    #ifdef S_ISDOOR /* Solaris 2.6, etc. */

    if (S_ISDOOR(mode)) { // NOLINT
        return findColor(SLK_DOOR);
    }

    #endif /* S_ISDOOR */

    return findNameColor(file);
}

char Entry::fileTypeLetter(uint32_t mode)
{
    if (S_ISREG(mode)) { // NOLINT
        return '-';
    }

    if (S_ISDIR(mode)) { // NOLINT
        return 'd';
    }

    if (S_ISBLK(mode)) { // NOLINT
        return 'b';
    }

    if (S_ISCHR(mode)) { // NOLINT
        return 'c';
    }

    #ifdef S_ISFIFO

    if (S_ISFIFO(mode)) { // NOLINT
        return 'p';
    }

    #endif /* S_ISFIFO */

    #ifdef S_ISLNK

    if (S_ISLNK(mode)) { // NOLINT
        return 'l';
    }

    #endif /* S_ISLNK */

    #ifdef S_ISSOCK

    if (S_ISSOCK(mode)) { // NOLINT
        return 's';
    }

    #endif /* S_ISSOCK */

    #ifdef S_ISDOOR /* Solaris 2.6, etc. */

    if (S_ISDOOR(mode)) { // NOLINT
        return 'D';
    }

    #endif /* S_ISDOOR */

    return '?';
}

char Entry::fileHasAcl()
{
    #ifdef S_ISLNK

    if (S_ISLNK(mode)) { // NOLINT
        return ' ';
    }

    #endif

    #ifdef __linux__

    ssize_t xattr = getxattr(
                fullpath.c_str(),
                XATTR_NAME_POSIX_ACL_ACCESS,
                nullptr,
                0
            );

    if (xattr > 0) {
        return '+';
    }

    // ENOTSUP: no ACL support on this filesystem, skip the second call
    if (xattr < 0 && errno != ENODATA) {
        return ' ';
    }

    if (S_ISDIR(mode)) { // NOLINT
        xattr = getxattr(
                    fullpath.c_str(),
                    XATTR_NAME_POSIX_ACL_DEFAULT,
                    nullptr,
                    0
                );

        if (xattr > 0) {
            return '+';
        }
    }

    #elif __APPLE__

    acl_entry_t dummy;
    acl_t acl = acl_get_link_np(fullpath.c_str(), ACL_TYPE_EXTENDED);

    if (acl && acl_get_entry(acl, ACL_FIRST_ENTRY, &dummy) == -1) {
        acl_free(acl);
        acl = nullptr;
    }

    ssize_t xattr = listxattr(fullpath.c_str(), NULL, 0, XATTR_NOFOLLOW);

    if (xattr < 0) {
        xattr = 0;
    }

    if (xattr > 0) {
        return '@';
    }

    if (acl != NULL) {
        acl_free(acl);
        return '+';
    }

    #endif

    return ' ';
}

std::string Entry::chmodPerms(uint32_t mode)
{
    const char sbits[3] = {
        static_cast<char>('0' + ((mode >> 6u) & 7u)),
        static_cast<char>('0' + ((mode >> 3u) & 7u)),
        static_cast<char>('0' + (mode & 7u)),
    };

    return colorperms(std::string_view(&sbits[0], 3));
}

std::string Entry::lsPerms(uint32_t mode)
{
    static const char *rwx[] = {
        "---",
        "--x",
        "-w-",
        "-wx",
        "r--",
        "r-x",
        "rw-",
        "rwx"
    };

    char bits[11] = {0};

    bits[0] = static_cast<char>(fileTypeLetter(mode));

    memcpy(&bits[1], gsl::at(rwx, (mode >> 6u) & 7u), 3u);
    memcpy(&bits[4], gsl::at(rwx, (mode >> 3u) & 7u), 3u);
    memcpy(&bits[7], gsl::at(rwx, (mode & 7u)), 3u);

    if ((mode & S_ISUID) != 0) { // NOLINT
        bits[3] = (mode & S_IXUSR) != 0 ? 's' : 'S'; // NOLINT
    }

    if ((mode & S_ISGID) != 0) { // NOLINT
        bits[6] = (mode & S_IXGRP) != 0 ? 's' : 'l'; // NOLINT
    }

    if ((mode & S_ISVTX) != 0) { // NOLINT
        bits[9] = (mode & S_IXOTH) != 0 ? 't' : 'T'; // NOLINT
    }

    bits[10] = fileHasAcl();
    return colorperms(std::string_view(&bits[0], 11));
}

std::string Entry::unitConv(float size)
{
    const std::string *units[] = {
        &settings.symbols.size.byte,
        &settings.symbols.size.kilo,
        &settings.symbols.size.mega,
        &settings.symbols.size.giga,
        &settings.symbols.size.tera,
        &settings.symbols.size.peta,
    };

    const color_t colors[] = {
        settings.color.size.byte,
        settings.color.size.kilo,
        settings.color.size.mega,
        settings.color.size.giga,
        settings.color.size.tera,
        settings.color.size.peta,
    };

    char csize[32] = {0};

    for (size_t i = 0; i < std::size(units); i++) {
        if ((size / 1024) <= 1.f) {
            color_t c_symbol = {0};
            color_t c_unit = {0};

            if (!settings.size_number_color) {
                c_unit = gsl::at(colors, i);
                c_symbol = c_unit;
            } else {
                c_symbol = gsl::at(colors, i);
                c_unit = settings.color.size.number;
            }

            if (static_cast<int>(size * 10) % 10 == 0) {
                // NOLINTNEXTLINE
                stbsp_snprintf(&csize[0], sizeof(csize), "%d", static_cast<int>(size));
            } else {
                // NOLINTNEXTLINE
                stbsp_snprintf(&csize[0], sizeof(csize), "%.1f", size);
            }

            std::string unit = colorize(&csize[0], c_unit); // NOLINT
            unit += colorize(*gsl::at(units, i), c_symbol);
            return unit;
        }

        size /= 1024;
    }

    stbsp_snprintf(&csize[0], sizeof(csize), "%.2g?", size); // NOLINT
    return &csize[0]; // NOLINT
}

DateFormat Entry::toDateFormat(const std::string &num, int unit)
{
    color_t c_symbol = {0};
    color_t c_unit = {0};

    const std::string *units[] = {
        &settings.symbols.date.sec,
        &settings.symbols.date.min,
        &settings.symbols.date.hour,
        &settings.symbols.date.day,
        &settings.symbols.date.week,
        &settings.symbols.date.mon,
        &settings.symbols.date.year,
    };

    const color_t colors[] = {
        settings.color.date.sec,
        settings.color.date.min,
        settings.color.date.hour,
        settings.color.date.day,
        settings.color.date.week,
        settings.color.date.mon,
        settings.color.date.year,
    };

    if (!settings.date_number_color) {
        c_unit = gsl::at(colors, unit);
        c_symbol = c_unit;
    } else {
        c_symbol = gsl::at(colors, unit);
        c_unit = settings.color.date.number;
    }

    return DateFormat(
               colorize(num, c_unit),
               colorize(*gsl::at(units, unit), c_symbol) // NOLINT
           );
}

DateFormat Entry::isoTime(time_t ftime)
{
    DateFormat output;
    struct tm tm = {};
    char buf[32];

    localtime_r(&ftime, &tm);

    stbsp_snprintf(&buf[0], sizeof(buf), "%d-%02d-%02d",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    output.first = colorize(&buf[0], settings.color.date.year);

    auto color = settings.color.date.number;
    if (!settings.date_number_color) {
        color = settings.color.date.year;
    }

    stbsp_snprintf(&buf[0], sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    output.second = colorize(&buf[0], color);

    return output;
}

DateFormat Entry::relativeTime(time_t ftime)
{
    int64_t delta = now - ftime;
    int64_t rel = delta;

    if (ftime == 0) {
        return DateFormat("?", "?"); // NOLINT
    }

    if (delta < 10) {
        return toDateFormat("<", DATE_SEC); // NOLINT
    }

    if (delta < 45) {
        return toDateFormat(std::to_string(rel), DATE_SEC);
    }

    rel /= 60;

    if (delta < 60) {
        return toDateFormat("<", DATE_MIN); // NOLINT
    }

    if (delta < 2700) {
        return toDateFormat(std::to_string(rel), DATE_MIN);
    }

    rel /= 60;

    if (delta < 3600) {
        return toDateFormat("<", DATE_HOUR); // NOLINT
    }

    if (delta < 64800) {
        return toDateFormat(std::to_string(rel), DATE_HOUR);
    }

    rel /= 24;

    if (delta < 86400) {
        return toDateFormat("<", DATE_DAY); // NOLINT
    }

    if (delta < 453600) {
        return toDateFormat(std::to_string(rel), DATE_DAY);
    }

    rel /= 7;

    if (delta < 604800) {
        return toDateFormat("<", DATE_WEEK); // NOLINT
    }

    if (delta < 1814400) {
        return toDateFormat(std::to_string(rel), DATE_WEEK);
    }

    rel /= 4;

    if (delta < 2419200) {
        return toDateFormat("<", DATE_MON); // NOLINT
    }

    if (delta < 29030400) {
        return toDateFormat(std::to_string(rel), DATE_MON);
    }

    rel /= 12;

    return toDateFormat(std::to_string(rel), DATE_YEAR);
}
