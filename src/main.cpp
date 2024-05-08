#include <cstdio>
#include <gsl-lite.hpp>
#include <re2/re2.h>

#include "entry.hpp"

#include <future>

extern "C" {
#include <dirent.h>
#include <libgen.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <getopt.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#ifdef USE_OPENMP
    #include <omp.h>
#endif

#ifdef USE_GIT
    #include <git2.h>
#else
    using git_repository = int;
#endif
}

using FileList = std::vector<Entry *>;
using DirList = std::unordered_map<std::string, FileList>;
using FlagsList = std::unordered_map<std::string, unsigned int>;

static re2::RE2 git_re("/\\.git/?$");

settings_t settings = {0};

void initcolors()
{
    const char *foo = new char;
    printf("%s\n", foo);
    const char *ls_colors = std::getenv("LS_COLORS");

    std::stringstream ss;
    ss << ls_colors;

    std::string token;

    while (std::getline(ss, token, ':')) {
        size_t pos = token.find('=');
        colors[token.substr(0, pos)] = token.substr(pos + 1);
    }
}

static int path_common_prefix(const char *path1, const char *path2)
{
    int i = 0;
    int ret = 0;

    if ((path1[1] == '/') != (path2[1] == '/')) {
        return 0;
    }

    while (*path1 && *path2) {
        if (*path1 != *path2) {
            break;
        }

        if (*path1 == '/') {
            ret = i + 1;
        }

        path1++;
        path2++;
        i++;
    }

    if (!*path1 && !*path2) {
        ret = i;
    }

    if (!*path1 && *path2 == '/') {
        ret = i;
    }

    if (!*path2 && *path1 == '/') {
        ret = i;
    }

    return ret;
}

static bool path_prefix(const char *prefix, const char *path)
{
    prefix++;
    path++;

    if (!*prefix) {
        return *path != '/';
    }

    if (*prefix == '/' && !prefix[1]) {
        return *path == '/';
    }

    while (*prefix && *path) {
        if (*prefix != *path) {
            break;
        }

        prefix++;
        path++;
    }

    return (!*prefix && (*path == '/' || !*path));
}

static std::string relpath(const char *can_fname, const char *can_relative_to)
{
    std::string output;

    if (!can_relative_to) {
        return can_fname;
    }

    int common_index = path_common_prefix(can_relative_to, can_fname);

    if (!common_index) {
        return can_fname;
    }

    const char *relto_suffix = can_relative_to + common_index;
    const char *fname_suffix = can_fname + common_index;

    if (*relto_suffix == '/') {
        relto_suffix++;
    }

    if (*fname_suffix == '/') {
        fname_suffix++;
    }

    if (*relto_suffix) {
        output += "..";

        for (; *relto_suffix; ++relto_suffix) {
            if (*relto_suffix == '/') {
                output += "/..";
            }
        }

        if (*fname_suffix) {
            output += "/" + std::string(fname_suffix);
        }
    } else {
        if (*fname_suffix) {
            output += fname_suffix;
        } else {
            output += ".";
        }
    }

    return output;
}

#ifdef USE_GIT
unsigned int dirflags(git_repository *repo, std::string rp, std::string path)
{
    unsigned char flags = GIT_DIR_CLEAN;

    if (!settings.resolve_repos) {
        return flags;
    }

    bool isrepo = false;
    int error;

    git_status_options opts = GIT_STATUS_OPTIONS_INIT;

    opts.show = GIT_STATUS_SHOW_WORKDIR_ONLY;
    opts.flags = (
                     GIT_STATUS_OPT_UPDATE_INDEX |
                     GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                     GIT_STATUS_OPT_INCLUDE_UNMODIFIED |
                     GIT_STATUS_OPT_DISABLE_PATHSPEC_MATCH |
                     GIT_STATUS_OPT_EXCLUDE_SUBMODULES
                 );

    if (repo == nullptr || exists((rp  + path + "/.git").c_str())) {
        isrepo = true;

        opts.flags = GIT_STATUS_OPT_UPDATE_INDEX |
            GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
            GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

        git_buf root = { nullptr };

        error = git_repository_discover(&root, (rp + path).c_str(), 0, nullptr);

        if (error == 0 && root.ptr != nullptr) {
            error = git_repository_open_ext(
                    &repo,
                    root.ptr,
                    GIT_REPOSITORY_OPEN_FROM_ENV,
                    nullptr
                );

            if (error == GIT_ENOTFOUND) {
                git_buf_dispose(&root);
                return NO_FLAGS;
            }

            if (error < 0) {
                fprintf(
                    stderr,
                    "Unable to open git repository at %s\n",
                    root.ptr
                );
                git_buf_dispose(&root);
                return NO_FLAGS;
            }

            const char *wd = git_repository_workdir(repo);
            if (wd == nullptr) {
                return GIT_ISREPO | GIT_DIR_BARE;
            }
            rp = wd;
        } else {
            git_buf_dispose(&root);
            return NO_FLAGS;
        }

        flags |= GIT_ISREPO;

        path.replace(path.begin(), path.begin() + rp.length(), "");
        git_buf_dispose(&root);
    }

    if (repo != nullptr) {
        opts.pathspec.count = 1;

        opts.pathspec.strings = new char *[1];
        opts.pathspec.strings[0] = const_cast<char *>(path.c_str());

        git_status_list *statuses = nullptr;
        error = git_status_list_new(&statuses, repo, &opts);

        if (error == 0 && statuses != nullptr) {
            for (size_t i = 0; i < git_status_list_entrycount(statuses); i++) {
                const git_status_entry *entry = git_status_byindex(statuses, i);
                flags |= GIT_ISTRACKED;

                if (entry->status != 0) {
                    flags |= GIT_DIR_DIRTY;
                    break;
                }
            }

            git_status_list_free(statuses);
        }

        if (isrepo) {
            git_repository_free(repo);
        }
    } else {
        return NO_FLAGS;
    }

    return flags;
}
#endif

Entry *addfile(const char *fpath, const char *file, git_repository *repo,
               const std::string &rp, FlagsList &flagsList)
{
    struct stat st = {0};
    std::string directory = fpath;

    if (!directory.empty()) {
        directory += '/';
    }

    char fullpath[PATH_MAX] = {0};

    stbsp_snprintf(fullpath, PATH_MAX, "%s%s", directory.c_str(), file);

    if ((lstat(&fullpath[0], &st)) < 0) {
        fprintf(stderr, "Unable to get stats for %s\n", &fullpath[0]);
        return nullptr;
    }

    #ifdef S_ISLNK

    if (S_ISLNK(st.st_mode) && settings.resolve_links) {
        char target[PATH_MAX] = {};

        if ((readlink(&fullpath[0], &target[0], sizeof(target))) >= 0) {
            std::string lpath = &target[0];

            if (lpath.at(0) != '/') {
                lpath = std::string(dirname(&fullpath[0])) + "/" + lpath;
            }

            if ((lstat(lpath.c_str(), &st)) < 0) {
                fprintf(
                    stderr,
                    "cannot access '%s': No such file or directory\n",
                    file
                );

                return new Entry(file, &fullpath[0], nullptr, 0);
            }

            strncpy(&fullpath[0], lpath.c_str(), PATH_MAX - 1);
        }
    }

    #endif /* S_ISLNK */

    unsigned int flags = ~0;

    #ifdef USE_GIT

    if (repo != nullptr && settings.resolve_in_repos) {
        char dirpath[PATH_MAX] = {0};

        if (
            realpath((directory + file).c_str(), &dirpath[0]) == nullptr &&
            !S_ISLNK(st.st_mode)
        ) {
            fprintf(
                stderr,
                "cannot access '%s': No such file or directory\n",
                file
            );

            return new Entry(file, &fullpath[0], nullptr, 0);
        }

        if (flagsList.count(&dirpath[0]) > 0) {
            flags = flagsList[&dirpath[0]];
        } else {
            flags = 0;
        }

        if (!S_ISLNK(st.st_mode)) {
            std::string lfpath = &dirpath[0];
            lfpath.replace(lfpath.begin(), lfpath.begin() + rp.length(), "");

            if (lfpath.length() > 0) {
                while (lfpath.at(0) == '/') {
                    lfpath.replace(lfpath.begin(), lfpath.begin() + 1, "");
                }
            }

            if (S_ISDIR(st.st_mode)) {
                /*     git_status_file(&flags, repo, fpath.c_str()); */

                if ((flags & GIT_STATUS_IGNORED) == 0 && lfpath != ".git") {
                    flags |= dirflags(repo, rp, lfpath);
                }
            }

            /* } else { */
            /*     git_status_file(&flags, repo, fpath.c_str()); */
            /* } */
        }
    } else {
        if (S_ISDIR(st.st_mode)) {
            flags = dirflags(nullptr, "", directory + file);
        }
    }

    #endif

    return new Entry(file, &fullpath[0], &st, flags);
}

FileList listdir(const char *path)
{
    FileList lst;
    DIR *dir;

    char rppath[PATH_MAX] = {0};

    if ((dir = opendir(path)) != nullptr) {
        dirent *ent;

        std::string rp;
        git_repository *repo = nullptr;
        FlagsList flagsList = {};

        #ifdef USE_GIT
        git_buf root = { nullptr };

        int error = -1;

        char dirpath[PATH_MAX] = {0};

        error = git_repository_open_ext(
                    &repo,
                    nullptr,
                    GIT_REPOSITORY_OPEN_FROM_ENV,
                    nullptr
                );

        if (error == 0) {
            const char *wd = git_repository_workdir(repo);

            if (wd != nullptr) {
                rp = wd;

                if (realpath(path, &dirpath[0]) == nullptr) {
                    git_repository_free(repo);
                    return lst;
                }

                if (realpath(rp.c_str(), &rppath[0]) == nullptr) {
                    git_repository_free(repo);
                    return lst;
                }

                if (!path_prefix(&rppath[0], &dirpath[0])) {
                    repo = nullptr;
                } else {
                    int ignored = 0;
                    std::string relp = relpath(&dirpath[0], &rppath[0]);

                    git_status_should_ignore(&ignored, repo, relp.c_str());
                    if (ignored != 1) {
                        git_status_file(nullptr, repo, relp.c_str());
                    }
                }
            }

            if (repo == nullptr) {
                rp = "";
                error = -1;
            }
        }

        if (repo != nullptr) {
            git_status_options opts = GIT_STATUS_OPTIONS_INIT;
            opts.flags = (
                             GIT_STATUS_OPT_INCLUDE_IGNORED |
                             GIT_STATUS_OPT_INCLUDE_UNMODIFIED |
                             /* GIT_STATUS_OPT_INCLUDE_UNTRACKED | */
                             GIT_STATUS_OPT_EXCLUDE_SUBMODULES |
                             GIT_STATUS_OPT_DISABLE_PATHSPEC_MATCH
                         );

            git_status_list *list;

            if (realpath(rp.c_str(), &rppath[0]) != nullptr) {
                if (git_status_list_new(&list, repo, &opts) == GIT_OK) {
                    size_t iMax = git_status_list_entrycount(list);

                    #pragma omp parallel for
                    for (size_t i = 0; i < iMax; ++i) {
                        const git_status_entry *status = git_status_byindex(list, i);
                        const char *filePath = (status->head_to_index != nullptr) ?
                                               status->head_to_index->new_file.path :
                                               (status->index_to_workdir != nullptr) ?
                                               status->index_to_workdir->new_file.path :
                                               nullptr;


                        if (filePath != nullptr) {
                            std::string relp = rp + "/" + filePath;

                            if (realpath(relp.c_str(), &dirpath[0]) != nullptr) {
                                #pragma omp critical
                                flagsList[&dirpath[0]] = status->status | GIT_ISTRACKED;
                            }
                        }
                    }

                    git_status_list_free(list);
                }
            }
        }

        #endif

        while ((ent = readdir((dir))) != nullptr) {
            if (
                strcmp(&ent->d_name[0], ".") == 0 ||
                strcmp(&ent->d_name[0], "..") == 0
            ) {
                continue;
            }

            if (ent->d_name[0] == '.' && !settings.show_hidden) {
                continue;
            }

            #pragma omp task shared(lst, repo)
            {
                auto f = addfile(path, &ent->d_name[0], repo, rp, flagsList);

                if (f != nullptr) {
                    #pragma omp critical
                    lst.push_back(f);
                }
            }
        }

        #pragma omp taskwait

        #ifdef USE_GIT

        if (repo != nullptr) {
            flagsList.clear();
            git_repository_free(repo);
            git_buf_dispose(&root);
        }

        #endif

        closedir(dir);
    }

    return lst;
}

void printdir(FileList *lst)
{
    std::sort(lst->begin(), lst->end(), [](const Entry *a, const Entry *b) {
        if (settings.dirs_first) {
            if (a->isdir && !b->isdir) {
                return true;
            }

            if (!a->isdir && b->isdir) {
                return false;
            }
        }

        int64_t cmp = 0;

        if ((settings.sort & SORT_TYPE) == SORT_TYPE) {
            cmp = b->extension.compare(a->extension);
        }

        if (cmp == 0) {
            if ((settings.sort & SORT_ALPHA) == SORT_ALPHA) {
                cmp = b->file.compare(a->file);
            } else if ((settings.sort & SORT_MODIFIED) == SORT_MODIFIED) {
                cmp = a->modified - b->modified;
            } else if ((settings.sort & SORT_SIZE) == SORT_SIZE) {
                cmp = a->bsize - b->bsize;
            }
        }

        if (settings.reversed) {
            return cmp < 0;
        }

        return cmp > 0;
    });

    int maxlen = 0;
    Lengths maxlens;

    for (const auto l : *lst) {
        for (const auto &f : l->processed) {
            maxlens[f.first] = std::max(f.second.second, maxlens[f.first]);
        }

        maxlen = std::max(l->totlen, maxlen);
    }

    struct winsize w = { 0 };

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    maxlen += 1;

    int columns = 0;

    if (settings.forced_columns > 0) {
        columns = settings.forced_columns;
    } else {
        int calc = (
                       static_cast<float>(w.ws_col) /
                       static_cast<float>(maxlen)
                   );
        columns = std::max(calc, 1);
    }

    int current = 0;
    std::string ext;
    std::string output;

    for (const auto l : *lst) {
        int outlen = 0;

        if ((settings.sort & SORT_TYPE) == SORT_TYPE && l->extension != ext) {
            fprintf(stdout, "\n\033[0m%s:\n", l->extension.c_str());
            ext = l->extension;
        }

        std::string curr = l->print(maxlens, &outlen);

        if (outlen < maxlen) {
            curr += std::string(maxlen - outlen, ' ');
        }

        output += curr;
        current++;

        if (current == columns)  {
            fprintf(stdout, "%s\033[0m\n", rtrim(output).c_str());
            current = 0;
            output = "";
        }
    }

    if (current != 0)  {
        fprintf(stdout, "%s\033[0m\n", rtrim(output).c_str());
        output = "";
    }
}

const char *gethome()
{
    const char *homedir = getenv("HOME");

    if (homedir != nullptr) {
        return homedir;
    }

    const struct passwd *result = getpwuid(getuid());

    if (result == nullptr) {
        fprintf(stderr, "Unable to find home-directory\n");
        exit(EXIT_FAILURE);
    }

    homedir = result->pw_dir;

    return homedir;
}

void loadconfig()
{
    dictionary *ini = nullptr;

    if (!settings.no_conf) {
        char filename[PATH_MAX] = {0};
        char file[PATH_MAX] = {"/lsext.ini"};

        const char *confdir = getenv("XDG_CONFIG_HOME");

        if (confdir == nullptr) {
            stbsp_snprintf(&file[0], PATH_MAX, "/.lsext.ini");
            confdir = gethome();
        } else {
            stbsp_snprintf(&filename[0], PATH_MAX, "%s%s", confdir, &file[0]);

            if (!exists(&filename[0])) {
                stbsp_snprintf(&file[0], PATH_MAX, "/.lsext.ini");
                confdir = gethome();
            }
        }

        stbsp_snprintf(&filename[0], PATH_MAX, "%s%s", confdir, &file[0]);

        if (!exists(&filename[0])) {
            stbsp_snprintf(&filename[0], PATH_MAX, "./lsext.ini");
        }

        if (exists(&filename[0])) {
            ini = iniparser_load(&filename[0]);
        }
    }

#define GETBOOL(a,b) static_cast<bool>(iniparser_getboolean(ini,a,b))
#define GETSTR(a,b) cpp11_getstring(ini,a,b)
#define GETINT(a,b) iniparser_getint(ini,a,b)

    settings.forced_columns = 0;

    settings.list_format = GETSTR("symbols:list_format",
                                  " @p    @U  @^r @t  @^s  @G@f");
    settings.format = GETSTR("symbols:format", "@G@F");

    settings.size_number_color = GETBOOL("settings:size_number_color", 1);
    settings.date_number_color = GETBOOL("settings:date_number_color", 1);

    settings.show_hidden = GETBOOL("settings:show_hidden", 0);
    settings.show_hidden = GETBOOL("settings:show_hidden", 0);

    settings.list = GETBOOL("settings:list", 0);

    settings.resolve_links = GETBOOL("settings:resolve_links", 0);
    settings.resolve_mounts = GETBOOL("settings:resolve_mounts", 1);

    settings.resolve_in_repos = GETBOOL("settings:resolve_in_repos", 1);
    settings.resolve_repos = GETBOOL("settings:resolve_repos", 1);
    settings.reversed = GETBOOL("settings:reversed", 0);
    settings.dirs_first = GETBOOL("settings:dirs_first", 1);

    settings.numeric_id = GETBOOL("settings:numeric_id", 0);

    settings.sort = SORT_ALPHA;

    settings.colors = GETBOOL("settings:colors", 1);

    settings.color.suffix.exec.fg = GETINT("colors:suffix_exec_fg", 10);
    settings.color.suffix.dir.fg = GETINT("colors:suffix_dir_fg", -1);
    settings.color.suffix.link.fg = GETINT("colors:suffix_link_fg", -1);
    settings.color.suffix.mountpoint.fg = GETINT("colors:suffix_mountpoint_fg",
                                          -1);

    settings.color.suffix.exec.bg = GETINT("colors:suffix_exec_bg", -1);
    settings.color.suffix.dir.bg = GETINT("colors:suffix_dir_bg", -1);
    settings.color.suffix.link.bg = GETINT("colors:suffix_link_bg", -1);
    settings.color.suffix.mountpoint.bg = GETINT("colors:suffix_mointpoint_fg",
                                          -1);

    settings.color.perm.none.fg = GETINT("colors:perm_none_fg", 0);
    settings.color.perm.exec.fg = GETINT("colors:perm_exec_fg", 2);
    settings.color.perm.read.fg = GETINT("colors:perm_read_fg", 3);
    settings.color.perm.write.fg = GETINT("colors:perm_write_fg", 1);

    settings.color.perm.full.fg = GETINT("colors:perm_full_fg", 15);
    settings.color.perm.readwrite.fg = GETINT("colors:perm_readwrite_fg", 11);
    settings.color.perm.readexec.fg = GETINT("colors:perm_readexec_fg", 6);
    settings.color.perm.writeexec.fg = GETINT("colors:perm_writeexec_fg", 5);

    settings.color.perm.none.bg = GETINT("colors:perm_none_bg", -1);
    settings.color.perm.exec.bg = GETINT("colors:perm_exec_bg", -1);
    settings.color.perm.read.bg = GETINT("colors:perm_read_bg", -1);
    settings.color.perm.write.bg = GETINT("colors:perm_write_bg", -1);

    settings.color.perm.full.bg = GETINT("colors:perm_full_bg", -1);
    settings.color.perm.readwrite.bg = GETINT("colors:perm_readwrite_bg", -1);
    settings.color.perm.readexec.bg = GETINT("colors:perm_readexec_bg", -1);
    settings.color.perm.writeexec.bg = GETINT("colors:perm_writeexec_bg", -1);

    settings.color.perm.dir.fg = GETINT("colors:perm_dir_fg", 4);
    settings.color.perm.link.fg = GETINT("colors:perm_link_fg", 6);
    settings.color.perm.sticky.fg = GETINT("colors:perm_sticky_fg", 5);
    settings.color.perm.special.fg = GETINT("colors:perm_special_fg", 5);
    settings.color.perm.block.fg = GETINT("colors:perm_block_fg", 5);
    settings.color.perm.unknown.fg = GETINT("colors:perm_unknown_fg", 1);
    settings.color.perm.other.fg = GETINT("colors:perm_other_fg", 7);

    settings.color.perm.dir.bg = GETINT("colors:perm_dir_bg", -1);
    settings.color.perm.link.bg = GETINT("colors:perm_link_bg", -1);
    settings.color.perm.sticky.bg = GETINT("colors:perm_sticky_bg", -1);
    settings.color.perm.special.bg = GETINT("colors:perm_special_bg", -1);
    settings.color.perm.block.bg = GETINT("colors:perm_block_bg", -1);
    settings.color.perm.unknown.bg = GETINT("colors:perm_unknown_bg", -1);
    settings.color.perm.other.bg = GETINT("colors:perm_other_bg", -1);

    settings.color.user.user.fg = GETINT("colors:user_fg", 11);
    settings.color.user.group.fg = GETINT("colors:group_fg", 3);
    settings.color.user.separator.fg = GETINT("colors:user_separator_fg", 0);

    settings.color.user.user.bg = GETINT("colors:user_bg", -1);
    settings.color.user.group.bg = GETINT("colors:group_bg", -1);
    settings.color.user.separator.bg = GETINT("colors:user_separator_bg", -1);

    settings.color.size.number.fg = GETINT("colors:size_number_fg", 12);
    settings.color.size.number.bg = GETINT("colors:size_number_bg", -1);

    settings.color.size.byte.fg = GETINT("colors:size_byte_fg", 4);
    settings.color.size.kilo.fg = GETINT("colors:size_kilo_fg", 4);
    settings.color.size.mega.fg = GETINT("colors:size_mega_fg", 4);
    settings.color.size.giga.fg = GETINT("colors:size_giga_fg", 4);
    settings.color.size.tera.fg = GETINT("colors:size_tera_fg", 4);
    settings.color.size.peta.fg = GETINT("colors:size_peta_fg", 4);

    settings.color.size.byte.bg = GETINT("colors:size_byte_bg", -1);
    settings.color.size.kilo.bg = GETINT("colors:size_kilo_bg", -1);
    settings.color.size.mega.bg = GETINT("colors:size_mega_bg", -1);
    settings.color.size.giga.bg = GETINT("colors:size_giga_bg", -1);
    settings.color.size.tera.bg = GETINT("colors:size_tera_bg", -1);
    settings.color.size.peta.bg = GETINT("colors:size_peta_bg", -1);

    settings.color.date.number.fg = GETINT("colors:date_number_fg", 10);
    settings.color.date.number.bg = GETINT("colors:date_number_bg", -1);

    settings.color.date.sec.fg = GETINT("colors:date_sec_fg", 2);
    settings.color.date.min.fg = GETINT("colors:date_min_fg", 2);
    settings.color.date.hour.fg = GETINT("colors:date_hour_fg", 2);
    settings.color.date.day.fg = GETINT("colors:date_day_fg", 2);
    settings.color.date.week.fg = GETINT("colors:date_week_fg", 2);
    settings.color.date.mon.fg = GETINT("colors:date_mon_fg", 2);
    settings.color.date.year.fg = GETINT("colors:date_year_fg", 2);
    settings.color.date.other.fg = GETINT("colors:date_other_fg", 2);

    settings.color.date.sec.bg = GETINT("colors:date_sec_bg", -1);
    settings.color.date.min.bg = GETINT("colors:date_min_bg", -1);
    settings.color.date.hour.bg = GETINT("colors:date_hour_bg", -1);
    settings.color.date.day.bg = GETINT("colors:date_day_bg", -1);
    settings.color.date.week.bg = GETINT("colors:date_week_bg", -1);
    settings.color.date.mon.bg = GETINT("colors:date_mon_bg", -1);
    settings.color.date.year.bg = GETINT("colors:date_year_bg", -1);
    settings.color.date.other.bg = GETINT("colors:date_other_bg", -1);

    settings.symbols.user.separator = GETSTR("symbols:user_separator", ":");

    settings.symbols.suffix.exec = GETSTR("symbols:suffix_exec", "*");
    settings.symbols.suffix.dir = GETSTR("symbols:suffix_dir", "/");
    settings.symbols.suffix.link = GETSTR("symbols:suffix_link", " -> ");
    settings.symbols.suffix.mountpoint = GETSTR("symbols:suffix_mountpoint",
                                         " @ ");

    settings.symbols.size.byte = GETSTR("symbols:size_byte", "B");
    settings.symbols.size.kilo = GETSTR("symbols:size_kilo", "K");
    settings.symbols.size.mega = GETSTR("symbols:size_mega", "M");
    settings.symbols.size.giga = GETSTR("symbols:size_giga", "G");
    settings.symbols.size.tera = GETSTR("symbols:size_tera", "T");
    settings.symbols.size.peta = GETSTR("symbols:size_peta", "P");

    settings.symbols.date.sec = GETSTR("symbols:date_sec", "sec");
    settings.symbols.date.min = GETSTR("symbols:date_min", "min");
    settings.symbols.date.hour = GETSTR("symbols:date_hour", "hour");
    settings.symbols.date.day = GETSTR("symbols:date_day", "day");
    settings.symbols.date.week = GETSTR("symbols:date_week", "week");
    settings.symbols.date.mon = GETSTR("symbols:date_mon", "mon");
    settings.symbols.date.year = GETSTR("symbols:date_year", "year");

    #ifdef USE_GIT
    settings.override_git_repo_color = GETBOOL("settings:override_git_repo_color",
                                       0);
    settings.override_git_dir_color = GETBOOL("settings:override_git_dir_color",
                                      0);

    settings.symbols.git.ignore = GETSTR("symbols:git_ignore", "!");
    settings.symbols.git.conflict = GETSTR("symbols:git_conflict", "X");
    settings.symbols.git.modified = GETSTR("symbols:git_modified", "~");
    settings.symbols.git.renamed = GETSTR("symbols:git_renamed", "R");
    settings.symbols.git.added = GETSTR("symbols:git_added", "+");
    settings.symbols.git.typechange = GETSTR("symbols:git_typechange", "T");
    settings.symbols.git.unreadable = GETSTR("symbols:git_unreadable", "-");
    settings.symbols.git.untracked = GETSTR("symbols:git_untracked", "?");
    settings.symbols.git.unchanged = GETSTR("symbols:git_unchanged", " ");

    settings.symbols.git.dir_dirty = GETSTR("symbols:git_dir_dirty", "!");
    settings.symbols.git.dir_clean = GETSTR("symbols:git_dir_clean", " ");

    settings.symbols.git.repo_dirty = GETSTR("symbols:git_repo_dirty", "!");
    settings.symbols.git.repo_clean = GETSTR("symbols:git_repo_clean", "@");
    settings.symbols.git.repo_bare = GETSTR("symbols:git_repo_bare", "+");

    settings.color.git.ignore.fg = GETINT("colors:git_ignore_fg", 0);
    settings.color.git.conflict.fg = GETINT("colors:git_conflict_fg", 1);
    settings.color.git.modified.fg = GETINT("colors:git_modified_fg", 3);
    settings.color.git.renamed.fg = GETINT("colors:git_renamed_fg", 5);
    settings.color.git.added.fg = GETINT("colors:git_added_fg", 2);
    settings.color.git.typechange.fg = GETINT("colors:git_typechange_fg", 4);
    settings.color.git.unreadable.fg = GETINT("colors:git_unreadable_fg", 9);
    settings.color.git.untracked.fg = GETINT("colors:git_untracked_fg", 8);
    settings.color.git.unchanged.fg = GETINT("colors:git_unchanged_fg", 0);

    settings.color.git.dir_dirty.fg = GETINT("colors:git_dir_dirty_fg", 1);
    settings.color.git.dir_clean.fg = GETINT("colors:git_dir_clean_fg", 0);

    settings.color.git.repo_dirty.fg = GETINT("colors:git_repo_dirty_fg", 1);
    settings.color.git.repo_clean.fg = GETINT("colors:git_repo_clean_fg", 2);
    settings.color.git.repo_bare.fg = GETINT("colors:git_repo_bare_fg", 4);

    settings.color.git.ignore.bg = GETINT("colors:git_ignore_bg", -1);
    settings.color.git.conflict.bg = GETINT("colors:git_conflict_bg", -1);
    settings.color.git.modified.bg = GETINT("colors:git_modified_bg", -1);
    settings.color.git.renamed.bg = GETINT("colors:git_renamed_bg", -1);
    settings.color.git.added.bg = GETINT("colors:git_added_bg", -1);
    settings.color.git.typechange.bg = GETINT("colors:git_typechange_bg", -1);
    settings.color.git.unreadable.bg = GETINT("colors:git_unreadable_bg", -1);
    settings.color.git.untracked.bg = GETINT("colors:git_untracked_bg", -1);
    settings.color.git.unchanged.bg = GETINT("colors:git_unchanged_bg", -1);

    settings.color.git.dir_dirty.bg = GETINT("colors:git_dir_dirty_bg", -1);
    settings.color.git.dir_clean.bg = GETINT("colors:git_dir_clean_bg", -1);

    settings.color.git.repo_dirty.bg = GETINT("colors:git_repo_dirty_bg", -1);
    settings.color.git.repo_clean.bg = GETINT("colors:git_repo_clean_bg", -1);
    settings.color.git.repo_bare.bg = GETINT("colors:git_repo_bare_bg", -1);

    settings.color.git.o_dir_dirty = GETSTR("color:git_dir_dirty", "");
    settings.color.git.o_dir_clean = GETSTR("color:git_dir_clean", "");

    settings.color.git.o_repo_dirty = GETSTR("color:git_repo_dirty", "");
    settings.color.git.o_repo_clean = GETSTR("color:git_repo_clean", "");
    settings.color.git.o_repo_bare = GETSTR("color:git_repo_bare", "");
    #endif

    iniparser_freedict(ini);
}

option long_options[] = {
    {"help", no_argument, nullptr, 'H'},
    {"dirs-first", no_argument, nullptr, 'f'},
    {"forced-columns", required_argument, nullptr, 'c'},
    {"format", required_argument, nullptr, 'F'},
    {"list", no_argument, nullptr, 'l'},
    {"no-color", no_argument, nullptr, 'C'},
    {"resolve-links", no_argument, nullptr, 'L'},
    {"resolve-mounts", no_argument, nullptr, 'M'},
    {"resolve-in-repos", no_argument, nullptr, 'g'},
    {"resolve-repos", no_argument, nullptr, 'G'},
    {"reversed", no_argument, nullptr, 'r'},
    {"show-hidden", no_argument, nullptr, 'a'},
    {"sort-date", no_argument, nullptr, 't'},
    {"sort-name", no_argument, nullptr, 'A'},
    {"sort-size", no_argument, nullptr, 'S'},
    {"sort-type", no_argument, nullptr, 'X'},
    {"numeric-uid-gid", no_argument, nullptr, 'n'},
    {nullptr, 0, nullptr, 0}
};

void printHelp()
{
    printf("--help\n");

    for (int i = 1; long_options[i].name != 0; i++) {
        if (long_options[i].has_arg != no_argument) {
            printf("-%c \"option\" --%s=\"option\"\n", long_options[i].val,
                   long_options[i].name);
        } else {
            printf("-%c --%s\n", long_options[i].val, long_options[i].name);
        }
    }
}

int main(int argc, const char *argv[])
{
    FileList files;
    DirList dirs;

    settings.no_conf = false;


    loadconfig();

    bool parse = true;

    while (parse) {
        int c = getopt_long(argc, const_cast<char **>(argv), "c:LMGgarfXtSAlnF:C",
                            long_options, 0);

        switch (c) {
            case 'c':
                settings.forced_columns = std::strtol(optarg, nullptr, 10);
                break;

            case 'L':
                settings.resolve_links = !settings.resolve_links;
                break;

            case 'M':
                settings.resolve_mounts = !settings.resolve_mounts;
                break;

            case 'g':
                settings.resolve_in_repos = !settings.resolve_in_repos;
                break;

            case 'G':
                settings.resolve_repos = !settings.resolve_repos;
                break;

            case 'a':
                settings.show_hidden = !settings.show_hidden;
                break;

            case 'r':
                settings.reversed = !settings.reversed;
                break;

            case 'f':
                settings.dirs_first = !settings.dirs_first;
                break;

            case 'X':
                settings.sort |= SORT_TYPE;
                break;

            case 't':
                settings.sort |= SORT_MODIFIED;
                settings.sort &= ~(SORT_SIZE | SORT_ALPHA);
                break;

            case 'S':
                settings.sort |= SORT_SIZE;
                settings.sort &= ~(SORT_MODIFIED | SORT_ALPHA);
                break;

            case 'A':
                settings.sort |= SORT_ALPHA;
                settings.sort &= ~(SORT_SIZE | SORT_MODIFIED);
                break;

            case 'l':
                settings.list = !settings.list;

                if (settings.list) {
                    settings.format = settings.list_format;
                    settings.forced_columns = 1;
                }

                break;

            case 'n':
                settings.numeric_id = !settings.numeric_id;
                break;

            case 'C':
                settings.colors = !settings.colors;
                break;

            case 'N':
                settings.no_conf = true;
                loadconfig();
                break;

            case 'F':
                settings.format = optarg;
                break;

            case 'H':
                printHelp();
                return EXIT_SUCCESS;

            default:
                parse = false;
                break;
        }
    }

    #ifdef USE_GIT
    git_libgit2_init();
    #endif

    if (settings.colors) {
        initcolors();
    }

    if (argc - optind > 0) {
        auto sp = gsl::make_span<const char *>(argv + optind, argv + argc);
        std::sort(sp.begin(), sp.end(), [](const char *a, const char *b) {
            return strlen(a) < strlen(b);
        });

        uint32_t count = argc - optind;

        #pragma omp parallel for
        for (uint32_t i = 0; i < count; i++) {
            struct stat st = {0};

            if ((lstat(gsl::at(sp, i), &st)) < 0) {
                fprintf(stderr, "Unable to open %s!\n", gsl::at(sp, i));
            } else {
                #ifdef S_ISLNK
                char target[PATH_MAX] = {};

                char fullpath[PATH_MAX] = {0};

                stbsp_snprintf(fullpath, PATH_MAX, "%s", gsl::at(sp, i));

                if ((readlink(gsl::at(sp, i), &target[0], sizeof(target))) >= 0) {
                    std::string lpath = &target[0];

                    if (lpath.at(0) != '/') {
                        lpath = std::string(dirname(&fullpath[0])) + "/" + lpath;
                    }

                    lstat(lpath.c_str(), &st);
                }

                #endif

                if (S_ISDIR(st.st_mode)) {
                    dirs.insert(DirList::value_type(
                        gsl::at(sp, i),
                        listdir(gsl::at(sp, i))
                    ));
                } else {
                    FlagsList flagsList = {};

                    #pragma omp critical
                    files.push_back(
                        addfile("", gsl::at(sp, i), nullptr, "", flagsList)
                    );
                }
            }
        }
    } else {
        dirs.insert(DirList::value_type("./", listdir(".")));
    }

    if (!files.empty()) {
        printdir(&files);
    }

    for (auto dir : dirs) {
        if (dirs.size() > 1 || !files.empty()) {
            std::string path = dir.first;

            while (path.back() == '/') {
                path.pop_back();
            }

            fprintf(stdout, "\n\033[0m%s:\n", path.c_str());
        }

        printdir(&dir.second);
    }

    files.clear();
    dirs.clear();

    #ifdef USE_GIT
    git_libgit2_shutdown();
    #endif

    return EXIT_SUCCESS;
}
