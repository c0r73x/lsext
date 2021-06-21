#include "entry.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <locale>
#include <unordered_map>

#include <gsl-lite.hpp>
#include <re2/re2.h>

extern "C" {
    #include <dirent.h>
    #include <grp.h>
    #include <libgen.h>
    #include <pwd.h>
    #include <sys/stat.h>
    #include <sys/statvfs.h>
    #include <sys/xattr.h>
    #include <unistd.h>
    #include <wordexp.h>
    #include <stb_sprintf.h>

    #ifdef __linux__
        #include <linux/xattr.h>
        #include <mntent.h>
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

std::unordered_map<std::string, std::string> colors;

std::unordered_map<uint8_t, std::string> uid_cache;
std::unordered_map<uint8_t, std::string> gid_cache;

std::string Entry::colorize(std::string input, color_t color)
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

        if (color.bg >= 0 && color.fg >= 0) {
            output += "\033[0m";
        }

        return output;
    }

    return input;
}

uint32_t Entry::cleanlen(std::string input)
{
    static re2::RE2 esc_re("\033\\[?[;:0-9]*m");
    static re2::RE2 uni_re("[\u0080-\uffff]+");

    re2::RE2::GlobalReplace(&input, esc_re, "");
    re2::RE2::GlobalReplace(&input, uni_re, " ");

    return input.length();
}

std::string Entry::isMountpoint(char *fullpath, struct stat *st)
{
    if (settings.resolve_mounts && settings.list) {
        struct stat parent = {0};
        struct stat target = {0};

        #ifdef __linux__
        struct stat check = {0};
        struct mntent *mnt = nullptr;
        #endif

        char *ppath = dirname(fullpath);

        if (stat(ppath, &parent) == 0) {
            if (st->st_dev != parent.st_dev || st->st_ino == parent.st_ino) {
                #ifdef __linux__

                FILE *fp = setmntent("/proc/mounts", "r");

                if (fp != nullptr) {
                    while ((mnt = getmntent(fp)) != nullptr) {
                        if (stat(mnt->mnt_dir, &check) != 0) {
                            continue;
                        }

                        if (
                            check.st_dev == st->st_dev &&
                            strcmp(mnt->mnt_type, "autofs") != 0
                        ) {
                            endmntent(fp);

                            this->islink = true;
                            this->target = mnt->mnt_fsname;

                            if (stat(mnt->mnt_fsname, &target) == 0) {
                                this->target_color = getColor(
                                                         mnt->mnt_fsname,
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
                }

                endmntent(fp);

                #elif __APPLE__

                struct statfs *mounts;
                int num = getmntinfo(&mounts, MNT_WAIT);

                if (num != 0) {
                    for (int i = 0; i < num; i++) {
                        if (st->st_dev == mounts[i].f_fsid.val[0]) {
                            this->islink = true;
                            this->target = mounts[i].f_mntfromname;

                            if (stat(mounts[i].f_mntfromname, &target) == 0) {
                                this->target_color = getColor(
                                                         mounts[i].f_mntfromname,
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
                }

                #endif
            }
        }
    }

    return colorize(
               settings.symbols.suffix.dir,
               settings.color.suffix.dir
           );
}

Entry::Entry(
    const std::string &file,
    char *fullpath,
    struct stat *st,
    unsigned int flags
) :
    file(file),
    git(1, ' '), // NOLINT
    suffix(1, ' ') // NOLINT
{
    this->islink = false;
    this->totlen = 0;

    if (st == nullptr) {
        this->user = colorize("????", settings.color.user.user); // NOLINT
        this->group = colorize("????", settings.color.user.group); // NOLINT
        this->mode = 0;
        this->isdir = false;
        this->modified = 0;
        this->bsize = 0;

        this->color = findColor(SLK_ORPHAN);
    } else {
        this->color = getColor(file, st->st_mode);

        #ifdef USE_GIT

        if (flags != NO_FLAGS) {
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
                        this->color = colorize(symbol, color);
                    } else {
                        this->git = colorize(symbol, color);
                    }
                } else {
                    if ((flags & GIT_DIR_DIRTY) != 0) {
                        color = settings.color.git.dir_dirty;
                        symbol = settings.symbols.git.dir_dirty;
                    } else if ((flags & GIT_STATUS_IGNORED) != 0) {
                        color = settings.color.git.ignore;
                        symbol = settings.symbols.git.ignore;
                    } else if ((flags & GIT_ISTRACKED) != 0) {
                        color = settings.color.git.dir_clean;
                        symbol = settings.symbols.git.dir_clean;
                    } else {
                        color = settings.color.git.untracked;
                        symbol = settings.symbols.git.untracked;
                    }

                    if (settings.override_git_dir_color) {
                        this->color = colorize(symbol + file, color);
                    } else {
                        this->git = colorize(symbol, color);
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
                } else if ((flags & GIT_ISTRACKED) != 0) {
                    color = settings.color.git.unchanged;
                    symbol = settings.symbols.git.unchanged;
                } else {
                    color = settings.color.git.untracked;
                    symbol = settings.symbols.git.untracked;
                }

                this->git = colorize(symbol, color);
            }
        }

        #endif

        this->target = "";

        #ifdef S_ISLNK

        if (S_ISLNK(st->st_mode) && !settings.resolve_links) { // NOLINT
            char target[PATH_MAX] = {0};

            if ((readlink(fullpath, &target[0], sizeof(target))) >= 0) {
                if (settings.list) {
                    this->suffix = colorize(
                                       settings.symbols.suffix.link,
                                       settings.color.suffix.link
                                   );
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
                    this->color = findColor(SLK_ORPHAN);
                    this->target_color = findColor(SLK_MISSING);
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

        if (cuid == uid_cache.end()) {
            struct passwd pw = { nullptr };
            struct passwd *pwp;
            if(settings.numeric_id) {
                char uidbuf[PATH_MAX]={0};
                stbsp_snprintf(uidbuf,PATH_MAX,"%i",st->st_uid);
                this->user=colorize(uidbuf,settings.color.user.user);
            } else {
                getpwuid_r(st->st_uid, &pw, &buf[0], sizeof(buf), &pwp);
                if (strlen(pw.pw_name) == 0) {
                    char uidbuf[PATH_MAX]={0};
                    stbsp_snprintf(uidbuf,PATH_MAX,"%i",st->st_uid);
                    // NOLINTNEXTLINE
                    this->user = colorize(uidbuf,settings.color.user.user);
                } else {
                    // NOLINTNEXTLINE
                    this->user = colorize(pw.pw_name, settings.color.user.user);
                }
            }


            uid_cache[st->st_uid] = this->user;
        } else {
            this->user = cuid->second;
        }

        if (cgid == gid_cache.end()) {
            struct group gr = { nullptr };
            struct group *grp;
            if(settings.numeric_id) {
                char gidbuf[PATH_MAX]={0};
                stbsp_snprintf(gidbuf,PATH_MAX,"%i",st->st_gid);
                this->group=colorize(gidbuf,settings.color.user.group);
            } else {
                getgrgid_r(st->st_gid, &gr, &buf[0], sizeof(buf), &grp);
                if (strlen(gr.gr_name) == 0) {
                    char gidbuf[PATH_MAX]={0};
                    stbsp_snprintf(gidbuf,PATH_MAX,"%i",st->st_gid);
                    // NOLINTNEXTLINE
                    this->group = colorize(gidbuf,settings.color.user.group);
                } else {
                    // NOLINTNEXTLINE
                    this->group = colorize(gr.gr_name, settings.color.user.group);
                }
            }

            gid_cache[st->st_gid] = this->group;
        } else {
            this->group = cgid->second;
        }

        this->modified = st->st_mtime;
        this->bsize = st->st_size;
        this->mode = st->st_mode;
        this->fullpath = fullpath;

        this->isdir = false;

        if (S_ISDIR(st->st_mode)) { // NOLINT
            this->isdir = true;
            this->suffix = isMountpoint(fullpath, st);
        } else if ((st->st_mode & S_IEXEC) != 0 && !islink) { // NOLINT
            this->suffix = colorize(
                               settings.symbols.suffix.exec,
                               settings.color.suffix.exec
                           );
        }
    }

    if (isdir){
        extension = "directory";
    } else {
        std::string::size_type idx = file.rfind('.');

        if (idx != std::string::npos) {
            extension = file.substr(idx + 1);
        } else {
            extension = "unknown";
        }
    }

    if (settings.colors) {
        this->file += "\033[0m";
    }

    postprocess();
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

            case '-': case '0':
                color = settings.color.perm.none;
                break;

            case 'r': case '4':
                color = settings.color.perm.read;
                break;

            case '7':
                color = settings.color.perm.full;
                break;

            case '6':
                color = settings.color.perm.readwrite;
                break;

            case '5':
                color = settings.color.perm.readexec;
                break;

            case '3':
                color = settings.color.perm.writeexec;
                break;

            case 'w': case '2':
                color = settings.color.perm.write;
                break;

            case 'x':
            case 't': case '1':
                color = settings.color.perm.exec;
                break;

            case '?':
                color = settings.color.perm.unknown;
                break;

            default:
                color = settings.color.perm.other;
                break;
        }

        output += colorize(std::string(&c, 1), color); // NOLINT
    }

    return output;
}

Segment Entry::format(char c)
{
    Segment output;

    switch (c) {
        case 'p': {
            output.first = lsPerms(mode);
            break;
        }

        case 'P': {
            output.first = chmodPerms(mode);
            break;
        }

        case 'u': {
            output.first = user;
            break;
        }

        case 'g': {
            output.first = group;
            break;
        }

        case 'U': {
            output.first = user + colorize(
                               settings.symbols.user.separator,
                               settings.color.user.separator
                           ) + group;
            break;
        }

        case 'r': {
            output.first = relativeTime(modified).first;
            break;
        }

        case 't': {
            output.first = relativeTime(modified).second;
            break;
        }

        case 'D': {
            output.first = isoTime(modified).first;
            break;
        }

        case 'T': {
            output.first = isoTime(modified).second;
            break;
        }

        case 's': {
            output.first = unitConv(bsize);
            break;
        }

        case 'G': {
            #ifdef USE_GIT
                output.first = git;
            #else
                output.first = "";
            #endif
            break;
        }

        case 'f': {
            output.first += color + file + suffix + target_color + target;
            break;
        }

        case 'F': {
            output.first += color + file + suffix;
            break;
        }

        default: {
            output.first = std::string(1, c); // NOLINT
        }
    }

    if (settings.colors) {
        output.first += "\033[0m";
    }

    output.second = cleanlen(output.first);
    return output;
}

void Entry::postprocess()
{
    totlen = settings.format.length();

    for (size_t pos = 0; (pos = settings.format.find('@', pos)) != std::string::npos;) {
        pos++;

        char c = settings.format.at(pos);

        if (c == '^') {
            pos++;
            c = settings.format.at(pos);
            totlen -= 1;
        }

        if (c != '@') {
            totlen -= 2;
            auto f = processed.find(c);

            if (f == processed.end()) {
                processed[c] = format(c);
                totlen += processed[c].second;
            }
        }
    }
}

std::string Entry::print(Lengths maxlens, int *outlen)
{
    std::string output;

    // NOLINTNEXTLINE
    for (auto c = settings.format.begin(); c != settings.format.end(); c++) {
        switch (*c) {
            case '@': {
                c++;

                if (*c == '^') {
                    c++;

                    auto s = processed[*c];
                    auto m = maxlens[*c];
                    *outlen += m;

                    output += std::string(m - s.second, ' '); // NOLINT
                    output += s.first;
                } else {
                    auto s = processed[*c];
                    auto m = maxlens[*c];
                    *outlen += m;

                    output += s.first;
                    output += std::string(m - s.second, ' '); // NOLINT
                }

                break;
            }

            default:
                output += *c;
                *outlen += 1;
        }
    }

    return output;
}

std::string Entry::findColor(const std::string &file)
{
    if (settings.colors) {
        auto c = colors.find(file); // NOLINT

        if (c != colors.end() && c->second != "target") {
            return "\033[" + c->second + "m";
        }

        c = std::find_if(colors.begin(), colors.end(),
            [file](const std::pair<std::string, std::string> &t) -> bool {
                return wildcmp(
                    t.first.c_str(),
                    file.c_str(),
                    t.first.length() - 1,
                    file.length() - 1
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

    return ""; // NOLINT
}

std::string Entry::getColor(const std::string &file, uint32_t mode)
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

    if (xattr < 0 && errno == ENODATA) {
        xattr = 0;
    } else if (xattr > 0) {
        return '+';
    }

    if (xattr == 0 && S_ISDIR(mode)) { // NOLINT
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
        return '+';
    }

    #endif

    return ' ';
}

std::string Entry::chmodPerms(uint32_t mode)
{
    std::string sbits = std::to_string((mode >> 6u) & 7u) +
        std::to_string((mode >> 3u) & 7u) +
        std::to_string(mode & 7u);

    return colorperms(sbits);
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
    std::string sbits;

    bits[0] = static_cast<char>(fileTypeLetter(mode));

    strncpy(&bits[1], gsl::at(rwx, (mode >> 6u) & 7u), 3u);
    strncpy(&bits[4], gsl::at(rwx, (mode >> 3u) & 7u), 3u);
    strncpy(&bits[7], gsl::at(rwx, (mode & 7u)), 3u);

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
    sbits = &bits[0];
    return colorperms(sbits + fileHasAcl());
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
                stbsp_snprintf(&csize[0], sizeof(csize), "%d", static_cast<int>(size));
            } else {
                // NOLINTNEXTLINE
                stbsp_snprintf(&csize[0], sizeof(csize), "%.1f", size);
            }

            unit = colorize(&csize[0], c_unit) + // NOLINT
                   colorize(gsl::at(units, i), c_symbol); // NOLINT

            return unit;
        }

        size /= 1024;
    }

    stbsp_snprintf(&csize[0], strlen(&csize[0]), "%.2g?", size); // NOLINT
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
        settings.symbols.date.week.c_str(),
        settings.symbols.date.mon.c_str(),
        settings.symbols.date.year.c_str(),
    };

    static const color_t colors[] = {
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
    } else if (settings.date_number_color) {
        c_symbol = gsl::at(colors, unit);
        c_unit = settings.color.date.number;
    }

    return DateFormat(
               colorize(num, c_unit),
               colorize(gsl::at(units, unit), c_symbol) // NOLINT
           );
}

DateFormat Entry::isoTime(time_t ftime)
{
    DateFormat output;
    auto tm = std::localtime(&ftime);

    output.first = colorize(
        fmt("%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon, tm->tm_mday),
        settings.color.date.year
    );

    auto color = settings.color.date.number;
    if (!settings.date_number_color) {
        color = settings.color.date.year;
    }

    output.second = colorize(fmt("%02d:%02d", tm->tm_hour, tm->tm_min), color);

    return output;
}

DateFormat Entry::relativeTime(time_t ftime)
{
    time_t utime;
    time(&utime);

    int64_t delta = utime - ftime;
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
