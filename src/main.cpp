#include <cstring>
#include <cstdlib>
#include <climits>

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

#include <dirent.h>
#include <libgen.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gsl-lite.h"
#include "entry.hpp"

#ifdef USE_GIT
    #include <git2.h>
#endif

#include <iniparser.h>

using FileList = std::vector<Entry *>;
using DirList = std::map<std::string, FileList>;

settings_t settings = {0};

void initcolors()
{
    const char *ls_colors = std::getenv("LS_COLORS");

    std::stringstream ss;
    ss << ls_colors;

    std::string token;
    Color color;

    while (std::getline(ss, token, ':')) {
        size_t pos = token.find('=');

        color.glob = token.substr(0, pos);
        color.color = token.substr(pos + 1);
        colors.push_back(color);
    }

    /* for (auto c : colors) { */
    /*     printf("\033[%sm%s\033[0m\n", c.color.c_str(), c.glob.c_str()); */
    /* } */
}

#ifdef USE_GIT
unsigned int dirflags(git_repository *repo, std::string rp, std::string path)
{
    unsigned int flags = GIT_DIR_CLEAN;

    if (!settings.resolve_repos) {
        return UINT_MAX;
    }

    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.flags = (
                     GIT_STATUS_OPT_INCLUDE_UNMODIFIED |
                     GIT_STATUS_OPT_EXCLUDE_SUBMODULES
                 );

    if (repo == nullptr) {
        opts.flags = GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

        git_buf root = {0};

        int error = git_repository_discover(&root, path.c_str(), 0, NULL);

        if (error >= 0) {
            error = git_repository_open(&repo, root.ptr);

            if (error < 0) {
                fprintf(stderr, "Unable to open git repository at %s", root.ptr);
                git_buf_free(&root);
                return UINT_MAX;
            }

            std::regex root_re("/\\.git$");

            rp = std::regex_replace(
                     root.ptr,
                     root_re,
                     "",
                     std::regex_constants::format_default
                 );
        } else {
            git_buf_free(&root);
            return UINT_MAX;
        }

        flags |= GIT_ISREPO;

        path.replace(path.begin(), path.begin() + rp.length(), "");
        git_buf_free(&root);
    }

    opts.pathspec.count = 1;
    opts.pathspec.strings = new char *[1];
    opts.pathspec.strings[0] = const_cast<char *>(path.c_str());

    git_status_list *statuses = NULL;
    git_status_list_new(&statuses, repo, &opts);

    size_t count = git_status_list_entrycount(statuses);

    for (size_t i = 0; i < count; ++i) {
        const git_status_entry *entry = git_status_byindex(statuses, i);

        if (entry->status != 0) {
            flags |= GIT_DIR_DIRTY;
            break;
        }
    }

    git_repository_free(repo);
    return flags;
}
#endif

Entry *addfile(const char *path, const char *file)
{
    struct stat st = {0};
    std::string directory = path;

    if (!directory.empty()) {
        directory += '/';
    }

    #ifdef USE_GIT
    std::string rp;
    git_buf root = {0};
    git_repository *repo = nullptr;

    int error = git_repository_discover(&root, directory.c_str(), 0, NULL);

    if (error == 0) {
        error = git_repository_open(&repo, root.ptr);

        if (error != 0) {
            fprintf(stderr, "Unable to open git repository at %s", root.ptr);
            return nullptr;
        }

        std::regex root_re("/\\.git$");

        rp = std::regex_replace(
                 root.ptr,
                 root_re,
                 "",
                 std::regex_constants::format_default
             );
    }

    #endif

    char fullpath[PATH_MAX] = {0};
    snprintf(&fullpath[0], PATH_MAX, "%s%s", directory.c_str(), file);

    if ((lstat(&fullpath[0], &st)) < 0) {
        fprintf(stderr, "Unable to get stats for %s\n", &fullpath[0]);

        #ifdef USE_GIT
        git_repository_free(repo);
        git_buf_free(&root);
        #endif

        return nullptr;
    }

    #ifdef S_ISLNK

    if (S_ISLNK(st.st_mode) && settings.resolve_links) {
        char target[PATH_MAX] = {0};

        if ((readlink(&fullpath[0], &target[0], sizeof(target))) >= 0) {
            std::string lpath = &target[0];

            char linkpath[PATH_MAX] = {0};

            if (target[0] != '/') {
                lpath = directory + "/" + std::string(&target[0]);
            }

            if (realpath(lpath.c_str(), &linkpath[0]) == nullptr) {
                fprintf(
                    stderr,
                    "cannot access '%s': No such file or directory\n",
                    file
                );

                #ifdef USE_GIT
                git_repository_free(repo);
                git_buf_free(&root);
                #endif

                return new Entry(
                           directory,
                           file,
                           &fullpath[0],
                           nullptr
                       );
            }

            if ((lstat(&linkpath[0], &st)) < 0) {
                fprintf(
                    stderr,
                    "cannot access '%s': No such file or directory\n",
                    file
                );

                #ifdef USE_GIT
                git_repository_free(repo);
                git_buf_free(&root);
                #endif

                return new Entry(
                           directory,
                           file,
                           &fullpath[0],
                           nullptr
                       );
            }

            strncpy(&fullpath[0], &linkpath[0], PATH_MAX);
        }
    }

    #endif /* S_ISLNK */

    unsigned int flags = UINT_MAX;

    #ifdef USE_GIT

    if (repo != nullptr) {
        flags = 0;
        char dirpath[PATH_MAX] = {0};

        if (!realpath((directory + file).c_str(), &dirpath[0])) {
            fprintf(
                stderr,
                "cannot access '%s': No such file or directory\n",
                file
            );

            git_repository_free(repo);
            git_buf_free(&root);

            return new Entry(
                       directory,
                       file,
                       &fullpath[0],
                       nullptr
                   );
        }

        std::string path = dirpath;
        path.replace(path.begin(), path.begin() + rp.length(), "");

        if (path.length()) {
            while (path.at(0) == '/') {
                path.replace(path.begin(), path.begin() + 1, "");
            }
        }

        if (S_ISDIR(st.st_mode)) {
            flags = dirflags(repo, rp, path);
        } else {
            git_status_file(&flags, repo, path.c_str());
        }

        /* git_repository_free(repo); */
        git_buf_free(&root);
    } else {
        if (S_ISDIR(st.st_mode)) {
            flags = dirflags(nullptr, "", directory + file);
        }
    }

    #endif

    return new Entry(
               directory,
               file,
               &fullpath[0],
               &st,
               flags
           );
}

FileList listdir(const char *path)
{
    FileList lst;
    DIR *dir;

    if ((dir = opendir(path)) != nullptr) {
        dirent *ent;

        while ((ent = readdir((dir))) != nullptr) {
            if (
                strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0
            ) {
                continue;
            }

            if (ent->d_name[0] == '.' && !settings.show_hidden) {
                continue;
            }

            auto f = addfile(path, &ent->d_name[0]);

            if (f != nullptr) {
                lst.push_back(f);
            }
        }

        closedir(dir);
    }

    return lst;
}

void printdir(FileList *lst)
{
    std::sort(lst->begin(), lst->end(), [](Entry * a, Entry * b) {
        if (settings.dirs_first) {
            if (a->isdir && !b->isdir) {
                return true;
            }

            if (!a->isdir && b->isdir) {
                return false;
            }
        }

        int64_t cmp = 0;

        switch (settings.sort) {
            case SORT_ALPHA:
                cmp = b->file.compare(a->file);
                break;

            case SORT_MODIFIED:
                cmp = a->modified - b->modified;
                break;

            case SORT_SIZE:
                cmp = a->bsize - b->bsize;
                break;
        }

        if (settings.reversed) {
            return cmp < 0;
        } else {
            return cmp > 0;
        }
    });

    if (settings.list) {
        size_t max_user = 0;
        size_t max_date = 0;
        size_t max_date_unit = 0;
        size_t max_size = 0;

        for (const auto l : *lst) {
            max_user = std::max(l->user_len, max_user);
            max_date = std::max(l->date_len,  max_date);
            max_date_unit = std::max(l->date_unit_len, max_date_unit);
            max_size = std::max(l->size_len, max_size);
        }

        for (const auto l : *lst) {
            l->list(max_user, max_date, max_date_unit, max_size);
        }
    } else {
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

        size_t max_len = 0;

        for (const auto l : *lst) {
            max_len = std::max(l->clean_len, max_len);
        }

        int calc = ((float)w.ws_col / (float)max_len);
        int columns = std::max(calc, 1);

        int current = 0;

        for (const auto l : *lst) {
            l->print(max_len);
            current++;

            if (current == columns)  {
                printf("\n");
                current = 0;
            }
        }

        printf("\n");
    }
}

const char *gethome()
{
    const char *homedir = getenv("HOME");

    if (homedir != 0) {
        return homedir;
    }

    struct passwd *result = getpwuid(getuid());

    if (result == 0) {
        fprintf(stderr, "Unable to find home-directory\n");
        exit(EXIT_FAILURE);
    }

    homedir = result->pw_dir;

    return homedir;
}

bool exists(const char *name)
{
    struct stat buffer;
    return (stat(name, &buffer) == 0);
}

const char *cpp11_getstring(dictionary *d, const char *key,
                            const char *def)
{
    return iniparser_getstring(d, key, const_cast<char *>(def));
}

void loadconfig()
{
    dictionary *ini = 0;
    char filename[PATH_MAX] = {0};
    char file[255] = {"/lsext.ini"};

    const char *confdir = getenv("XDG_CONFIG_HOME");

    if (confdir == 0) {
        sprintf(file, "/.lsext.ini");
        confdir = gethome();
    } else {
        sprintf(filename, "%s%s", confdir, file);

        if (!exists(filename)) {
            sprintf(file, "/.lsext.ini");
            confdir = gethome();
        }
    }

    sprintf(filename, "%s%s", confdir, file);

    if (!exists(filename)) {
        sprintf(filename, "./lsext.ini"); // Useful when debugging
    }

    if (exists(filename)) {
        ini = iniparser_load(filename);
    }

    settings.size_number_color = iniparser_getboolean(ini,
                                 "settings:size_number_color", true);
    settings.date_number_color = iniparser_getboolean(ini,
                                 "settings:date_number_color", true);

    settings.show_hidden = iniparser_getboolean(ini, "settings:show_hidden",
                           false);
    settings.show_hidden = iniparser_getboolean(ini, "settings:show_hidden",
                           false);
    settings.list = iniparser_getboolean(ini, "settings:list", false);
    settings.resolve_links = iniparser_getboolean(ini, "settings:resolve_links",
                             false);
    settings.resolve_repos = iniparser_getboolean(ini, "settings:resolve_repos",
                             true);
    settings.reversed = iniparser_getboolean(ini, "settings:reversed", false);
    settings.dirs_first = iniparser_getboolean(ini, "settings:dirs_first", true);
    settings.sort = static_cast<sort_t>(
                        iniparser_getint(ini, "settings:sort", SORT_ALPHA)
                    );

    settings.colors = iniparser_getboolean(ini, "settings:colors", true);

    settings.color.suffix.exec.fg = iniparser_getint(ini, "colors:suffix_exec_fg",
                                    10);
    settings.color.suffix.dir.fg = iniparser_getint(ini, "colors:suffix_dir_fg",
                                   -1);
    settings.color.suffix.link.fg = iniparser_getint(ini, "colors:suffix_link_fg",
                                    -1);

    settings.color.suffix.exec.bg = iniparser_getint(ini, "colors:suffix_exec_bg",
                                    -1);
    settings.color.suffix.dir.bg = iniparser_getint(ini, "colors:suffix_dir_bg",
                                   -1);
    settings.color.suffix.link.bg = iniparser_getint(ini, "colors:suffix_link_bg",
                                    -1);

    settings.color.perm.none.fg = iniparser_getint(ini, "colors:perm_none_fg", 0);
    settings.color.perm.exec.fg = iniparser_getint(ini, "colors:perm_exec_fg", 2);
    settings.color.perm.read.fg = iniparser_getint(ini, "colors:perm_read_fg", 3);
    settings.color.perm.write.fg = iniparser_getint(ini, "colors:perm_write_fg",
                                   1);

    settings.color.perm.none.bg = iniparser_getint(ini, "colors:perm_none_bg",
                                  -1);
    settings.color.perm.exec.bg = iniparser_getint(ini, "colors:perm_exec_bg",
                                  -1);
    settings.color.perm.read.bg = iniparser_getint(ini, "colors:perm_read_bg",
                                  -1);
    settings.color.perm.write.bg = iniparser_getint(ini, "colors:perm_write_bg",
                                   -1);

    settings.color.perm.dir.fg = iniparser_getint(ini, "colors:perm_dir_fg", 4);
    settings.color.perm.link.fg = iniparser_getint(ini, "colors:perm_link_fg", 6);
    settings.color.perm.sticky.fg = iniparser_getint(ini, "colors:perm_sticky_fg",
                                    5);
    settings.color.perm.special.fg = iniparser_getint(ini,
                                     "colors:perm_special_fg", 5);
    settings.color.perm.block.fg = iniparser_getint(ini, "colors:perm_block_fg",
                                   5);
    settings.color.perm.unknown.fg = iniparser_getint(ini,
                                     "colors:perm_unknown_fg", 1);
    settings.color.perm.other.fg = iniparser_getint(ini, "colors:perm_other_fg",
                                   7);

    settings.color.perm.dir.bg = iniparser_getint(ini, "colors:perm_dir_bg", -1);
    settings.color.perm.link.bg = iniparser_getint(ini, "colors:perm_link_bg",
                                  -1);
    settings.color.perm.sticky.bg = iniparser_getint(ini, "colors:perm_sticky_bg",
                                    -1);
    settings.color.perm.special.bg = iniparser_getint(ini,
                                     "colors:perm_special_bg", -1);
    settings.color.perm.block.bg = iniparser_getint(ini, "colors:perm_block_bg",
                                   -1);
    settings.color.perm.unknown.bg = iniparser_getint(ini,
                                     "colors:perm_unknown_bg", -1);
    settings.color.perm.other.bg = iniparser_getint(ini, "colors:perm_other_bg",
                                   -1);

    settings.color.user.user.fg = iniparser_getint(ini, "colors:user_fg", 11);
    settings.color.user.group.fg = iniparser_getint(ini, "colors:group_fg", 3);
    settings.color.user.separator.fg = iniparser_getint(ini,
                                       "colors:user_separator_fg", 0);

    settings.color.user.user.bg = iniparser_getint(ini, "colors:user_bg", -1);
    settings.color.user.group.bg = iniparser_getint(ini, "colors:group_bg", -1);
    settings.color.user.separator.bg = iniparser_getint(ini,
                                       "colors:user_separator_bg", -1);

    settings.color.size.number.fg = iniparser_getint(ini, "colors:size_number_fg",
                                    12);
    settings.color.size.number.bg = iniparser_getint(ini, "colors:size_number_bg",
                                    -1);

    settings.color.size.byte.fg = iniparser_getint(ini, "colors:size_byte_fg", 4);
    settings.color.size.kilo.fg = iniparser_getint(ini, "colors:size_kilo_fg", 4);
    settings.color.size.mega.fg = iniparser_getint(ini, "colors:size_mega_fg", 4);
    settings.color.size.giga.fg = iniparser_getint(ini, "colors:size_giga_fg", 4);
    settings.color.size.tera.fg = iniparser_getint(ini, "colors:size_tera_fg", 4);
    settings.color.size.peta.fg = iniparser_getint(ini, "colors:size_peta_fg", 4);

    settings.color.size.byte.bg = iniparser_getint(ini, "colors:size_byte_bg",
                                  -1);
    settings.color.size.kilo.bg = iniparser_getint(ini, "colors:size_kilo_bg",
                                  -1);
    settings.color.size.mega.bg = iniparser_getint(ini, "colors:size_mega_bg",
                                  -1);
    settings.color.size.giga.bg = iniparser_getint(ini, "colors:size_giga_bg",
                                  -1);
    settings.color.size.tera.bg = iniparser_getint(ini, "colors:size_tera_bg",
                                  -1);
    settings.color.size.peta.bg = iniparser_getint(ini, "colors:size_peta_bg",
                                  -1);

    settings.color.date.number.fg = iniparser_getint(ini, "colors:date_number_fg",
                                    10);
    settings.color.date.number.bg = iniparser_getint(ini, "colors:date_number_bg",
                                    -1);

    settings.color.date.sec.fg = iniparser_getint(ini, "colors:date_sec_fg", 2);
    settings.color.date.min.fg = iniparser_getint(ini, "colors:date_min_fg", 2);
    settings.color.date.hour.fg = iniparser_getint(ini, "colors:date_hour_fg", 2);
    settings.color.date.day.fg = iniparser_getint(ini, "colors:date_day_fg", 2);
    settings.color.date.mon.fg = iniparser_getint(ini, "colors:date_mon_fg", 2);
    settings.color.date.year.fg = iniparser_getint(ini, "colors:date_year_fg", 2);
    settings.color.date.other.fg = iniparser_getint(ini, "colors:date_other_fg",
                                   2);

    settings.color.date.sec.bg = iniparser_getint(ini, "colors:date_sec_bg", -1);
    settings.color.date.min.bg = iniparser_getint(ini, "colors:date_min_bg", -1);
    settings.color.date.hour.bg = iniparser_getint(ini, "colors:date_hour_bg",
                                  -1);
    settings.color.date.day.bg = iniparser_getint(ini, "colors:date_day_bg", -1);
    settings.color.date.mon.bg = iniparser_getint(ini, "colors:date_mon_bg", -1);
    settings.color.date.year.bg = iniparser_getint(ini, "colors:date_year_bg",
                                  -1);
    settings.color.date.other.bg = iniparser_getint(ini, "colors:date_other_bg",
                                   -1);

    settings.symbols.user.separator = cpp11_getstring(ini,
                                      "symbols:user_separator", ":");

    settings.symbols.suffix.exec = cpp11_getstring(ini, "symbols:prefix_exec",
                                   "*");
    settings.symbols.suffix.dir = cpp11_getstring(ini, "symbols:prefix_exec",
                                  "/");
    settings.symbols.suffix.link = cpp11_getstring(ini, "symbols:prefix_exec",
                                   "@");

    settings.symbols.size.byte = cpp11_getstring(ini, "symbols:size_byte", "B");
    settings.symbols.size.kilo = cpp11_getstring(ini, "symbols:size_kilo", "K");
    settings.symbols.size.mega = cpp11_getstring(ini, "symbols:size_mega", "M");
    settings.symbols.size.giga = cpp11_getstring(ini, "symbols:size_giga", "G");
    settings.symbols.size.tera = cpp11_getstring(ini, "symbols:size_tera", "T");
    settings.symbols.size.peta = cpp11_getstring(ini, "symbols:size_peta", "P");

    settings.symbols.date.sec = cpp11_getstring(ini, "symbols:date_sec", "sec");
    settings.symbols.date.min = cpp11_getstring(ini, "symbols:date_min", "min");
    settings.symbols.date.hour = cpp11_getstring(ini, "symbols:date_hour",
                                 "hour");
    settings.symbols.date.day = cpp11_getstring(ini, "symbols:date_day", "day");
    settings.symbols.date.mon = cpp11_getstring(ini, "symbols:date_mon", "mon");
    settings.symbols.date.year = cpp11_getstring(ini, "symbols:date_year",
                                 "year");

    #ifdef USE_GIT
    settings.symbols.git.ignore = cpp11_getstring(ini, "symbols:git_ignore", "!");
    settings.symbols.git.conflict = cpp11_getstring(ini, "symbols:git_conflict",
                                    "X");
    settings.symbols.git.modified = cpp11_getstring(ini, "symbols:git_modified",
                                    "~");
    settings.symbols.git.renamed = cpp11_getstring(ini, "symbols:git_renamed",
                                   "R");
    settings.symbols.git.added = cpp11_getstring(ini, "symbols:git_added", "+");
    settings.symbols.git.typechange = cpp11_getstring(ini,
                                      "symbols:git_typechange", "T");
    settings.symbols.git.unreadable = cpp11_getstring(ini,
                                      "symbols:git_unreadable", "-");
    settings.symbols.git.untracked = cpp11_getstring(ini, "symbols:git_untracked",
                                     "?");
    settings.symbols.git.unchanged = cpp11_getstring(ini, "symbols:git_unchanged",
                                     " ");

    settings.symbols.git.dir_dirty = cpp11_getstring(ini, "symbols:git_dir_dirty",
                                     "!");
    settings.symbols.git.dir_clean = cpp11_getstring(ini, "symbols:git_dir_clean",
                                     " ");
    settings.symbols.git.repo_dirty = cpp11_getstring(ini,
                                      "symbols:git_repo_dirty", "!");
    settings.symbols.git.repo_clean = cpp11_getstring(ini,
                                      "symbols:git_repo_clean", "@");

    settings.color.git.ignore.fg = iniparser_getint(ini, "colors:git_ignore_fg",
                                   0);
    settings.color.git.conflict.fg = iniparser_getint(ini,
                                     "colors:git_conflict_fg", 1);
    settings.color.git.modified.fg = iniparser_getint(ini,
                                     "colors:git_modified_fg", 3);
    settings.color.git.renamed.fg = iniparser_getint(ini, "colors:git_renamed_fg",
                                    5);
    settings.color.git.added.fg = iniparser_getint(ini, "colors:git_added_fg", 2);
    settings.color.git.typechange.fg = iniparser_getint(ini,
                                       "colors:git_typechange_fg", 4);
    settings.color.git.unreadable.fg = iniparser_getint(ini,
                                       "colors:git_unreadable_fg", 9);
    settings.color.git.untracked.fg = iniparser_getint(ini,
                                      "colors:git_untracked_fg", 8);
    settings.color.git.unchanged.fg = iniparser_getint(ini,
                                      "colors:git_unchanged_fg", 0);

    settings.color.git.dir_dirty.fg = iniparser_getint(ini,
                                      "colors:git_dir_dirty_fg", 1);
    settings.color.git.dir_clean.fg = iniparser_getint(ini,
                                      "colors:git_dir_clean_fg", 0);
    settings.color.git.repo_dirty.fg = iniparser_getint(ini,
                                       "colors:git_repo_dirty_fg", 1);
    settings.color.git.repo_clean.fg = iniparser_getint(ini,
                                       "colors:git_repo_clean_fg", 2);

    settings.color.git.ignore.bg = iniparser_getint(ini, "colors:git_ignore_bg",
                                   -1);
    settings.color.git.conflict.bg = iniparser_getint(ini,
                                     "colors:git_conflict_bg", -1);
    settings.color.git.modified.bg = iniparser_getint(ini,
                                     "colors:git_modified_bg", -1);
    settings.color.git.renamed.bg = iniparser_getint(ini, "colors:git_renamed_bg",
                                    -1);
    settings.color.git.added.bg = iniparser_getint(ini, "colors:git_added_bg",
                                  -1);
    settings.color.git.typechange.bg = iniparser_getint(ini,
                                       "colors:git_typechange_bg", -1);
    settings.color.git.unreadable.bg = iniparser_getint(ini,
                                       "colors:git_unreadable_bg", -1);
    settings.color.git.untracked.bg = iniparser_getint(ini,
                                      "colors:git_untracked_bg", -1);
    settings.color.git.unchanged.bg = iniparser_getint(ini,
                                      "colors:git_unchanged_bg", -1);

    settings.color.git.dir_dirty.bg = iniparser_getint(ini,
                                      "colors:git_dir_dirty_bg", -1);
    settings.color.git.dir_clean.bg = iniparser_getint(ini,
                                      "colors:git_dir_clean_bg", -1);
    settings.color.git.repo_dirty.bg = iniparser_getint(ini,
                                       "colors:git_repo_dirty_bg", -1);
    settings.color.git.repo_clean.bg = iniparser_getint(ini,
                                       "colors:git_repo_clean_bg", -1);
    #endif

    iniparser_freedict(ini);
}

int main(int argc, const char *argv[])
{
    FileList files;
    DirList dirs;

    loadconfig();

    bool parse = true;

    while (parse) {
        int c = getopt(argc, const_cast<char **>(argv), "AalrtfSLnh");

        switch (c) {
            case 'L':
                settings.resolve_links = !settings.resolve_links;
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

            case 't':
                settings.sort = SORT_MODIFIED;
                break;

            case 'S':
                settings.sort = SORT_SIZE;
                break;

            case 'A':
                settings.sort = SORT_ALPHA;
                break;

            case 'l':
                settings.list = !settings.list;
                break;

            case 'n':
                settings.colors = !settings.colors;
                break;

            case 'h':
                printf("\nTODO: Add help.\n\n");
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

        for (int i = 0; i < argc - optind; i++) {
            struct stat st = {0};

            if ((lstat(sp.at(i), &st)) < 0) {
                return EXIT_FAILURE;
            }

            if (S_ISDIR(st.st_mode)) {
                dirs.insert(DirList::value_type(sp.at(i), listdir(sp.at(i))));
            } else {
                char file[PATH_MAX] = {0};
                strncpy(&file[0], sp.at(i), PATH_MAX);

                files.push_back(addfile("", &file[0]));
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
            fprintf(stdout, "\n%s:\n", dir.first.c_str());
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
