// Copyright 2025 <c0r73x@gmail.com>

extern "C" {
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#ifdef USE_GIT
    #include <git2.h>
#endif
}

#ifdef USE_OPENMP
    #include <omp.h>
    #ifdef __GLIBCXX__
        #include <parallel/algorithm>
    #endif
#endif

#include <algorithm>
#include <climits>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "entry.hpp"

// Entries are intentionally never freed, the process exits right after
// printing and freeing thousands of small objects would only cost time.
using FileList = std::vector<Entry *>;
using DirList = std::vector<std::pair<std::string, FileList>>;

// Below this many entries per thread the OpenMP wake-up costs more than
// the work being split.
#define PARALLEL_MIN 32
// Beyond this, extra threads mostly spin at barriers and fight over the
// allocator (measured on a 16 core Ryzen; 4-8 was best in every case).
#define MAX_THREADS 8
// Index entries below the listed directory needed per extra git status
// pass, each pass opens its own repository and loads the index again.
#define GIT_ENTRIES_PER_PASS 4000

static int teamSize(size_t work)
{
    #ifdef USE_OPENMP
    int threads = std::min(omp_get_max_threads(), MAX_THREADS);
    return std::max(1, std::min(threads, static_cast<int>(work / PARALLEL_MIN)));
    #else
    (void)work;
    return 1;
    #endif
}

settings_t settings = {0};

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

#ifdef USE_GIT
struct RepoContext {
    git_repository *repo = nullptr;
    std::string workdir;
    // repo relative path of the listed directory, "" or "sub/dir/"
    std::string prefix;
    // direct child name -> index into the entry list
    std::unordered_map<std::string, size_t> index;
    // per entry: git_status_t | GIT_ISTRACKED for the entry itself, plus
    // GIT_ISTRACKED | GIT_DIR_DIRTY folded in from everything below it
    std::vector<unsigned int> flags;

    // submodules (gitlink index entries) below the listed directory
    struct Gitlink {
        std::string path; // repo relative
        git_oid id;       // commit recorded in the superproject
        size_t index;     // entry the result is folded into
        bool direct;      // the entry itself is the submodule
    };

    std::vector<Gitlink> gitlinks;
};

// Open the repository containing path (not the cwd) and work out where in
// the work tree path is.
static bool openRepoContext(const char *path, RepoContext *ctx)
{
    if (git_repository_open_ext(
                &ctx->repo,
                path,
                GIT_REPOSITORY_OPEN_FROM_ENV,
                nullptr
            ) != 0) {
        ctx->repo = nullptr;
        return false;
    }

    const char *wd = git_repository_workdir(ctx->repo);
    char dirpath[PATH_MAX] = {0};
    char rppath[PATH_MAX] = {0};
    char gitpath[PATH_MAX] = {0};

    if (
        wd == nullptr ||
        realpath(path, &dirpath[0]) == nullptr ||
        realpath(wd, &rppath[0]) == nullptr ||
        !path_prefix(&rppath[0], &dirpath[0]) ||
        (
            realpath(git_repository_path(ctx->repo), &gitpath[0]) != nullptr &&
            path_prefix(&gitpath[0], &dirpath[0])
        )
    ) {
        git_repository_free(ctx->repo);
        ctx->repo = nullptr;
        return false;
    }

    ctx->workdir = wd;

    const char *rel = &dirpath[strlen(&rppath[0])];

    while (*rel == '/') {
        rel++;
    }

    ctx->prefix = rel;

    if (!ctx->prefix.empty()) {
        ctx->prefix += '/';
    }

    return true;
}

// Run one status pass limited to paths (repo relative, a directory path
// includes everything below it) and fold the result into ctx->flags.
// Direct children get their own status, deeper paths are folded into
// their top level directory so no per-directory status runs are needed.
// Callers must give concurrent passes disjoint sets of children.
static void collectStatus(git_repository *repo, RepoContext *ctx,
                          const std::vector<std::string> &paths)
{
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.flags = (
                     GIT_STATUS_OPT_INCLUDE_IGNORED |
                     GIT_STATUS_OPT_INCLUDE_UNMODIFIED |
                     GIT_STATUS_OPT_EXCLUDE_SUBMODULES |
                     GIT_STATUS_OPT_DISABLE_PATHSPEC_MATCH
                 );

    std::vector<char *> specs;
    specs.reserve(paths.size());

    for (const auto &p : paths) {
        specs.push_back(const_cast<char *>(p.c_str())); // NOLINT
    }

    if (!specs.empty()) {
        opts.pathspec.strings = specs.data();
        opts.pathspec.count = specs.size();
    }

    git_status_list *list = nullptr;

    if (git_status_list_new(&list, repo, &opts) != 0) {
        return;
    }

    size_t count = git_status_list_entrycount(list);
    std::string name;

    for (size_t i = 0; i < count; i++) {
        const git_status_entry *status = git_status_byindex(list, i);
        const char *filePath = (status->head_to_index != nullptr) ?
                               status->head_to_index->new_file.path :
                               (status->index_to_workdir != nullptr) ?
                               status->index_to_workdir->new_file.path :
                               nullptr;

        if (
            filePath == nullptr ||
            strncmp(filePath, ctx->prefix.c_str(), ctx->prefix.length()) != 0
        ) {
            continue;
        }

        const char *rest = filePath + ctx->prefix.length();
        const char *slash = strchr(rest, '/');

        if (slash == nullptr) {
            name.assign(rest);
        } else {
            name.assign(rest, slash);
        }

        auto idx = ctx->index.find(name);

        if (idx == ctx->index.end()) {
            continue;
        }

        unsigned int &flags = ctx->flags[idx->second];
        unsigned int add = 0;

        if (slash == nullptr || slash[1] == '\0') {
            // a file, or a directory reported as a whole ("build/")
            add = status->status | GIT_ISTRACKED;
        } else if (status->status != GIT_STATUS_IGNORED) {
            add = GIT_ISTRACKED;

            if (status->status != GIT_STATUS_CURRENT) {
                add |= GIT_DIR_DIRTY;
            }
        }

        // submodule checks may update the same entry concurrently
        #pragma omp atomic
        flags |= add;
    }

    git_status_list_free(list);
}

// Clean/dirty/bare state of the repository rooted at path. For a
// submodule, expected is the commit the superproject records; a different
// HEAD counts as dirty just like changes in its work tree.
static unsigned int repoflags(const std::string &path,
                              const git_oid *expected = nullptr)
{
    git_repository *repo = nullptr;

    if (git_repository_open_ext(
                &repo,
                path.c_str(),
                GIT_REPOSITORY_OPEN_NO_SEARCH,
                nullptr
            ) != 0) {
        return NO_FLAGS;
    }

    if (git_repository_workdir(repo) == nullptr) {
        git_repository_free(repo);
        return GIT_ISREPO | GIT_DIR_BARE;
    }

    unsigned int flags = GIT_ISREPO;

    if (expected != nullptr) {
        git_oid head;

        if (git_reference_name_to_id(&head, repo, "HEAD") == 0 &&
                git_oid_cmp(&head, expected) != 0) {
            // no need to walk the work tree, it is dirty either way
            git_repository_free(repo);
            return flags | GIT_DIR_DIRTY;
        }
    }

    // submodules included, a modified submodule makes the repo dirty
    // just like in git status
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_WORKDIR_ONLY;
    opts.flags = 0;

    git_status_list *statuses = nullptr;

    if (git_status_list_new(&statuses, repo, &opts) == 0) {
        // without INCLUDE_UNMODIFIED only changed entries are listed
        if (git_status_list_entrycount(statuses) > 0) {
            flags |= GIT_DIR_DIRTY;
        }

        git_status_list_free(statuses);
    }

    git_repository_free(repo);
    return flags;
}

static bool hasDotGit(int dfd, const std::string &name)
{
    std::string dotgit = name + "/.git";
    return faccessat(dfd, dotgit.c_str(), F_OK, 0) == 0;
}
#endif

static Entry *makeEntry(int dfd, const std::string &dirprefix,
                        const char *name, const struct stat *parent)
{
    struct stat st = {0};
    std::string fullpath = dirprefix + name;

    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        fprintf(stderr, "Unable to get stats for %s\n", fullpath.c_str());
        return nullptr;
    }

    #ifdef S_ISLNK

    if (S_ISLNK(st.st_mode) && settings.resolve_links) {
        char target[PATH_MAX];
        ssize_t len = readlinkat(dfd, name, &target[0], sizeof(target) - 1);

        if (len > 0) {
            target[len] = '\0';
            std::string lpath;

            if (target[0] != '/') {
                std::string copy = fullpath;
                lpath = std::string(dirname(&copy[0])) + "/" + &target[0];
            } else {
                lpath = &target[0];
            }

            if ((lstat(lpath.c_str(), &st)) < 0) {
                fprintf(
                    stderr,
                    "cannot access '%s': No such file or directory\n",
                    name
                );

                return new Entry(name, fullpath.c_str(), nullptr, nullptr);
            }

            fullpath = lpath;
            parent = nullptr;
        }
    }

    #endif /* S_ISLNK */

    return new Entry(name, fullpath.c_str(), &st, parent);
}

FileList listdir(const char *path)
{
    FileList lst;

    int dfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (dfd < 0) {
        fprintf(stderr, "Unable to open %s!\n", path);
        return lst;
    }

    struct stat pst = {0};
    const struct stat *parent = (fstat(dfd, &pst) == 0) ? &pst : nullptr;

    DIR *dir = fdopendir(dfd);

    if (dir == nullptr) {
        close(dfd);
        return lst;
    }

    // Read all names up front on one thread, readdir() on a shared DIR is
    // not something to split between threads.
    std::vector<std::string> names;
    names.reserve(64);

    for (dirent *ent = readdir(dir); ent != nullptr; ent = readdir(dir)) {
        const char *n = &ent->d_name[0];

        if (n[0] == '.') {
            if (!settings.show_hidden ||
                    n[1] == '\0' || (n[1] == '.' && n[2] == '\0')) {
                continue;
            }
        }

        names.emplace_back(n);
    }

    std::string dirprefix = path;

    if (!dirprefix.empty() && dirprefix.back() != '/') {
        dirprefix += '/';
    }

    const size_t count = names.size();
    const int threads = teamSize(count);
    lst.assign(count, nullptr);

    #ifdef USE_GIT
    RepoContext ctx;
    const bool inrepo = settings.resolve_in_repos &&
                        openRepoContext(path, &ctx);

    // Status work items. Each directory is its own item, the plain files
    // are chunked. With a single item the pass is simply limited to the
    // listed directory.
    std::vector<std::vector<std::string>> items;
    std::vector<git_repository *> repos;

    if (inrepo) {
        ctx.flags.assign(count, 0);
        ctx.index.reserve(count);

        for (size_t i = 0; i < count; i++) {
            ctx.index.emplace(names[i], i);
        }

        // Every extra status pass needs its own repository and index load,
        // so only split when the listed subtree is big enough to pay for
        // that. Children are weighted by their number of index entries and
        // packed into one item per pass.
        std::vector<size_t> weight(count, 1);
        size_t subtree = 0;
        git_index *index = nullptr;

        if (git_repository_index(&index, ctx.repo) == 0) {
            size_t pos = 0;
            size_t total = git_index_entrycount(index);

            if (!ctx.prefix.empty() &&
                    git_index_find_prefix(&pos, index, ctx.prefix.c_str()) != 0) {
                pos = total;
            }

            std::string name;

            for (; pos < total; pos++) {
                const git_index_entry *entry = git_index_get_byindex(index, pos);

                if (strncmp(entry->path, ctx.prefix.c_str(),
                            ctx.prefix.length()) != 0) {
                    break;
                }

                const char *rest = entry->path + ctx.prefix.length();
                const char *slash = strchr(rest, '/');
                name.assign(rest, slash != nullptr ? slash : rest + strlen(rest));

                auto idx = ctx.index.find(name);

                if (idx != ctx.index.end()) {
                    weight[idx->second]++;

                    if (entry->mode == GIT_FILEMODE_COMMIT) {
                        ctx.gitlinks.push_back({
                            entry->path,
                            entry->id,
                            idx->second,
                            slash == nullptr
                        });
                    }
                }

                subtree++;
            }

            git_index_free(index);
        }

        const size_t passes = std::min<size_t>(
                                  threads, subtree / GIT_ENTRIES_PER_PASS
                              );

        if (passes > 1) {
            std::vector<size_t> order;
            order.reserve(count);

            for (size_t i = 0; i < count; i++) {
                if (names[i] != ".git") {
                    order.push_back(i);
                }
            }

            std::sort(order.begin(), order.end(), [&weight](size_t a, size_t b) {
                return weight[a] > weight[b];
            });

            // longest processing time first: heaviest child to lightest bin
            items.resize(passes);
            std::vector<size_t> load(passes, 0);

            for (size_t i : order) {
                size_t bin = std::min_element(load.begin(), load.end()) -
                             load.begin();
                items[bin].push_back(ctx.prefix + names[i]);
                load[bin] += weight[i];
            }
        } else {
            std::string spec = ctx.prefix;

            if (!spec.empty()) {
                spec.pop_back();
                items.push_back({ spec });
            } else {
                items.emplace_back();
            }
        }

        repos.assign(std::max<size_t>(threads, 1), nullptr);
        repos[0] = ctx.repo;
    }

    const size_t nitems = items.size();
    #endif

    #pragma omp parallel num_threads(threads)
    {
        #ifdef USE_GIT
        // git_repository is not thread safe, every thread opens its own.
        // Threads move on to stat entries once the items run out, the
        // barrier after that loop waits for both.
        #pragma omp for schedule(dynamic, 1) nowait
        for (size_t i = 0; i < nitems; i++) {
            #ifdef USE_OPENMP
            const int t = omp_get_thread_num();
            #else
            const int t = 0;
            #endif

            if (repos[t] == nullptr &&
                    git_repository_open_ext(
                        &repos[t],
                        ctx.workdir.c_str(),
                        GIT_REPOSITORY_OPEN_NO_SEARCH,
                        nullptr
                    ) != 0) {
                repos[t] = nullptr;
                continue;
            }

            collectStatus(repos[t], &ctx, items[i]);
        }

        // Submodules are excluded from the status passes, check each one
        // on its own and fold the result into its entry (or the directory
        // containing it).
        #pragma omp for schedule(dynamic, 1) nowait
        for (size_t i = 0; i < ctx.gitlinks.size(); i++) {
            const auto &link = ctx.gitlinks[i];
            unsigned int add = GIT_ISTRACKED;

            if (settings.resolve_repos) {
                unsigned int rf = repoflags(ctx.workdir + link.path, &link.id);

                if (rf != NO_FLAGS) {
                    add |= link.direct ? rf : (rf & GIT_DIR_DIRTY);
                }
            }

            if (link.direct) {
                add |= GIT_ISSUBMODULE;
            }

            #pragma omp atomic
            ctx.flags[link.index] |= add;
        }
        #endif

        #pragma omp for schedule(dynamic, 16)
        for (size_t i = 0; i < count; i++) {
            lst[i] = makeEntry(dfd, dirprefix, names[i].c_str(), parent);
        }

        #pragma omp for schedule(dynamic, 4)
        for (size_t i = 0; i < count; i++) {
            Entry *e = lst[i];

            if (e == nullptr) {
                continue;
            }

            #ifdef USE_GIT
            if (e->mode != 0) {
                const std::string &name = names[i];
                unsigned int flags = NO_FLAGS;

                if (inrepo) {
                    flags = ctx.flags[i];

                    // submodules were handled with the status passes
                    if (e->isdir && settings.resolve_repos &&
                            (flags & GIT_ISSUBMODULE) == 0 &&
                            name != ".git" && hasDotGit(dfd, name)) {
                        unsigned int rf = repoflags(dirprefix + name);

                        if (rf != NO_FLAGS) {
                            flags |= rf;
                        }
                    }
                } else if (e->isdir && settings.resolve_repos &&
                           hasDotGit(dfd, name)) {
                    flags = repoflags(dirprefix + name);
                }

                e->setGit(flags);
            }
            #endif

            e->postprocess();
        }
    }

    #ifdef USE_GIT
    for (auto *repo : repos) {
        if (repo != nullptr) {
            git_repository_free(repo);
        }
    }
    #endif

    closedir(dir);

    lst.erase(std::remove(lst.begin(), lst.end(), nullptr), lst.end());
    return lst;
}

void printdir(FileList *lst, std::string *out)
{
    lst->erase(std::remove(lst->begin(), lst->end(), nullptr), lst->end());

    const bool dirs_first = settings.dirs_first;
    const bool reversed = settings.reversed;
    const unsigned char sort = settings.sort;

    auto compare = [dirs_first, reversed, sort](const Entry *a, const Entry *b) {
        if (dirs_first && a->isdir != b->isdir) {
            return a->isdir;
        }

        int64_t cmp = 0;

        if ((sort & SORT_TYPE) == SORT_TYPE) {
            cmp = b->extension.compare(a->extension);
        }

        if (cmp == 0) {
            if ((sort & SORT_ALPHA) == SORT_ALPHA) {
                cmp = b->file.compare(a->file);
            } else if ((sort & SORT_MODIFIED) == SORT_MODIFIED) {
                cmp = a->modified - b->modified;
            } else if ((sort & SORT_SIZE) == SORT_SIZE) {
                cmp = a->bsize - b->bsize;
            }
        }

        if (reversed) {
            return cmp < 0;
        }

        return cmp > 0;
    };

    #if defined(USE_OPENMP) && defined(__GLIBCXX__)
    // multiway mergesort on all threads, sequential for small inputs
    __gnu_parallel::sort(lst->begin(), lst->end(), compare);
    #else
    std::sort(lst->begin(), lst->end(), compare);
    #endif

    const size_t count = lst->size();
    const size_t slots = parsed_format.slots.length();

    int maxtotlen = 0;
    Lengths maxlens(slots, 0);

    for (const auto *l : *lst) {
        for (size_t s = 0; s < slots; s++) {
            maxlens[s] = std::max(l->processed[s].second, maxlens[s]);
        }

        maxtotlen = std::max(l->totlen, maxtotlen);
    }

    // every printed entry is padded to the widest value of each field
    int rowlen = parsed_format.literal_len;

    for (const auto &token : parsed_format.tokens) {
        if (!token.literal) {
            rowlen += maxlens[token.slot];
        }
    }

    const int width = std::max(rowlen, maxtotlen + 1);

    int columns = 1;

    if (settings.forced_columns > 0) {
        columns = settings.forced_columns;
    } else if (isatty(STDOUT_FILENO)) {
        struct winsize w = { 0 };
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        columns = std::max(static_cast<int>(w.ws_col) / width, 1);
    }

    std::vector<std::string> lines(count);

    #pragma omp parallel for schedule(static) num_threads(teamSize(count / 8))
    for (size_t i = 0; i < count; i++) {
        lines[i].reserve(rowlen * 4);
        (*lst)[i]->print(lines[i], maxlens);
    }

    auto endRow = [out](size_t start) {
        size_t end = out->find_last_not_of(" \t\n\v\f\r");
        end = (end == std::string::npos || end < start) ? start : end + 1;
        out->resize(end);
        *out += "\033[0m\n";
    };

    const std::string *ext = nullptr;
    int current = 0;
    size_t rowstart = out->length();

    for (size_t i = 0; i < count; i++) {
        const Entry *l = (*lst)[i];

        if ((sort & SORT_TYPE) == SORT_TYPE &&
                (ext == nullptr || l->extension != *ext)) {
            if (current != 0) {
                endRow(rowstart);
                current = 0;
            }

            *out += "\n\033[0m";
            *out += l->extension;
            *out += ":\n";
            ext = &l->extension;
            rowstart = out->length();
        }

        *out += lines[i];

        if (rowlen < width) {
            out->append(width - rowlen, ' ');
        }

        current++;

        if (current == columns) {
            endRow(rowstart);
            current = 0;
            rowstart = out->length();
        }
    }

    if (current != 0) {
        endRow(rowstart);
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
    {"help", no_argument, nullptr, 'h'},
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

void printHelp(const char *name)
{
    struct help_t {
        const char *opt;
        const char *desc;
    };

    static const help_t options[] = {
        {"-a, --show-hidden", "show entries starting with ."},
        {"-c, --forced-columns=N", "use N columns instead of the terminal width"},
        {"-C, --no-color", "disable colors"},
        {"-f, --dirs-first", "list directories before files"},
        {"-F, --format=FORMAT", "use FORMAT for each entry, see below"},
        {"-g, --resolve-in-repos", "show git status of entries inside a repository"},
        {"-G, --resolve-repos", "show clean/dirty state of repositories"},
        {"-l, --list", "long listing, uses list_format from the config"},
        {"-L, --resolve-links", "show the target of symlinks instead of the link"},
        {"-M, --resolve-mounts", "show the device of mount points (with -l)"},
        {"-n, --numeric-uid-gid", "show numeric user and group ids"},
        {"-r, --reversed", "reverse the sort order"},
        {"-A, --sort-name", "sort by name"},
        {"-S, --sort-size", "sort by size"},
        {"-t, --sort-date", "sort by modification time"},
        {"-X, --sort-type", "group and sort by extension"},
        {"-h, --help", "show this help and exit"},
    };

    static const help_t formats[] = {
        {"@p", "permissions (drwxr-xr-x)"},
        {"@P", "permissions in octal (755)"},
        {"@u", "user"},
        {"@g", "group"},
        {"@U", "user:group"},
        {"@r", "relative modification time, number"},
        {"@t", "relative modification time, unit"},
        {"@D", "modification date (YYYY-MM-DD)"},
        {"@T", "modification time (HH:MM)"},
        {"@s", "size"},
        {"@G", "git status"},
        {"@F", "file name"},
        {"@f", "file name with link/mount target"},
        {"@^x", "right align field x"},
        {"@@", "a literal @"},
    };

    const char *base = strrchr(name, '/');
    printf("Usage: %s [OPTION]... [FILE]...\n", base != nullptr ? base + 1 : name);
    printf("List FILEs (the current directory by default).\n\n");
    printf("Options:\n");

    for (const auto &o : options) {
        printf("  %-26s %s\n", o.opt, o.desc);
    }

    printf("\nOn/off options toggle the value set in lsext.ini.\n\n");
    printf("Format:\n");

    for (const auto &f : formats) {
        printf("  %-26s %s\n", f.opt, f.desc);
    }

    printf("\n  default format:      \"%s\"\n", settings.format.c_str());
    printf("  default list format: \"%s\"\n", settings.list_format.c_str());
}

int main(int argc, const char *argv[])
{
    settings.no_conf = false;

    loadconfig();

    bool parse = true;

    while (parse) {
        int c = getopt_long(argc, const_cast<char **>(argv), "c:LMGgarfXtSAlnF:Ch",
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

            case 'h':
                printHelp(argv[0]);
                return EXIT_SUCCESS;

            default:
                parse = false;
                break;
        }
    }

    #ifdef USE_OPENMP
    omp_set_num_threads(std::min(omp_get_max_threads(), MAX_THREADS));
    #endif

    now = time(nullptr);
    parseFormat(settings.format);

    if (settings.colors) {
        initColors();
    }

    initTables();

    #ifdef USE_GIT
    const bool use_git = settings.resolve_repos || settings.resolve_in_repos;

    if (use_git) {
        git_libgit2_init();
        git_libgit2_opts(GIT_OPT_ENABLE_STRICT_HASH_VERIFICATION, 0);
        #ifdef LSEXT_GIT_FORK
        git_libgit2_opts(GIT_OPT_DISABLE_INDEX_CHECKSUM_VERIFICATION, 1);
        git_libgit2_opts(GIT_OPT_DISABLE_INDEX_FILEPATH_VALIDATION, 1);
        git_libgit2_opts(GIT_OPT_DISABLE_READNG_PACKED_TAGS, 1);
        #endif
    }
    #endif

    std::vector<const char *> args;

    if (argc - optind > 0) {
        args.assign(argv + optind, argv + argc);
        std::stable_sort(args.begin(), args.end(), [](const char *a, const char *b) {
            return strlen(a) < strlen(b);
        });
    } else {
        args.push_back(".");
    }

    std::vector<const char *> fileargs;
    DirList dirs;

    for (const char *curr : args) {
        struct stat st = {0};

        // follow a symlinked argument so links to directories get listed
        if (stat(curr, &st) < 0 && lstat(curr, &st) < 0) {
            fprintf(stderr, "Unable to open %s!\n", curr);
        } else if (S_ISDIR(st.st_mode)) {
            dirs.emplace_back(curr, FileList());
        } else {
            fileargs.push_back(curr);
        }
    }

    FileList files(fileargs.size(), nullptr);

    #pragma omp parallel for schedule(dynamic) num_threads(teamSize(fileargs.size()))
    for (size_t i = 0; i < fileargs.size(); i++) {
        files[i] = makeEntry(AT_FDCWD, "", fileargs[i], nullptr);

        if (files[i] != nullptr) {
            files[i]->postprocess();
        }
    }

    for (auto &dir : dirs) {
        dir.second = listdir(dir.first.c_str());
    }

    std::string output;

    if (!files.empty()) {
        printdir(&files, &output);
    }

    for (auto &dir : dirs) {
        if (dirs.size() > 1 || !files.empty()) {
            std::string path = dir.first;

            while (path.length() > 1 && path.back() == '/') {
                path.pop_back();
            }

            output += "\n\033[0m";
            output += path;
            output += ":\n";
        }

        printdir(&dir.second, &output);
    }

    fwrite(output.data(), 1, output.size(), stdout);

    #ifdef USE_GIT
    if (use_git) {
        git_libgit2_shutdown();
    }
    #endif

    return EXIT_SUCCESS;
}
