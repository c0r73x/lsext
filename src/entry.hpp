#ifndef ENTRY_HPP_
#define ENTRY_HPP_

#define USE_GIT 1

#include <string>
#include <vector>
#include <map>

#include <sys/types.h>

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

#define GIT_DIR_CLEAN 0x00000000
#define GIT_DIR_DIRTY 0x00000001
#define GIT_ISREPO    0x00000010

using DateFormat = std::pair<std::string, std::string>;

struct Color {
    std::string glob;
    std::string color;
};

enum sort_t {
    SORT_ALPHA,
    SORT_MODIFIED,
    SORT_SIZE,
};

enum dateunit_t {
    DATE_SEC = 0,
    DATE_MIN,
    DATE_HOUR,
    DATE_DAY,
    DATE_MON,
    DATE_YEAR,
};

struct settings_t {
    bool resolve_links;
    bool show_hidden;
    bool reversed;
    bool dirs_first;
    bool list;
    bool colors;
    bool size_number_color;
    bool date_number_color;
    sort_t sort;

    struct colors_t {
        struct date_t {
            int number;

            int sec;
            int min;
            int hour;
            int day;
            int mon;
            int year;
            int other;
        } date;

        struct perm_t {
            int none;
            int read;
            int write;
            int exec;

            int dir;
            int link;
            int sticky;
            int special;
            int block;
            int other;
            int unknown;
        } perm;

        struct user_t {
            int user;
            int group;
            int separator;
        } user;

        struct fsize_t {
            int number;

            int byte;
            int kilo;
            int mega;
            int giga;
            int tera;
            int peta;
        } size;

        #ifdef USE_GIT
        struct git_t {
            int ignore;
            int conflict;
            int modified;
            int renamed;
            int added;
            int typechange;
            int unreadable;
            int untracked;
            int unchanged;
            int dir_dirty;
            int dir_clean;
            int repo_dirty;
            int repo_clean;
        } git;
        #endif
    } color;

    struct symbols_t {
        struct user_t {
            std::string separator;
        } user;

        struct date_t {
            std::string sec;
            std::string min;
            std::string hour;
            std::string day;
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
        } git;
        #endif
    } symbols;
};

extern settings_t settings;
extern std::vector<Color> colors;

class Entry
{
public:
    Entry(
        std::string directory,
        const char *file,
        char *fullpath,
        struct stat *st,
        unsigned int flags = 0
    );

    Entry(const Entry &) = default;
    virtual ~Entry() = default;

    std::string file;

    bool isdir;
    bool islink;

    int64_t modified;
    int64_t bsize;

    size_t user_len;

    size_t date_len;
    size_t date_unit_len;

    size_t size_len;
    size_t file_len;

    void list(int max_user, int max_date, int max_date_unit, int max_size);
    void print(int max_len);
private:
    std::string git;
    std::string prefix;
    std::string perms;
    std::string user;
    DateFormat date;
    std::string size;
    std::string suffix;
    std::string target;
    std::string color;
    std::string target_color;

    static char fileTypeLetter(unsigned int mode);
    static char *lsPerms(unsigned int mode);
    static DateFormat timeAgo(int64_t ftime);
    std::string unitConv(float size);
    std::string findColor(const char *file);
    std::string getColor(const char *file, unsigned int mode);
    std::string colorperms(std::string input);

    int fileHasAcl(char const *name, struct stat const *sb);
};

#endif // ENTRY_HPP_
