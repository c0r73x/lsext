// NOLINTNEXTLINE
#ifndef ENTRY_HPP_
#define ENTRY_HPP_

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <iniparser/iniparser.h>
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

// Kept above the git_status_t bits so they can be OR:ed with a status
// without colliding (git_status_t uses bits 0-15).
#define GIT_DIR_CLEAN 0u
#define GIT_DIR_DIRTY (1u << 24u)
#define GIT_DIR_BARE  (1u << 25u)
#define GIT_ISREPO    (1u << 26u)
#define GIT_ISTRACKED (1u << 27u)
#define GIT_ISSUBMODULE (1u << 28u)

#define NO_FLAGS ~0u

using DateFormat = std::pair<std::string, std::string>;

using Segment = std::pair<std::string, int>;
using OutputFormat = std::vector<Segment>; // indexed by format slot
using Lengths = std::vector<int>;          // indexed by format slot

#define SORT_TYPE     1
#define SORT_ALPHA    2
#define SORT_MODIFIED 4
#define SORT_SIZE     8

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
    bool resolve_in_repos;
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

// The format string parsed once instead of once per entry.
struct FormatToken {
    char c;       // literal character or format specifier
    bool literal;
    bool right;   // @^x, right aligned
    int slot;     // index into Entry::processed / Lengths
};

struct ParsedFormat {
    std::vector<FormatToken> tokens;
    std::string slots;   // one specifier char per slot
    int literal_len = 0; // visible length of all literal characters

    bool uses(char c) const
    {
        return slots.find(c) != std::string::npos;
    }
};

extern ParsedFormat parsed_format;
extern time_t now;

void parseFormat(const std::string &format);
void initColors();
void initTables();

class Entry
{
public:
    Entry(
        const std::string &file,
        const char *fullpath,
        const struct stat *st,
        const struct stat *parent
    );

    Entry(const Entry &) = delete;
    Entry(Entry &&other) = delete;
    Entry &operator=(const Entry &other) = delete;
    Entry &operator=(Entry &&other) = delete;
    ~Entry() = default;

    std::string file;
    std::string extension;

    bool isdir;
    bool islink;

    time_t modified;
    int64_t bsize;
    uint32_t mode;
    int totlen;

    void setGit(unsigned int flags);
    void postprocess();
    void print(std::string &output, const Lengths &maxlens) const;

    static std::string colorize(std::string_view input, color_t color);

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
    static std::string colorperms(std::string_view input);
    static uint32_t cleanlen(std::string_view input);
    std::string format(char c, DateFormat *rel, DateFormat *iso);

    std::string isMountpoint(const struct stat *st, const struct stat *parent);
    static std::string unitConv(float size);
    static const std::string &findColor(const char *type);
    static const std::string &findNameColor(const std::string &file);
    static const std::string &getColor(const std::string &file, uint32_t mode);
    std::string lsPerms(uint32_t mode);
    static std::string chmodPerms(uint32_t mode);

    char fileHasAcl();
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

// Glob match supporting '*', iterative with single backtrack point.
static inline bool wildcmp(const char *w, const char *s)
{
    const char *star = nullptr;
    const char *ss = s;

    while (*s != '\0') {
        if (*w == '*') {
            star = w++;
            ss = s;
        } else if (*w == *s) {
            w++;
            s++;
        } else if (star != nullptr) {
            w = star + 1;
            s = ++ss;
        } else {
            return false;
        }
    }

    while (*w == '*') {
        w++;
    }

    return *w == '\0';
}

#endif // ENTRY_HPP_
