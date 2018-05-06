#include <re2/re2.h>

#include "entry.hpp"
#include "gsl-lite.h"

extern "C" {
    #include <dirent.h>
    #include <libgen.h>
    #include <pwd.h>
    #include <sys/ioctl.h>
    #include <unistd.h>

    #ifdef USE_OPENMP
        #include <omp.h>
    #else
        using omp_int_t = int;
        inline omp_int_t omp_get_thread_num() { return 0;}
        inline omp_int_t omp_get_num_threads() { return 1;}
    #endif

    #ifdef USE_GIT
        #include <git2.h>
    #endif
}

using FileList = std::vector<Entry *>;
using DirList = std::unordered_map<std::string, FileList>;

static re2::RE2 git_re("/\\.git/?$");

settings_t settings = {0}; // NOLINT

void initcolors()
{
    const char *ls_colors = std::getenv("LS_COLORS");

    std::stringstream ss; // NOLINT
    ss << ls_colors;

    std::string token;

    while (std::getline(ss, token, ':')) {
        size_t pos = token.find('='); // NOLINT
        colors[token.substr(0, pos)] = token.substr(pos + 1); // NOLINT
    }

    /* for (auto c : colors) { */
    /*     printf( */
    /*         "\033[%sm\\033[%sm%s\033[0m\n", */
    /*         c.second.c_str(), */
    /*         c.second.c_str(), */
    /*         c.first.c_str() */
    /*     ); */
    /* } */
}

#ifdef USE_GIT
unsigned int dirflags(git_repository *repo, std::string rp, std::string path)
{
    unsigned int flags = GIT_DIR_CLEAN;

    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.flags = (
                     GIT_STATUS_OPT_INCLUDE_UNMODIFIED |
                     GIT_STATUS_OPT_EXCLUDE_SUBMODULES
                 );

    if (repo == nullptr) {
        opts.flags = GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

        git_buf root = { nullptr };

        int error = git_repository_discover(&root, path.c_str(), 0, nullptr);

        if (error == 0) {
            error = git_repository_open(&repo, root.ptr);

            if (error < 0) {
                // NOLINTNEXTLINE
                fprintf(stderr, "Unable to open git repository at %s", root.ptr);
                git_buf_free(&root);
                return UINT_MAX;
            }

            rp = root.ptr;
            re2::RE2::Replace(&rp, git_re, "");
        } else {
            git_buf_free(&root);
            return UINT_MAX;
        }

        flags |= GIT_ISREPO;

        path.replace(path.begin(), path.begin() + rp.length(), "");
        git_buf_free(&root);
    }

    opts.pathspec.count = 1;

    opts.pathspec.strings = new char *[1]; // NOLINT
    opts.pathspec.strings[0] = const_cast<char*>(path.c_str()); // NOLINT

    git_status_list *statuses = nullptr;
    int error = git_status_list_new(&statuses, repo, &opts);

    if (error < 0) { // Probably bare repo
        git_repository_free(repo);
        return GIT_ISREPO | GIT_DIR_BARE;
    }

    size_t count = git_status_list_entrycount(statuses);

    #pragma omp parallel shared(flags, count)
    {
        int i = omp_get_thread_num() * count / omp_get_num_threads();
        int stop = (omp_get_thread_num() + 1) * count / omp_get_num_threads();   

        for (; i < stop && (flags & GIT_DIR_DIRTY) == 0; ++i) {
            const git_status_entry *entry = git_status_byindex(statuses, i);

            if (entry->status != 0) {
                flags |= GIT_DIR_DIRTY;
            }
        }
    }

    git_repository_free(repo);
    return flags;
}
#endif

Entry *addfile(const char *fpath, const char *file)
{
    struct stat st = {0};
    std::string directory = fpath; // NOLINT

    if (!directory.empty()) {
        directory += '/';
    }

    #ifdef USE_GIT
    std::string rp;
    git_buf root = { nullptr };
    git_repository *repo = nullptr;

    int error = git_repository_discover(&root, directory.c_str(), 0, nullptr);

    if (error == 0) {
        error = git_repository_open(&repo, root.ptr);

        if (error != 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Unable to open git repository at %s", root.ptr);
            return nullptr;
        }

        rp = root.ptr;
        re2::RE2::Replace(&rp, git_re, "");
    }

    #endif

    char fullpath[PATH_MAX] = {0};

    // NOLINTNEXTLINE
    snprintf(fullpath, PATH_MAX, "%s%s", directory.c_str(), file);

    if ((lstat(&fullpath[0], &st)) < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Unable to get stats for %s\n", &fullpath[0]);

        #ifdef USE_GIT
        git_repository_free(repo);
        git_buf_free(&root);
        #endif

        return nullptr;
    }

    #ifdef S_ISLNK

    if (S_ISLNK(st.st_mode) && settings.resolve_links) { // NOLINT
        char target[PATH_MAX] = {};
        std::string lpath;

        if ((readlink(&fullpath[0], &target[0], sizeof(target))) >= 0) {
            lpath = &target[0];
            if (lpath.at(0) != '/') {
                // NOLINTNEXTLINE
                lpath = std::string(dirname(&fullpath[0])) + "/" + lpath;
            }

            if ((lstat(lpath.c_str(), &st)) < 0) {
                // NOLINTNEXTLINE
                fprintf(
                    stderr,
                    "cannot access '%s': No such file or directory\n",
                    file
                );

                #ifdef USE_GIT
                git_repository_free(repo);
                git_buf_free(&root);
                #endif

                return new Entry(file, &fullpath[0], nullptr, 0); // NOLINT
            }

            strncpy(&fullpath[0], lpath.c_str(), PATH_MAX);
        }
    }

    #endif /* S_ISLNK */

    unsigned int flags = UINT_MAX;

    #ifdef USE_GIT

    if (repo != nullptr) {
        flags = 0;
        char dirpath[PATH_MAX] = {0};

        if (
            realpath((directory + file).c_str(), &dirpath[0]) == nullptr &&
            !S_ISLNK(st.st_mode) // NOLINT
        ) {
            // NOLINTNEXTLINE
            fprintf(
                stderr,
                "cannot access '%s': No such file or directory\n",
                file
            );

            git_repository_free(repo);
            git_buf_free(&root);

            return new Entry(file, &fullpath[0], nullptr, 0);
        }

        if (!S_ISLNK(st.st_mode)) { // NOLINT
            std::string fpath = &dirpath[0]; // NOLINT
            fpath.replace(fpath.begin(), fpath.begin() + rp.length(), "");

            if (fpath.length() > 0) {
                while (fpath.at(0) == '/') {
                    fpath.replace(fpath.begin(), fpath.begin() + 1, "");
                }
            }

            if (S_ISDIR(st.st_mode)) { // NOLINT
                git_status_file(&flags, repo, fpath.c_str());
                flags |= dirflags(repo, rp, fpath);
            } else {
                git_status_file(&flags, repo, fpath.c_str());
            }

            /* git_repository_free(repo); */
            git_buf_free(&root);
        }
    } else {
        if (S_ISDIR(st.st_mode)) { // NOLINT
            flags = dirflags(nullptr, "", directory + file); // NOLINT
        }
    }

    #endif

    return new Entry(
               file,
               &fullpath[0],
               &st,
               flags
           );
}

FileList listdir(const char *path, const std::string &fp, bool hidden)
{
    FileList lst;
    DIR* dir;

    if ((dir = opendir(path)) != nullptr) {
        dirent * ent;

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
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w); // NOLINT

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
    std::string output = "";

    for (const auto l : *lst) {
        int outlen = 0;
        std::string curr = l->print(maxlens, &outlen);

        if (outlen < maxlen) {
            curr += std::string(maxlen - outlen, ' ');
        }

        output += curr;
        current++;

        if (current == columns)  {
            printf("%s\033[0m\n", rtrim(output).c_str());
            current = 0;
            output = "";
        }
    }

    if (current != 0)  {
        printf("%s\033[0m\n", rtrim(output).c_str());
        current = 0;
        output = "";
    }
}

const char *gethome()
{
    const char *homedir = getenv("HOME");

    if (homedir != nullptr) {
        return homedir;
    }

    struct passwd *result = getpwuid(getuid());

    if (result == nullptr) {
        fprintf(stderr, "Unable to find home-directory\n"); // NOLINT
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
        char file[255] = {"/lsext.ini"};

        const char *confdir = getenv("XDG_CONFIG_HOME");

        if (confdir == nullptr) {
            sprintf(&file[0], "/.lsext.ini"); // NOLINT
            confdir = gethome();
        } else {
            sprintf(&filename[0], "%s%s", confdir, &file[0]); // NOLINT

            if (!exists(&filename[0])) {
                sprintf(&file[0], "/.lsext.ini"); // NOLINT
                confdir = gethome();
            }
        }

        sprintf(&filename[0], "%s%s", confdir, &file[0]); // NOLINT

        if (!exists(&filename[0])) {
            sprintf(&filename[0], "./lsext.ini"); // NOLINT
        }

        if (exists(&filename[0])) {
            ini = iniparser_load(&filename[0]);
        }
    }

    settings.forced_columns = 0;

    settings.list_format = cpp11_getstring(ini, "symbols:list_format", " @p    @U  @^r @t  @^s  @G@f");
    settings.format = cpp11_getstring(ini, "symbols:format", "@G@F");

    // NOLINTNEXTLINE
    settings.size_number_color = iniparser_getboolean(ini, "settings:size_number_color", true);
    // NOLINTNEXTLINE
    settings.date_number_color = iniparser_getboolean(ini, "settings:date_number_color", true);

    // NOLINTNEXTLINE
    settings.show_hidden = iniparser_getboolean(ini, "settings:show_hidden", false);
    // NOLINTNEXTLINE
    settings.show_hidden = iniparser_getboolean(ini, "settings:show_hidden", false);

    // NOLINTNEXTLINE
    settings.list = iniparser_getboolean(ini, "settings:list", false);

    // NOLINTNEXTLINE
    settings.resolve_links = iniparser_getboolean(ini, "settings:resolve_links", false);
    // NOLINTNEXTLINE
    settings.resolve_mounts = iniparser_getboolean(ini, "settings:resolve_mounts", true);

    // NOLINTNEXTLINE
    settings.resolve_repos = iniparser_getboolean(ini, "settings:resolve_repos", true);
    // NOLINTNEXTLINE
    settings.reversed = iniparser_getboolean(ini, "settings:reversed", false);
    // NOLINTNEXTLINE
    settings.dirs_first = iniparser_getboolean(ini, "settings:dirs_first", true);

    settings.sort = static_cast<sort_t>(
        iniparser_getint(ini, "settings:sort", SORT_ALPHA)
    );

    // NOLINTNEXTLINE
    settings.colors = iniparser_getboolean(ini, "settings:colors", true);

    settings.color.suffix.exec.fg = iniparser_getint(ini, "colors:suffix_exec_fg", 10);
    settings.color.suffix.dir.fg = iniparser_getint(ini, "colors:suffix_dir_fg", -1);
    settings.color.suffix.link.fg = iniparser_getint(ini, "colors:suffix_link_fg", -1);
    settings.color.suffix.mountpoint.fg = iniparser_getint(ini, "colors:suffix_mountpoint_fg", -1);

    settings.color.suffix.exec.bg = iniparser_getint(ini, "colors:suffix_exec_bg", -1);
    settings.color.suffix.dir.bg = iniparser_getint(ini, "colors:suffix_dir_bg", -1);
    settings.color.suffix.link.bg = iniparser_getint(ini, "colors:suffix_link_bg", -1);
    settings.color.suffix.mountpoint.bg= iniparser_getint(ini, "colors:suffix_mointpoint_fg", -1);

    settings.color.perm.none.fg = iniparser_getint(ini, "colors:perm_none_fg", 0);
    settings.color.perm.exec.fg = iniparser_getint(ini, "colors:perm_exec_fg", 2);
    settings.color.perm.read.fg = iniparser_getint(ini, "colors:perm_read_fg", 3);
    settings.color.perm.write.fg = iniparser_getint(ini, "colors:perm_write_fg", 1);

    settings.color.perm.full.fg = iniparser_getint(ini, "colors:perm_full_fg", 15);
    settings.color.perm.readwrite.fg = iniparser_getint(ini, "colors:perm_readwrite_fg", 11);
    settings.color.perm.readexec.fg = iniparser_getint(ini, "colors:perm_readexec_fg", 6);
    settings.color.perm.writeexec.fg = iniparser_getint(ini, "colors:perm_writeexec_fg", 5);

    settings.color.perm.none.bg = iniparser_getint(ini, "colors:perm_none_bg", -1);
    settings.color.perm.exec.bg = iniparser_getint(ini, "colors:perm_exec_bg", -1);
    settings.color.perm.read.bg = iniparser_getint(ini, "colors:perm_read_bg", -1);
    settings.color.perm.write.bg = iniparser_getint(ini, "colors:perm_write_bg", -1);

    settings.color.perm.full.bg = iniparser_getint(ini, "colors:perm_full_bg", -1);
    settings.color.perm.readwrite.bg = iniparser_getint(ini, "colors:perm_readwrite_bg", -1);
    settings.color.perm.readexec.bg = iniparser_getint(ini, "colors:perm_readexec_bg", -1);
    settings.color.perm.writeexec.bg = iniparser_getint(ini, "colors:perm_writeexec_bg", -1);

    settings.color.perm.dir.fg = iniparser_getint(ini, "colors:perm_dir_fg", 4);
    settings.color.perm.link.fg = iniparser_getint(ini, "colors:perm_link_fg", 6);
    settings.color.perm.sticky.fg = iniparser_getint(ini, "colors:perm_sticky_fg", 5);
    settings.color.perm.special.fg = iniparser_getint(ini, "colors:perm_special_fg", 5);
    settings.color.perm.block.fg = iniparser_getint(ini, "colors:perm_block_fg", 5);
    settings.color.perm.unknown.fg = iniparser_getint(ini, "colors:perm_unknown_fg", 1);
    settings.color.perm.other.fg = iniparser_getint(ini, "colors:perm_other_fg", 7);

    settings.color.perm.dir.bg = iniparser_getint(ini, "colors:perm_dir_bg", -1);
    settings.color.perm.link.bg = iniparser_getint(ini, "colors:perm_link_bg", -1);
    settings.color.perm.sticky.bg = iniparser_getint(ini, "colors:perm_sticky_bg", -1);
    settings.color.perm.special.bg = iniparser_getint(ini, "colors:perm_special_bg", -1);
    settings.color.perm.block.bg = iniparser_getint(ini, "colors:perm_block_bg", -1);
    settings.color.perm.unknown.bg = iniparser_getint(ini, "colors:perm_unknown_bg", -1);
    settings.color.perm.other.bg = iniparser_getint(ini, "colors:perm_other_bg", -1);

    settings.color.user.user.fg = iniparser_getint(ini, "colors:user_fg", 11);
    settings.color.user.group.fg = iniparser_getint(ini, "colors:group_fg", 3);
    settings.color.user.separator.fg = iniparser_getint(ini, "colors:user_separator_fg", 0);

    settings.color.user.user.bg = iniparser_getint(ini, "colors:user_bg", -1);
    settings.color.user.group.bg = iniparser_getint(ini, "colors:group_bg", -1);
    settings.color.user.separator.bg = iniparser_getint(ini, "colors:user_separator_bg", -1);

    settings.color.size.number.fg = iniparser_getint(ini, "colors:size_number_fg", 12);
    settings.color.size.number.bg = iniparser_getint(ini, "colors:size_number_bg", -1);

    settings.color.size.byte.fg = iniparser_getint(ini, "colors:size_byte_fg", 4);
    settings.color.size.kilo.fg = iniparser_getint(ini, "colors:size_kilo_fg", 4);
    settings.color.size.mega.fg = iniparser_getint(ini, "colors:size_mega_fg", 4);
    settings.color.size.giga.fg = iniparser_getint(ini, "colors:size_giga_fg", 4);
    settings.color.size.tera.fg = iniparser_getint(ini, "colors:size_tera_fg", 4);
    settings.color.size.peta.fg = iniparser_getint(ini, "colors:size_peta_fg", 4);

    settings.color.size.byte.bg = iniparser_getint(ini, "colors:size_byte_bg", -1);
    settings.color.size.kilo.bg = iniparser_getint(ini, "colors:size_kilo_bg", -1);
    settings.color.size.mega.bg = iniparser_getint(ini, "colors:size_mega_bg", -1);
    settings.color.size.giga.bg = iniparser_getint(ini, "colors:size_giga_bg", -1);
    settings.color.size.tera.bg = iniparser_getint(ini, "colors:size_tera_bg", -1);
    settings.color.size.peta.bg = iniparser_getint(ini, "colors:size_peta_bg", -1);

    settings.color.date.number.fg = iniparser_getint(ini, "colors:date_number_fg", 10);
    settings.color.date.number.bg = iniparser_getint(ini, "colors:date_number_bg", -1);

    settings.color.date.sec.fg = iniparser_getint(ini, "colors:date_sec_fg", 2);
    settings.color.date.min.fg = iniparser_getint(ini, "colors:date_min_fg", 2);
    settings.color.date.hour.fg = iniparser_getint(ini, "colors:date_hour_fg", 2);
    settings.color.date.day.fg = iniparser_getint(ini, "colors:date_day_fg", 2);
    settings.color.date.week.fg = iniparser_getint(ini, "colors:date_week_fg", 2);
    settings.color.date.mon.fg = iniparser_getint(ini, "colors:date_mon_fg", 2);
    settings.color.date.year.fg = iniparser_getint(ini, "colors:date_year_fg", 2);
    settings.color.date.other.fg = iniparser_getint(ini, "colors:date_other_fg", 2);

    settings.color.date.sec.bg = iniparser_getint(ini, "colors:date_sec_bg", -1);
    settings.color.date.min.bg = iniparser_getint(ini, "colors:date_min_bg", -1);
    settings.color.date.hour.bg = iniparser_getint(ini, "colors:date_hour_bg", -1);
    settings.color.date.day.bg = iniparser_getint(ini, "colors:date_day_bg", -1);
    settings.color.date.week.bg = iniparser_getint(ini, "colors:date_week_bg", -1);
    settings.color.date.mon.bg = iniparser_getint(ini, "colors:date_mon_bg", -1);
    settings.color.date.year.bg = iniparser_getint(ini, "colors:date_year_bg", -1);
    settings.color.date.other.bg = iniparser_getint(ini, "colors:date_other_bg", -1);

    settings.symbols.user.separator = cpp11_getstring(ini, "symbols:user_separator", ":");

    settings.symbols.suffix.exec = cpp11_getstring(ini, "symbols:suffix_exec", "*");
    settings.symbols.suffix.dir = cpp11_getstring(ini, "symbols:suffix_dir", "/");
    settings.symbols.suffix.link = cpp11_getstring(ini, "symbols:suffix_link", " -> ");
    settings.symbols.suffix.mountpoint = cpp11_getstring(ini, "symbols:suffix_mountpoint", " @ ");

    settings.symbols.size.byte = cpp11_getstring(ini, "symbols:size_byte", "B");
    settings.symbols.size.kilo = cpp11_getstring(ini, "symbols:size_kilo", "K");
    settings.symbols.size.mega = cpp11_getstring(ini, "symbols:size_mega", "M");
    settings.symbols.size.giga = cpp11_getstring(ini, "symbols:size_giga", "G");
    settings.symbols.size.tera = cpp11_getstring(ini, "symbols:size_tera", "T");
    settings.symbols.size.peta = cpp11_getstring(ini, "symbols:size_peta", "P");

    settings.symbols.date.sec = cpp11_getstring(ini, "symbols:date_sec", "sec");
    settings.symbols.date.min = cpp11_getstring(ini, "symbols:date_min", "min");
    settings.symbols.date.hour = cpp11_getstring(ini, "symbols:date_hour", "hour");
    settings.symbols.date.day = cpp11_getstring(ini, "symbols:date_day", "day");
    settings.symbols.date.week = cpp11_getstring(ini, "symbols:date_week", "week");
    settings.symbols.date.mon = cpp11_getstring(ini, "symbols:date_mon", "mon");
    settings.symbols.date.year = cpp11_getstring(ini, "symbols:date_year", "year");

    #ifdef USE_GIT
    // NOLINTNEXTLINE
    settings.override_git_repo_color = iniparser_getboolean(ini, "settings:override_git_repo_color", false);
    // NOLINTNEXTLINE
    settings.override_git_dir_color = iniparser_getboolean(ini, "settings:override_git_dir_color", false);

    settings.symbols.git.ignore = cpp11_getstring(ini, "symbols:git_ignore", "!");
    settings.symbols.git.conflict = cpp11_getstring(ini, "symbols:git_conflict", "X");
    settings.symbols.git.modified = cpp11_getstring(ini, "symbols:git_modified", "~");
    settings.symbols.git.renamed = cpp11_getstring(ini, "symbols:git_renamed", "R");
    settings.symbols.git.added = cpp11_getstring(ini, "symbols:git_added", "+");
    settings.symbols.git.typechange = cpp11_getstring(ini, "symbols:git_typechange", "T");
    settings.symbols.git.unreadable = cpp11_getstring(ini, "symbols:git_unreadable", "-");
    settings.symbols.git.untracked = cpp11_getstring(ini, "symbols:git_untracked", "?");
    settings.symbols.git.unchanged = cpp11_getstring(ini, "symbols:git_unchanged", " ");

    settings.symbols.git.dir_dirty = cpp11_getstring(ini, "symbols:git_dir_dirty", "!");
    settings.symbols.git.dir_clean = cpp11_getstring(ini, "symbols:git_dir_clean", " ");

    settings.symbols.git.repo_dirty = cpp11_getstring(ini, "symbols:git_repo_dirty", "!");
    settings.symbols.git.repo_clean = cpp11_getstring(ini, "symbols:git_repo_clean", "@");
    settings.symbols.git.repo_bare = cpp11_getstring(ini, "symbols:git_repo_bare", "+");

    settings.color.git.ignore.fg = iniparser_getint(ini, "colors:git_ignore_fg", 0);
    settings.color.git.conflict.fg = iniparser_getint(ini, "colors:git_conflict_fg", 1);
    settings.color.git.modified.fg = iniparser_getint(ini, "colors:git_modified_fg", 3);
    settings.color.git.renamed.fg = iniparser_getint(ini, "colors:git_renamed_fg", 5);
    settings.color.git.added.fg = iniparser_getint(ini, "colors:git_added_fg", 2);
    settings.color.git.typechange.fg = iniparser_getint(ini, "colors:git_typechange_fg", 4);
    settings.color.git.unreadable.fg = iniparser_getint(ini, "colors:git_unreadable_fg", 9);
    settings.color.git.untracked.fg = iniparser_getint(ini, "colors:git_untracked_fg", 8);
    settings.color.git.unchanged.fg = iniparser_getint(ini, "colors:git_unchanged_fg", 0);

    settings.color.git.dir_dirty.fg = iniparser_getint(ini, "colors:git_dir_dirty_fg", 1);
    settings.color.git.dir_clean.fg = iniparser_getint(ini, "colors:git_dir_clean_fg", 0);

    settings.color.git.repo_dirty.fg = iniparser_getint(ini, "colors:git_repo_dirty_fg", 1);
    settings.color.git.repo_clean.fg = iniparser_getint(ini, "colors:git_repo_clean_fg", 2);
    settings.color.git.repo_bare.fg = iniparser_getint(ini, "colors:git_repo_bare_fg", 4);

    settings.color.git.ignore.bg = iniparser_getint(ini, "colors:git_ignore_bg", -1);
    settings.color.git.conflict.bg = iniparser_getint(ini, "colors:git_conflict_bg", -1);
    settings.color.git.modified.bg = iniparser_getint(ini, "colors:git_modified_bg", -1);
    settings.color.git.renamed.bg = iniparser_getint(ini, "colors:git_renamed_bg", -1);
    settings.color.git.added.bg = iniparser_getint(ini, "colors:git_added_bg", -1);
    settings.color.git.typechange.bg = iniparser_getint(ini, "colors:git_typechange_bg", -1);
    settings.color.git.unreadable.bg = iniparser_getint(ini, "colors:git_unreadable_bg", -1);
    settings.color.git.untracked.bg = iniparser_getint(ini, "colors:git_untracked_bg", -1);
    settings.color.git.unchanged.bg = iniparser_getint(ini, "colors:git_unchanged_bg", -1);

    settings.color.git.dir_dirty.bg = iniparser_getint(ini, "colors:git_dir_dirty_bg", -1);
    settings.color.git.dir_clean.bg = iniparser_getint(ini, "colors:git_dir_clean_bg", -1);

    settings.color.git.repo_dirty.bg = iniparser_getint(ini, "colors:git_repo_dirty_bg", -1);
    settings.color.git.repo_clean.bg = iniparser_getint(ini, "colors:git_repo_clean_bg", -1);
    settings.color.git.repo_bare.bg = iniparser_getint(ini, "colors:git_repo_bare_bg", -1);

    settings.color.git.o_dir_dirty = cpp11_getstring(ini, "color:git_dir_dirty", "");
    settings.color.git.o_dir_clean = cpp11_getstring(ini, "color:git_dir_clean", "");

    settings.color.git.o_repo_dirty = cpp11_getstring(ini, "color:git_repo_dirty", "");
    settings.color.git.o_repo_clean = cpp11_getstring(ini, "color:git_repo_clean", "");
    settings.color.git.o_repo_bare = cpp11_getstring(ini, "color:git_repo_bare", "");
    #endif

    iniparser_freedict(ini);
}

int main(int argc, const char *argv[])
{
    FileList files;
    DirList dirs;

    settings.no_conf = false;


    loadconfig();

    bool parse = true;

    while (parse) {
        // NOLINTNEXTLINE
        int c = getopt(argc, const_cast<char **>(argv), "AalrtfSLMnhNc:F:");

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
                if (settings.list) {
                    settings.format = settings.list_format;
                    settings.forced_columns = 1;
                }
                break;

            case 'n':
                settings.colors = !settings.colors;
                break;

            case 'N':
                settings.no_conf = true;
                loadconfig();
                break;

            case 'F':
                settings.format = optarg;
                break;

            case 'h':
                // NOLINTNEXTLINE
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

        uint32_t count = argc - optind;

        #pragma omp parallel for shared(count)
        for (uint32_t i = 0; i < count; i++) { // NOLINT
            struct stat st = {0};

            if ((lstat(sp.at(i), &st)) < 0) {
                // NOLINTNEXTLINE
                fprintf(stderr, "Unable to open %s!\n", sp.at(i));
            } else {
                #ifdef S_ISLNK
                char target[PATH_MAX] = {};
                std::string lpath;

                char fullpath[PATH_MAX] = {0};

                // NOLINTNEXTLINE
                snprintf(fullpath, PATH_MAX, "%s", sp.at(i));

                if ((readlink(sp.at(i), &target[0], sizeof(target))) >= 0) {
                    lpath = &target[0];
                    if (lpath.at(0) != '/') {
                        // NOLINTNEXTLINE
                        lpath = std::string(dirname(&fullpath[0])) + "/" + lpath;
                    }

                    lstat(lpath.c_str(), &st);
                }
                #endif

                if (S_ISDIR(st.st_mode)) { // NOLINT
                    dirs.insert(DirList::value_type(
                        sp.at(i),
                        listdir(sp.at(i), "/*", false) // NOLINT
                    ));
                } else {
                    char file[PATH_MAX] = {0};
                    strncpy(&file[0], sp.at(i), PATH_MAX);

                    files.push_back(addfile("", &file[0]));
                }
            }
        }
    } else {
        dirs.insert(DirList::value_type(
            "./",
            listdir(".", "/*", false) // NOLINT
        ));
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

            // NOLINTNEXTLINE
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
