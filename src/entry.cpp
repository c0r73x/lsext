#include "entry.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <climits>

#include <algorithm>

#include <dirent.h>
#include <grp.h>
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

std::vector<Color> colors;

int wildcmp(const char *w, const char *s)
{
    int lw = strlen(w) + 1;
    int ls = strlen(s) + 1;

    auto wp = gsl::make_span<const char>(w, w + lw);
    auto sp = gsl::make_span<const char>(s, s + ls);

    for (int i = 0; i < lw; i++) {
        switch (wp.at(i)) {
            case '*': {
                for (int j = i; j < ls; j++) {
                    if (wildcmp(&wp.at(i + 1), &sp.at(j)) == 0) {
                        return 0;
                    }
                }

                return -1;
            }

            default:
                if (wp.at(i) != sp.at(i)) {
                    return -1;
                }

                break;
        }
    }

    return 0;
}

Entry::Entry(std::string directory, const char *file, char *fullpath,
             struct stat *st, unsigned int flags)
{
    this->islink = false;
    this->color = "";

    if (st == nullptr) {
        this->user = '?';
        this->date = DateFormat("?", "?");
        this->size = '?';
        this->perms = "l?????????";
        this->file = file;
        this->user_len = 1;
        this->date_len = 1;
        this->size_len = 1;
        this->prefix = ' ';
        this->git = " ";
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
            int color;

            if (S_ISDIR(st->st_mode)) {
                if (flags & GIT_ISREPO) {
                    if (flags & GIT_DIR_DIRTY) {
                        color = settings.color.git.repo_dirty;
                        symbol = settings.symbols.git.repo_dirty;
                    } else {
                        color = settings.color.git.repo_clean;
                        symbol = settings.symbols.git.repo_clean;
                    }
                } else {
                    if (flags & GIT_DIR_DIRTY) {
                        color = settings.color.git.dir_dirty;
                        symbol = settings.symbols.git.dir_dirty;
                    } else {
                        color = settings.color.git.dir_clean;
                        symbol = settings.symbols.git.dir_clean;
                    }
                }
            } else {
                if (flags & GIT_STATUS_IGNORED) {
                    color = settings.color.git.ignore;
                    symbol = settings.symbols.git.ignore;
                } else if (flags & GIT_STATUS_CONFLICTED) {
                    color = settings.color.git.conflict;
                    symbol = settings.symbols.git.conflict;
                } else if (flags & GIT_STATUS_WT_MODIFIED) {
                    color = settings.color.git.modified;
                    symbol = settings.symbols.git.modified;
                } else if (flags & GIT_STATUS_WT_RENAMED) {
                    color = settings.color.git.renamed;
                    symbol = settings.symbols.git.renamed;
                } else if (flags & GIT_STATUS_INDEX_NEW) {
                    color = settings.color.git.added;
                    symbol = settings.symbols.git.added;
                } else if (flags & GIT_STATUS_WT_TYPECHANGE) {
                    color = settings.color.git.typechange;
                    symbol = settings.symbols.git.typechange;
                } else if (flags & GIT_STATUS_WT_UNREADABLE) {
                    color = settings.color.git.unreadable;
                    symbol = settings.symbols.git.unreadable;
                } else if (flags & (GIT_STATUS_INDEX_NEW | GIT_STATUS_WT_NEW)) {
                    color = settings.color.git.untracked;
                    symbol = settings.symbols.git.untracked;
                } else {
                    color = settings.color.git.unchanged;
                    symbol = settings.symbols.git.unchanged;
                }
            }

            if (settings.colors) {
                this->git = "\033[38;5;" + std::to_string(color) + "m";
                this->git += symbol;
                this->git += "\033[0m";
            } else {
                this->git = symbol;
            }
        } else {
            this->git = ' ';
        }

        #endif

        this->target = "";

        #ifdef S_ISLNK

        if (S_ISLNK(st->st_mode) && !settings.resolve_links) {
            char target[PATH_MAX] = {0};

            if ((readlink(fullpath, &target[0], sizeof(target))) >= 0) {

                if (settings.list) {
                    this->suffix = " -> ";
                }

                this->islink = true;

                this->target = &target[0];
                std::string path = &target[0];

                char linkpath[PATH_MAX] = {0};

                if (target[0] != '/') {
                    path = directory + "/" + this->target;
                }

                if (realpath(path.c_str(), &linkpath[0]) != nullptr) {
                    struct stat tst = {0};

                    if ((lstat(&linkpath[0], &tst)) < 0) {
                        this->color = findColor(SLK_ORPHAN);
                        this->target_color = findColor(SLK_ORPHAN);
                    } else {
                        this->color = getColor(file, tst.st_mode);
                        this->target_color = getColor(&target[0], tst.st_mode);
                    }
                } else {
                    this->color = findColor(SLK_MISSING);
                    this->target_color = findColor(SLK_MISSING);

                    if (!settings.list) {
                        this->suffix = '@';
                    }
                }
            }
        }

        #endif /* S_ISLNK */

        struct passwd *pw = getpwuid(st->st_uid);
        struct group  *gr = getgrgid(st->st_gid);

        if (settings.colors) {
            this->user = "\033[38;5;" + std::to_string(
                             settings.color.user.user
                         ) + "m";

            this->user += pw->pw_name;
            this->user += "\033[38;5;" + std::to_string(
                              settings.color.user.separator
                          ) + "m";

            this->user += settings.symbols.user.separator;
            this->user += "\033[38;5;" + std::to_string(
                              settings.color.user.group
                          ) + "m";

            this->user += gr->gr_name;
            this->user += "\033[0m";
        } else {
            this->user = pw->pw_name;
            this->user += settings.symbols.user.separator;
            this->user += gr->gr_name;
        }

        this->date = timeAgo(st->st_ctime);
        this->size = unitConv(st->st_size);
        this->perms = lsPerms(st->st_mode);

        this->modified = st->st_ctime;
        this->bsize = st->st_size;

        this->file = file;

        this->user_len = this->user.length();
        this->date_len = this->date.first.length();
        this->date_unit_len = this->date.second.length();
        this->size_len = this->size.length();

        this->prefix = (fileHasAcl(fullpath, st) > 0) ? '+' : ' ';
        this->isdir = false;

        if (S_ISDIR(st->st_mode)) {
            this->suffix = '/';
            this->isdir = true;
        } else if ((st->st_mode & S_IEXEC) != 0 && !islink) {
            this->suffix = '*';
        }

        this->file_len = strlen(file) + suffix.length();
    }

    if (settings.colors) {
        this->perms = colorperms(this->perms);
    }
}

std::string Entry::colorperms(std::string input)
{
    std::string output;
    std::string symbol;

    for (auto c : input) {
        switch (c) {
            case 'b':
                symbol = std::to_string(settings.color.perm.block);
                break;

            case 'c':
                symbol = std::to_string(settings.color.perm.special);
                break;

            case 's':
                symbol = std::to_string(settings.color.perm.sticky);
                break;

            case 'l':
                symbol = std::to_string(settings.color.perm.link);
                break;

            case 'd':
                symbol = std::to_string(settings.color.perm.dir);
                break;

            case '-':
                symbol = std::to_string(settings.color.perm.none);
                break;

            case 'r':
                symbol = std::to_string(settings.color.perm.read);
                break;

            case 'w':
                symbol = std::to_string(settings.color.perm.write);
                break;

            case 'x':
            case 't':
                symbol = std::to_string(settings.color.perm.exec);
                break;

            case '?':
                symbol = std::to_string(settings.color.perm.unknown);
                break;

            default:
                symbol = std::to_string(settings.color.perm.other);
                break;
        }

        output += "\033[38;5;" + symbol + "m";
        output += c;
        output += "\033[0m";
    }

    return output;
}

void Entry::list(int max_user, int max_date, int max_date_unit, int max_size)
{
    printf(
        " %s%s    %-*s   %-*s%-*s %*s  %s%s%s\033[0m%s%s%s\033[0m\n",
        perms.c_str(),
        prefix.c_str(),
        (max_user + 1),
        user.c_str(),
        (max_date + 1),
        date.first.c_str(),
        (max_date_unit + 1),
        date.second.c_str(),
        (max_size + 1),
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
    /* max_len += + strlen("\033[0m") * 2; */

    printf(
        "%s%s%-*s\033[0m",
        git.c_str(),
        color.c_str(),
        (max_len),
        (file + suffix).c_str()
    );
}

std::string Entry::findColor(const char *file)
{
    for (auto c : colors) {
        if (wildcmp(c.glob.c_str(), file) == 0) {
            return "\033[" + c.color + "m";

        }
    }

    for (auto c : colors) {
        if (wildcmp(c.glob.c_str(), "fi") == 0) {
            return "\033[" + c.color + "m";
        }
    }

    return "\033[0m";
}

std::string Entry::getColor(const char *file, unsigned int mode)
{
    if ((mode & S_ISUID) != 0) {
        return findColor(SLK_SUID);
    }

    if ((mode & S_ISGID) != 0) {
        return findColor(SLK_SGID);
    }

    if ((mode & S_ISVTX) != 0) {
        return findColor((mode & S_IWOTH) ? SLK_OWT : SLK_STICKY);
    }

    if (S_ISDIR(mode)) {
        return findColor((mode & S_IWOTH) ? SLK_OWR : SLK_DIR);
    }

    if (S_ISBLK(mode)) {
        return findColor(SLK_BLK);
    }

    if (S_ISCHR(mode)) {
        return findColor(SLK_CHR);
    }

    #ifdef S_ISFIFO

    if (S_ISFIFO(mode)) {
        return findColor(SLK_FIFO);
    }

    #endif /* S_ISFIFO */

    #ifdef S_ISLNK

    if (S_ISLNK(mode)) {
        return findColor(SLK_LNK);
    }

    #endif /* S_ISLNK */

    #ifdef S_ISSOCK

    if (S_ISSOCK(mode)) {
        return findColor(SLK_SOCK);
    }

    #endif /* S_ISSOCK */

    #ifdef S_ISDOOR /* Solaris 2.6, etc. */

    if (S_ISDOOR(mode)) {
        return findColor(SLK_DOOR);
    }

    #endif /* S_ISDOOR */

    return findColor(file);
}

char Entry::fileTypeLetter(unsigned int mode)
{
    if (S_ISREG(mode)) {
        return '-';
    }

    if (S_ISDIR(mode)) {
        return 'd';
    }

    if (S_ISBLK(mode)) {
        return 'b';
    }

    if (S_ISCHR(mode)) {
        return 'c';
    }

    #ifdef S_ISFIFO

    if (S_ISFIFO(mode)) {
        return 'p';
    }

    #endif /* S_ISFIFO */

    #ifdef S_ISLNK

    if (S_ISLNK(mode)) {
        return 'l';
    }

    #endif /* S_ISLNK */

    #ifdef S_ISSOCK

    if (S_ISSOCK(mode)) {
        return 's';
    }

    #endif /* S_ISSOCK */

    #ifdef S_ISDOOR /* Solaris 2.6, etc. */

    if (S_ISDOOR(mode)) {
        return 'D';
    }

    #endif /* S_ISDOOR */

    return '?';
}

int Entry::fileHasAcl(char const *name, struct stat const *sb)
{
    ssize_t ret = 0;

    #ifdef S_ISLNK

    if (S_ISLNK(sb->st_mode)) {
        return 0;
    }

    #endif

    ret = getxattr(name, XATTR_NAME_POSIX_ACL_ACCESS, nullptr, 0);

    if (ret < 0 && errno == ENODATA) {
        ret = 0;
    } else if (ret > 0) {
        return 1;
    }

    if (ret == 0 && S_ISDIR(sb->st_mode)) {
        ret = getxattr(name, XATTR_NAME_POSIX_ACL_DEFAULT, nullptr, 0);

        if (ret < 0 && errno == ENODATA) {
            ret = 0;
        } else if (ret > 0) {
            return 1;
        }
    }

    return ret;
}

char *Entry::lsPerms(unsigned int mode)
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

    strncpy(&bits[1], gsl::at(rwx, (mode >> 6) & 7), 3);
    strncpy(&bits[4], gsl::at(rwx, (mode >> 3) & 7), 3);
    strncpy(&bits[7], gsl::at(rwx, (mode & 7)), 3);

    if ((mode & S_ISUID) != 0) {
        bits[3] = (mode & S_IXUSR) != 0 ? 's' : 'S';
    }

    if ((mode & S_ISGID) != 0) {
        bits[6] = (mode & S_IXGRP) != 0 ? 's' : 'l';
    }

    if ((mode & S_ISVTX) != 0) {
        bits[9] = (mode & S_IXOTH) != 0 ? 't' : 'T';
    }

    bits[10] = '\0';
    return &bits[0];
}

std::string Entry::unitConv(float size)
{
    static char unit[40] = {0};
    static const char *units[] = {
        settings.symbols.size.byte.c_str(),
        settings.symbols.size.kilo.c_str(),
        settings.symbols.size.mega.c_str(),
        settings.symbols.size.giga.c_str(),
        settings.symbols.size.tera.c_str(),
        settings.symbols.size.peta.c_str(),
    };

    static const int colors[] = {
        settings.color.size.byte,
        settings.color.size.kilo,
        settings.color.size.mega,
        settings.color.size.giga,
        settings.color.size.tera,
        settings.color.size.peta,
    };

    unsigned int len = sizeof(unit);

    for (unsigned int i = 0; i < sizeof(units); i++) {
        if ((size / 1024) <= 1.f) {
            std::string c_symbol = "";
            std::string c_unit = "";

            if (settings.colors && !settings.size_number_color) {
                c_unit = "\033[38;5;" + std::to_string(
                             gsl::at(colors, i)
                         ) + "m";
            } else if (settings.colors && settings.size_number_color) {
                c_symbol = "\033[38;5;" + std::to_string(
                               gsl::at(colors, i)
                           ) + "m";
                c_unit = "\033[38;5;" + std::to_string(
                             settings.color.size.number
                         ) + "m";
            }

            if (static_cast<int>(size * 10) % 10 == 0) {
                snprintf(
                    &unit[0],
                    len,
                    "%s%d%s%s\033[0m",
                    c_unit.c_str(),
                    static_cast<int>(size),
                    c_symbol.c_str(),
                    gsl::at(units, i)
                );
            } else {
                snprintf(
                    &unit[0],
                    len,
                    "%s%.1f%s%s\033[0m",
                    c_unit.c_str(),
                    size,
                    c_symbol.c_str(),
                    gsl::at(units, i)
                );
            }

            return &unit[0];
        }

        size /= 1024;
    }

    snprintf(&unit[0], len, "%.2g?", size);
    return &unit[0];
}

DateFormat toDateFormat(std::string num, int unit)
{
    std::string c_symbol = "";
    std::string c_unit = "";

    static const char *units[] = {
        settings.symbols.date.sec.c_str(),
        settings.symbols.date.min.c_str(),
        settings.symbols.date.hour.c_str(),
        settings.symbols.date.day.c_str(),
        settings.symbols.date.mon.c_str(),
        settings.symbols.date.year.c_str(),
    };

    static const int colors[] = {
        settings.color.date.sec,
        settings.color.date.min,
        settings.color.date.hour,
        settings.color.date.day,
        settings.color.date.mon,
        settings.color.date.year,
    };

    if (settings.colors && !settings.size_number_color) {
        c_unit = "\033[38;5;" + std::to_string(
                     gsl::at(colors, unit)
                 ) + "m";
    } else if (settings.colors && settings.size_number_color) {
        c_symbol = "\033[38;5;" + std::to_string(
                       gsl::at(colors, unit)
                   ) + "m";
        c_unit = "\033[38;5;" + std::to_string(
                     settings.color.date.number
                 ) + "m";
    }

    return DateFormat(
               c_unit + num,
               c_symbol + gsl::at(units, unit) + "\033[0m"
           );
}

DateFormat Entry::timeAgo(int64_t ftime)
{
    time_t utime;
    time(&utime);

    int64_t delta = utime - ftime;
    int64_t rel = delta;

    if (delta < 10) {
        return toDateFormat("<", DATE_SEC);
    }

    if (delta < 60) {
        return toDateFormat(std::to_string(rel), DATE_SEC);
    }

    rel /= 60;

    if (delta < 120) {
        return toDateFormat("<", DATE_MIN);
    }

    if (delta < 2700) {
        return toDateFormat(std::to_string(rel), DATE_MIN);
    }

    rel /= 60;

    if (delta < 5400) {
        return toDateFormat("<", DATE_HOUR);
    }

    if (delta < 129600) {
        return toDateFormat(std::to_string(rel), DATE_HOUR);
    }

    rel /= 24;

    if (delta < 172800) {
        return toDateFormat("<", DATE_DAY);
    }

    if (delta < 2592000) {
        return toDateFormat(std::to_string(rel), DATE_DAY);
    }

    rel /= 30;

    if (delta < 5184000) {
        return toDateFormat("<", DATE_MON);
    }

    if (delta < 31104000) {
        return toDateFormat(std::to_string(rel), DATE_MON);
    }

    rel /= 12;

    return toDateFormat(std::to_string(rel), DATE_YEAR);
}
