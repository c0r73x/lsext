#include "entry.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unordered_map>

#include <pcrecpp.h>

#include <dirent.h>
#include <grp.h>
#include <libgen.h>
#include <linux/xattr.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <wordexp.h>

#include "gsl-lite.h"

#ifdef USE_GIT
    #include <git2.h>
#endif

std::unordered_map<std::string, std::string> colors;

std::unordered_map<uint8_t, std::string> uid_cache;
std::unordered_map<uint8_t, std::string> gid_cache;

bool wildcmp(const char *w, const char *s, uint8_t wl, uint8_t sl)
{
    const char *wp = &w[wl]; // NOLINT
    const char *sp = &s[sl]; // NOLINT

    bool star = false;

loopStart:

    for (; *sp; --sp, --wp, --wl, --sl) { // NOLINT
        switch (*wp) {
            case '*':
                star = true;
                s = &sp[sl], wp = &w[wl]; // NOLINT

                if (*--w == 0) { // NOLINT
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
    }

    return (*wp == 0);

starCheck:

    if (!star) {
        return false;
    }

    s--; // NOLINT
    goto loopStart;
}

std::string Entry::colorize(std::string input, color_t color, bool ending)
{
    if (settings.colors) {
        std::string output;

        if (color.fg >= 0) {
            output = "\033[38;5;" + std::to_string(color.fg) + "m";
        }

        if (color.bg >= 0) {
            output = "\033[48;5;" + std::to_string(color.bg) + "m";
        }

        if (color.bg < 0 && color.fg < 0) {
            output = "\033[0m";
        }

        output += input;

        if (ending) {
            output += "\033[0m";
        }

        return output;
    }

    return input;
}

uint32_t Entry::cleanlen(std::string input)
{
    static pcrecpp::RE esc_re("\033\\[?[;:0-9]*m");
    static pcrecpp::RE uni_re("[\u0080-\uffff]+");

    if (esc_re.GlobalReplace("", &input) != 0) {
        uni_re.GlobalReplace(" ", &input);

        return input.length();
    }

    uni_re.GlobalReplace(" ", &input);
    return input.length();
}

Entry::Entry(const char *file, char *fullpath, struct stat *st,
             uint32_t flags)
{
    this->islink = false;
    this->color = "";
    this->git = ' ';

    if (st == nullptr) {
        this->user = '?';
        this->date = DateFormat("?", "?");
        this->size = '?';
        this->perms = "l?????????";
        this->file = file;
        this->user_len = 1;
        this->date_len = 1;
        this->date_unit_len = 1;
        this->size_len = 1;
        this->suffix = ' ';
        this->prefix = ' ';
        this->isdir = false;
        this->modified = 0;
        this->bsize = 0;

        if (settings.colors) {
            this->color = findColor(SLK_ORPHAN);
        }
    } else {
        if (settings.colors) {
            this->color = getColor(file, st->st_mode);
        }

        this->suffix = ' ';

        #ifdef USE_GIT

        if (flags != UINT_MAX) {
            std::string symbol;
            color_t color = {0};

            if (S_ISDIR(st->st_mode)) { // NOLINT
                if ((flags & GIT_ISREPO) != 0) {
                    if ((flags & GIT_DIR_DIRTY) != 0) {
                        color = settings.color.git.repo_dirty;
                        symbol = settings.symbols.git.repo_dirty;
                    } else if ((flags & GIT_DIR_BARE) != 0) {
                        color = settings.color.git.repo_bare;
                        symbol = settings.symbols.git.repo_bare;
                    } else {
                        color = settings.color.git.repo_clean;
                        symbol = settings.symbols.git.repo_clean;
                    }

                    if (settings.override_git_repo_color) {
                        this->color = colorize(symbol, color, false);
                    } else {
                        this->git = colorize(symbol, color, true);
                    }
                } else {
                    if ((flags & GIT_DIR_DIRTY) != 0) {
                        color = settings.color.git.dir_dirty;
                        symbol = settings.symbols.git.dir_dirty;
                    } else if ((flags & GIT_STATUS_IGNORED) != 0) {
                        color = settings.color.git.ignore;
                        symbol = settings.symbols.git.ignore;
                    } else if ((flags & ( // NOLINT
                        GIT_STATUS_INDEX_NEW | GIT_STATUS_WT_NEW
                    )) != 0) {
                        color = settings.color.git.untracked;
                        symbol = settings.symbols.git.untracked;
                    } else {
                        color = settings.color.git.dir_clean;
                        symbol = settings.symbols.git.dir_clean;
                    }

                    if (settings.override_git_dir_color) {
                        this->color = colorize(symbol, color, false);
                    } else {
                        this->git = colorize(symbol, color, true);
                    }
                }
            } else {
                if ((flags & GIT_STATUS_IGNORED) != 0) {
                    color = settings.color.git.ignore;
                    symbol = settings.symbols.git.ignore;
                } else if ((flags & GIT_STATUS_CONFLICTED) != 0) {
                    color = settings.color.git.conflict;
                    symbol = settings.symbols.git.conflict;
                } else if ((flags & GIT_STATUS_WT_MODIFIED) != 0) {
                    color = settings.color.git.modified;
                    symbol = settings.symbols.git.modified;
                } else if ((flags & GIT_STATUS_WT_RENAMED) != 0) {
                    color = settings.color.git.renamed;
                    symbol = settings.symbols.git.renamed;
                } else if ((flags & GIT_STATUS_INDEX_NEW) != 0) {
                    color = settings.color.git.added;
                    symbol = settings.symbols.git.added;
                } else if ((flags & GIT_STATUS_WT_TYPECHANGE) != 0) {
                    color = settings.color.git.typechange;
                    symbol = settings.symbols.git.typechange;
                } else if ((flags & GIT_STATUS_WT_UNREADABLE) != 0) {
                    color = settings.color.git.unreadable;
                    symbol = settings.symbols.git.unreadable;
                } else if ((flags & ( // NOLINT
                    GIT_STATUS_INDEX_NEW | GIT_STATUS_WT_NEW
                )) != 0) {
                    color = settings.color.git.untracked;
                    symbol = settings.symbols.git.untracked;
                } else {
                    color = settings.color.git.unchanged;
                    symbol = settings.symbols.git.unchanged;
                }

                this->git = colorize(symbol, color, true);
            }
        }

        #endif

        this->target = "";

        #ifdef S_ISLNK

        if (S_ISLNK(st->st_mode) && !settings.resolve_links) { // NOLINT
            char target[PATH_MAX] = {0};

            if ((readlink(fullpath, &target[0], sizeof(target))) >= 0) {
                if (settings.list) {
                    this->suffix = " -> ";
                }

                this->islink = true;

                this->target = &target[0];
                std::string fpath = &target[0]; // NOLINT

                if (target[0] != '/') {
                    // NOLINTNEXTLINE
                    fpath = std::string(dirname(fullpath)) + "/" + this->target;
                }

                struct stat tst = {0};

                if ((lstat(fpath.c_str(), &tst)) < 0) {
                    this->target_color = this->color;
                } else {
                    this->color = getColor(file, tst.st_mode);
                    this->target_color = getColor(&target[0], tst.st_mode);
                }
            }
        }

        #endif /* S_ISLNK */

        char buf[PATH_MAX] = {0};

        auto cuid = uid_cache.find(st->st_uid);
        auto cgid = gid_cache.find(st->st_gid);
        std::string user;
        std::string group;

        if (cuid == uid_cache.end()) {
            struct passwd pw = { nullptr };
            struct passwd *pwp;

            if (getpwuid_r(st->st_uid, &pw, &buf[0], sizeof(buf), &pwp) != 0) {
                // NOLINTNEXTLINE
                user = colorize("????", settings.color.user.user, true);
            } else {
                // NOLINTNEXTLINE
                user = colorize(pw.pw_name, settings.color.user.user, true);
            }

            uid_cache[st->st_uid] = user;
        } else {
            user = cuid->second;
        }

        if (cgid == gid_cache.end()) {
            struct group gr = { nullptr };
            struct group *grp;

            if (getgrgid_r(st->st_gid, &gr, &buf[0], sizeof(buf), &grp) != 0) {
                // NOLINTNEXTLINE
                group = colorize("????", settings.color.user.group, true);
            } else {
                // NOLINTNEXTLINE
                group = colorize(gr.gr_name, settings.color.user.group, true);
            }

            gid_cache[st->st_gid] = group;
        } else {
            group += cgid->second;
        }

        this->user = user + colorize(
                         settings.symbols.user.separator,
                         settings.color.user.separator,
                         true
                     ) + group;

        this->date = timeAgo(st->st_ctime);
        this->size = unitConv(st->st_size);
        this->perms = lsPerms(st->st_mode);

        this->modified = st->st_ctime;
        this->bsize = st->st_size;

        this->file = file;

        this->user_len = cleanlen(this->user);
        this->date_len = cleanlen(this->date.first);
        this->date_unit_len = cleanlen(this->date.second);
        this->size_len = cleanlen(this->size);

        this->prefix = (fileHasAcl(fullpath, st) > 0) ? '+' : ' ';
        this->isdir = false;

        if (S_ISDIR(st->st_mode)) { // NOLINT
            this->suffix = colorize(
                               settings.symbols.suffix.dir,
                               settings.color.suffix.dir,
                               true
                           );
            this->isdir = true;
        } else if ((st->st_mode & S_IEXEC) != 0 && !islink) { // NOLINT
            this->suffix = colorize(
                               settings.symbols.suffix.exec,
                               settings.color.suffix.exec,
                               true
                           );
        }
    }

    this->file_len = (git + color + file + suffix).length();

    if (settings.colors) {
        this->file += "\033[0m";
        this->perms = colorperms(this->perms);

        this->clean_len = cleanlen(
                              this->git +
                              this->color +
                              this->file +
                              this->suffix
                          );
    } else {
        this->clean_len = this->file_len;
    }
}

std::string Entry::colorperms(std::string input)
{
    std::string output;
    color_t color = {0};

    for (auto c : input) {
        switch (c) {
            case 'b':
                color = settings.color.perm.block;
                break;

            case 'c':
                color = settings.color.perm.special;
                break;

            case 's':
                color = settings.color.perm.sticky;
                break;

            case 'l':
                color = settings.color.perm.link;
                break;

            case 'd':
                color = settings.color.perm.dir;
                break;

            case '-':
                color = settings.color.perm.none;
                break;

            case 'r':
                color = settings.color.perm.read;
                break;

            case 'w':
                color = settings.color.perm.write;
                break;

            case 'x':
            case 't':
                color = settings.color.perm.exec;
                break;

            case '?':
                color = settings.color.perm.unknown;
                break;

            default:
                color = settings.color.perm.other;
                break;
        }

        output += colorize(std::string(&c, 1), color, true); // NOLINT
    }

    return output;
}

void Entry::list(int max_user, int max_date, int max_date_unit,
                 int max_size)
{
    int ulen = cleanlen(user);
    int dlen = cleanlen(date.first);
    int dulen = cleanlen(date.second);
    int slen = cleanlen(size);

    // NOLINTNEXTLINE
    printf(
        " %s%s    %s%s   %s%s %s%s  %s%s  %s%s%s%s%s%s\033[0m\n",
        perms.c_str(),
        prefix.c_str(),
        user.c_str(),
        std::string(max_user - ulen, ' ').c_str(), // NOLINT
        date.first.c_str(),
        std::string(max_date - dlen, ' ').c_str(), // NOLINT
        date.second.c_str(),
        std::string(max_date_unit - dulen, ' ').c_str(), // NOLINT
        std::string(max_size - slen, ' ').c_str(), // NOLINT
        size.c_str(),
        #ifdef USE_GIT
        git.c_str()
        #else
        ""
        #endif
        ,
        color.c_str(),
        file.c_str(),
        suffix.c_str(),
        target_color.c_str(),
        target.c_str()
    );
}

void Entry::print(int max_len)
{
    std::string combined = git + color + file + suffix;
    int len = cleanlen(combined);

    // NOLINTNEXTLINE
    printf("%s", (combined + std::string(max_len - len, ' ')).c_str());
}

std::string Entry::findColor(const char *file)
{
    auto c = std::find_if(colors.begin(), colors.end(),
        [file](const std::pair<std::string, std::string> &t) -> bool {
            return wildcmp(
                t.first.c_str(),
                file,
                t.first.length() - 1,
                strlen(file) - 1
            );
        }
    );

    if (c != colors.end() && c->second != "target") {
        return "\033[" + c->second + "m";
    }

    c = colors.find("fi"); // NOLINT

    if (c != colors.end()) {
        return "\033[" + c->second + "m";
    }

    return "\033[0m"; // NOLINT
}

std::string Entry::getColor(const char *file, uint32_t mode)
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

    return findColor(file);
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

int Entry::fileHasAcl(char const *name, struct stat const *sb)
{
    ssize_t ret = 0;

    #ifdef S_ISLNK

    if (S_ISLNK(sb->st_mode)) { // NOLINT
        return 0;
    }

    #endif

    ret = getxattr(name, XATTR_NAME_POSIX_ACL_ACCESS, nullptr, 0);

    if (ret < 0 && errno == ENODATA) {
        ret = 0;
    } else if (ret > 0) {
        return 1;
    }

    if (ret == 0 && S_ISDIR(sb->st_mode)) { // NOLINT
        ret = getxattr(name, XATTR_NAME_POSIX_ACL_DEFAULT, nullptr, 0);

        if (ret < 0 && errno == ENODATA) {
            ret = 0;
        } else if (ret > 0) {
            return 1;
        }
    }

    return ret;
}

char *Entry::lsPerms(uint32_t mode)
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

    static char bits[11];

    bits[0] = static_cast<char>(fileTypeLetter(mode));

    strncpy(&bits[1], gsl::at(rwx, (mode >> 6) & 7), 3); // NOLINT
    strncpy(&bits[4], gsl::at(rwx, (mode >> 3) & 7), 3); // NOLINT
    strncpy(&bits[7], gsl::at(rwx, (mode & 7)), 3); // NOLINT

    if ((mode & S_ISUID) != 0) { // NOLINT
        bits[3] = (mode & S_IXUSR) != 0 ? 's' : 'S'; // NOLINT
    }

    if ((mode & S_ISGID) != 0) { // NOLINT
        bits[6] = (mode & S_IXGRP) != 0 ? 's' : 'l'; // NOLINT
    }

    if ((mode & S_ISVTX) != 0) { // NOLINT
        bits[9] = (mode & S_IXOTH) != 0 ? 't' : 'T'; // NOLINT
    }

    bits[10] = '\0';
    return &bits[0];
}

std::string Entry::unitConv(float size)
{
    std::string unit;
    static const char *units[] = {
        settings.symbols.size.byte.c_str(),
        settings.symbols.size.kilo.c_str(),
        settings.symbols.size.mega.c_str(),
        settings.symbols.size.giga.c_str(),
        settings.symbols.size.tera.c_str(),
        settings.symbols.size.peta.c_str(),
    };

    static const color_t colors[] = {
        settings.color.size.byte,
        settings.color.size.kilo,
        settings.color.size.mega,
        settings.color.size.giga,
        settings.color.size.tera,
        settings.color.size.peta,
    };

    char csize[PATH_MAX] = {0};

    for (uint32_t i = 0; i < sizeof(units); i++) {
        if ((size / 1024) <= 1.f) {

            color_t c_symbol = {0};
            color_t c_unit = {0};

            if (!settings.size_number_color) {
                c_unit = gsl::at(colors, i);
                c_symbol = c_unit;
            } else if (settings.size_number_color) {
                c_symbol = gsl::at(colors, i);
                c_unit = settings.color.size.number;
            }

            if (static_cast<int>(size * 10) % 10 == 0) {
                // NOLINTNEXTLINE
                snprintf(&csize[0], sizeof(csize), "%d", static_cast<int>(size));
            } else {
                // NOLINTNEXTLINE
                snprintf(&csize[0], sizeof(csize), "%.1f", size);
            }

            unit = colorize(&csize[0], c_unit, true) + // NOLINT
                   colorize(gsl::at(units, i), c_symbol, true); // NOLINT

            return unit;
        }

        size /= 1024;
    }

    snprintf(&csize[0], strlen(&csize[0]), "%.2g?", size); // NOLINT
    return &csize[0]; // NOLINT
}

DateFormat Entry::toDateFormat(const std::string &num, int unit)
{
    color_t c_symbol = {0};
    color_t c_unit = {0};

    static const char *units[] = {
        settings.symbols.date.sec.c_str(),
        settings.symbols.date.min.c_str(),
        settings.symbols.date.hour.c_str(),
        settings.symbols.date.day.c_str(),
        settings.symbols.date.mon.c_str(),
        settings.symbols.date.year.c_str(),
    };

    static const color_t colors[] = {
        settings.color.date.sec,
        settings.color.date.min,
        settings.color.date.hour,
        settings.color.date.day,
        settings.color.date.mon,
        settings.color.date.year,
    };

    if (!settings.date_number_color) {
        c_unit = gsl::at(colors, unit);
        c_symbol = c_unit;
    } else if (settings.date_number_color) {
        c_symbol = gsl::at(colors, unit);
        c_unit = settings.color.date.number;
    }

    return DateFormat(
               colorize(num, c_unit, true),
               colorize(gsl::at(units, unit), c_symbol, true) // NOLINT
           );
}

DateFormat Entry::timeAgo(int64_t ftime)
{
    time_t utime;
    time(&utime);

    int64_t delta = utime - ftime;
    int64_t rel = delta;

    if (delta < 10) {
        return toDateFormat("<", DATE_SEC); // NOLINT
    }

    if (delta < 60) {
        return toDateFormat(std::to_string(rel), DATE_SEC);
    }

    rel /= 60;

    if (delta < 120) {
        return toDateFormat("<", DATE_MIN); // NOLINT
    }

    if (delta < 2700) {
        return toDateFormat(std::to_string(rel), DATE_MIN);
    }

    rel /= 60;

    if (delta < 5400) {
        return toDateFormat("<", DATE_HOUR); // NOLINT
    }

    if (delta < 129600) {
        return toDateFormat(std::to_string(rel), DATE_HOUR);
    }

    rel /= 24;

    if (delta < 172800) {
        return toDateFormat("<", DATE_DAY); // NOLINT
    }

    if (delta < 2592000) {
        return toDateFormat(std::to_string(rel), DATE_DAY);
    }

    rel /= 30;

    if (delta < 5184000) {
        return toDateFormat("<", DATE_MON); // NOLINT
    }

    if (delta < 31104000) {
        return toDateFormat(std::to_string(rel), DATE_MON);
    }

    rel /= 12;

    return toDateFormat(std::to_string(rel), DATE_YEAR);
}
