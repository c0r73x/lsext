// NOLINTNEXTLINE
#ifndef ENTRY_HPP_
#define ENTRY_HPP_

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <vector>

extern "C" {
    #include <iniparser.h>
    #include <sys/stat.h>
    #include <sys/types.h>
}

#define SLK_NORM "no"
#define SLK_FILE "fi"
#define SLK_RESET "rs"
#define SLK_DIR "di"
#define SLK_LNK "ln"
#define SLK_ORPHAN "or"
#define SLK_MISSING "mi"
#define SLK_FIFO "pi"
#define SLK_SOCK "so"
#define SLK_BLK "bd"
#define SLK_CHR "cd"
#define SLK_DOOR "do"
#define SLK_EXEC "ex"
#define SLK_LEFT "lc"
#define SLK_RIGHT "rc"
#define SLK_END "ec"
#define SLK_SUID "su"
#define SLK_SGID "sg"
#define SLK_STICKY "st"
#define SLK_OWR "ow"
#define SLK_OWT "tw"
#define SLK_CAPABILITY "ca"
#define SLK_MULTIHARDLINK "mh"
#define SLK_CLRTOEOL "cl"

#define GIT_DIR_CLEAN 0b0000'0000u
#define GIT_DIR_DIRTY 0b0000'0010u
#define GIT_DIR_BARE  0b0000'0100u
#define GIT_ISREPO    0b0000'1000u

#define NO_FLAGS ~0u

using DateFormat = std::pair<std::string, std::string>;

using Segment = std::pair<std::string, int>;
using OutputFormat = std::unordered_map<char, Segment>;
using Lengths = std::unordered_map<char, int>;

#define SORT_TYPE     0b0000'0001u
#define SORT_ALPHA    0b0000'0010u
#define SORT_MODIFIED 0b0000'0100u
#define SORT_SIZE     0b0000'1000u

enum dateunit_t {
    DATE_SEC = 0,
    DATE_MIN,
    DATE_HOUR,
    DATE_DAY,
    DATE_WEEK,
    DATE_MON,
    DATE_YEAR,
};

struct color_t {
    int fg;
    int bg;
};

struct settings_t { // NOLINT
    bool resolve_links;
    bool resolve_mounts;
    bool resolve_repos;
    bool show_hidden;
    bool reversed;
    bool dirs_first;
    bool list;
    bool colors;
    bool size_number_color;
    bool date_number_color;
    bool numeric_id;

    std::string format;
    std::string list_format;

    #ifdef USE_GIT
    bool override_git_repo_color;
    bool override_git_dir_color;
    #endif

    bool no_conf;

    int forced_columns;

    unsigned char sort;

    struct colors_t { // NOLINT
        struct suffix_t {
            color_t exec;
            color_t dir;
            color_t link;
            color_t mountpoint;
        } suffix;

        struct date_t {
            color_t number;

            color_t sec;
            color_t min;
            color_t hour;
            color_t day;
            color_t week;
            color_t mon;
            color_t year;
            color_t other;
        } date;

        struct perm_t {
            color_t none;
            color_t read;
            color_t write;
            color_t exec;

            color_t full;
            color_t readwrite;
            color_t readexec;
            color_t writeexec;

            color_t dir;
            color_t link;
            color_t sticky;
            color_t special;
            color_t block;
            color_t other;
            color_t unknown;
        } perm;

        struct user_t {
            color_t user;
            color_t group;
            color_t separator;
        } user;

        struct fsize_t {
            color_t number;

            color_t byte;
            color_t kilo;
            color_t mega;
            color_t giga;
            color_t tera;
            color_t peta;
        } size;

        #ifdef USE_GIT
        struct git_t {
            color_t ignore;
            color_t conflict;
            color_t modified;
            color_t renamed;
            color_t added;
            color_t typechange;
            color_t unreadable;
            color_t untracked;
            color_t unchanged;

            color_t dir_dirty;
            color_t dir_clean;

            color_t repo_dirty;
            color_t repo_clean;
            color_t repo_bare;

            std::string o_dir_dirty;
            std::string o_dir_clean;

            std::string o_repo_dirty;
            std::string o_repo_clean;
            std::string o_repo_bare;
        } git;
        #endif
    } color;

    struct symbols_t {
        struct suffix_t {
            std::string exec;
            std::string dir;
            std::string link;
            std::string mountpoint;
        } suffix;

        struct user_t {
            std::string separator;
        } user;

        struct date_t {
            std::string sec;
            std::string min;
            std::string hour;
            std::string day;
            std::string week;
            std::string mon;
            std::string year;
        } date;

        struct fsize_t {
            std::string byte;
            std::string kilo;
            std::string mega;
            std::string giga;
            std::string tera;
            std::string peta;
        } size;

        #ifdef USE_GIT
        struct git_t {
            std::string ignore;
            std::string conflict;
            std::string modified;
            std::string renamed;
            std::string added;
            std::string typechange;
            std::string unreadable;
            std::string untracked;
            std::string unchanged;

            std::string dir_dirty;
            std::string dir_clean;

            std::string repo_dirty;
            std::string repo_clean;
            std::string repo_bare;
        } git;
        #endif
    } symbols;
};

extern settings_t settings;
extern std::unordered_map<std::string, std::string> colors;

class Entry
{
public:
    Entry(
        const std::string &file,
        char *fullpath,
        struct stat *st,
        unsigned int flags
    );

    Entry(const Entry &) = default;
    virtual ~Entry() = default;

    Entry(Entry &&other) = delete;
    Entry &operator=(const Entry &other) = delete;
    Entry &operator=(Entry &&other) = delete;

    std::string file;
    std::string extension;

    bool isdir;
    bool islink;

    time_t modified;
    int64_t bsize;
    uint32_t mode;
    int totlen;

    std::string print(Lengths maxlens, int *outlen);

    OutputFormat processed;
private:
    std::string fullpath;

    std::string user;
    std::string group;
    std::string git;
    std::string target;
    std::string suffix;

    std::string color;
    std::string target_color;

    static char fileTypeLetter(uint32_t mode);
    static DateFormat toDateFormat(const std::string &num, int unit);
    static DateFormat relativeTime(time_t ftime);
    static DateFormat isoTime(time_t ftime);
    static std::string colorize(std::string input, color_t color);
    static std::string colorperms(std::string input);
    static uint32_t cleanlen(std::string input);
    Segment format(char c);

    std::string isMountpoint(char *fullpath, struct stat *st);
    std::string unitConv(float size);
    std::string findColor(const std::string &file);
    std::string getColor(const std::string &file, uint32_t mode);
    std::string lsPerms(uint32_t mode);
    std::string chmodPerms(uint32_t mode);

    char fileHasAcl();

    void postprocess();
};

static inline const char *cpp11_getstring(dictionary *d, const char *key,
        const char *def)
{
    return iniparser_getstring(d, key, const_cast<char *>(def)); // NOLINT
}

static inline bool exists(const char *name)
{
    struct stat buffer = { 0 };
    return (stat(name, &buffer) == 0);
}

static inline std::string rtrim(const std::string &s)
{
    std::string output = s;
    output.erase(std::find_if(output.rbegin(), output.rend(), [](int ch) {
        return (std::isspace(ch) == 0);
    }).base(), output.end());

    return output;
}

static inline bool wildcmp(const char *w, const char *s, uint8_t wl,
                           uint8_t sl)
{
    const char *wp = &w[wl]; // NOLINT
    const char *sp = &s[sl]; // NOLINT

    bool star = false;

loopStart:

    for (; *sp; --sp, --wp, --wl, --sl) { // NOLINT
        switch (*wp) {
            case '*':
                star = true;
                sp = &s[sl], wp = &w[wl]; // NOLINT

                if (wl == 0) { // NOLINT
                    return true;
                }

                goto loopStart;

            default:
                if (*sp != *wp) {
                    goto starCheck;
                }

                break;
        }
    }

    if (*wp == '*') {
        --wp; // NOLINT
        wl -= (wl != 0) ? 1 : 0;
    }

    return (wl == 0);

starCheck:

    if (!star) {
        return false;
    }

    sp--; // NOLINT
    sl -= (sl != 0) ? 1 : 0;
    goto loopStart;
}

template<typename... Args>
static inline std::string fmt(const char* fmt, Args... args)
{
    size_t size = snprintf(nullptr, 0, fmt, args...);
    std::string buf;
    buf.reserve(size + 1);
    buf.resize(size);
    snprintf(&buf[0], size + 1, fmt, args...);
    return buf;
}

#endif // ENTRY_HPP_
