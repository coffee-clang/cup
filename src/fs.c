#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include <string.h>
#include <dirent.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "fs.h"

static CupError create_directory(const char *path) {
    struct stat info;

    if (stat(path, &info) == -1) {
        if (mkdir(path, 0700) != 0) {
            fprintf(stderr, "Error: could not create directory '%s'.\n", path);
            return CUP_ERR_FS;
        }
    }

    return CUP_OK;
}

static CupError remove_directory_recursive(const char *path) {
    DIR *dir;
    struct dirent *entry;
    char full_path[MAX_PATH_LEN];

    dir = opendir(path);
    if (dir == NULL) {
        return CUP_ERR_FS;
    }

    while ((entry = readdir(dir)) != NULL) {
        DIR *subdir;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (checked_snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name) != CUP_OK) {
            closedir(dir);
            return CUP_ERR_FS;
        }

        subdir = opendir(full_path);
        if (subdir != NULL) {
            closedir(subdir);

            if (remove_directory_recursive(full_path) != CUP_OK) {
                closedir(dir);
                return CUP_ERR_FS;
            }
        } else {
            if (remove(full_path) != 0) {
                closedir(dir);
                return CUP_ERR_FS;
            }
        }
    }

    closedir(dir);

    if (rmdir(path) != 0) {
        return CUP_ERR_FS;
    }

    return CUP_OK;
}

CupError checked_snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    int written;

    if (buffer == NULL || format == NULL || size == 0) {
        fprintf(stderr, "Error: invalid snprintf arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    va_start(args, format);
    written = vsnprintf(buffer, size, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= size) {
        fprintf(stderr, "Error: formatted path is too long.\n");
        return CUP_ERR_FS;
    }

    return CUP_OK;
}

// Temporary
const char *get_platform_name(void) {
    return "linux";
}

CupError get_cup_root_path(char *buffer, size_t size) {
    const char *home;

    home = getenv("HOME");
    if (home == NULL) {
        fprintf(stderr, "Error: HOME environment variable is not set.\n");
        return CUP_ERR_FS;
    }

    return checked_snprintf(buffer, size, "%s/.cup", home);
}

CupError get_state_file_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/state.txt", root);
}

CupError get_components_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/components", root);
}

CupError get_tmp_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/tmp", root);
}

CupError get_cache_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/cache", root);
}

CupError get_package_manifest_path(char *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    return checked_snprintf(buffer, size, "config/packages.cfg");
}

CupError ensure_cup_structure(void) {
    CupError err;
    char root[MAX_PATH_LEN];
    char components[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];
    char cache[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_components_root_path(components, sizeof(components));
    if (err != CUP_OK) {
        return err;
    }

    err = get_tmp_root_path(tmp, sizeof(tmp));
    if (err != CUP_OK) {
        return err;
    }

    err = get_cache_root_path(cache, sizeof(cache));
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(root);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(components);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(tmp);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(cache);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError ensure_component_base_dirs(const char *component, const char *tool, const char *platform) {
    CupError err;
    char components_root[MAX_PATH_LEN];
    char component_dir[MAX_PATH_LEN];
    char tool_dir[MAX_PATH_LEN];
    char platform_dir[MAX_PATH_LEN];

    if (component == NULL || tool == NULL || platform == NULL) {
        fprintf(stderr, "Error: invalid component directory arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    err = get_components_root_path(components_root, sizeof(components_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(component_dir, sizeof(component_dir), "%s/%s", components_root, component);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(tool_dir, sizeof(tool_dir), "%s/%s", component_dir, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(platform_dir, sizeof(platform_dir), "%s/%s", tool_dir, platform);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(component_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(tool_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(platform_dir);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError ensure_cache_package_dirs(const char *component, const char *tool, const char *release) {
    CupError err;
    char cache_root[MAX_PATH_LEN];
    char component_dir[MAX_PATH_LEN];
    char tool_dir[MAX_PATH_LEN];
    char release_dir[MAX_PATH_LEN];

    if (component == NULL || tool == NULL || release == NULL) {
        fprintf(stderr, "Error: invalid component directory arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    err = get_cache_root_path(cache_root, sizeof(cache_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(component_dir, sizeof(component_dir), "%s/%s", cache_root, component);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(tool_dir, sizeof(tool_dir), "%s/%s", component_dir, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(release_dir, sizeof(release_dir), "%s/%s", tool_dir, release);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(component_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(tool_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(release_dir);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    char components_root[MAX_PATH_LEN];

    err = get_components_root_path(components_root, sizeof(components_root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/%s/%s/%s/%s", components_root, component, tool, get_platform_name(), release);
}

CupError build_tmp_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, int suffix) {
    CupError err;
    char tmp_root[MAX_PATH_LEN];

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/%s-%s-%s-%d", tmp_root, component, tool, release, suffix);
}

CupError build_cache_package_path(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    char cache_root[MAX_PATH_LEN];

    err = get_cache_root_path(cache_root, sizeof(cache_root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/%s/%s/%s", cache_root, component, tool, release);
}

CupError build_cache_archive_path(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    char cache_package_path[MAX_PATH_LEN];

    err = build_cache_package_path(cache_package_path, sizeof(cache_package_path), component, tool, release);
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/package.tar.xz", cache_package_path);
}

CupError build_package_url_from_manifest(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    char template_url[MAX_PATH_LEN];

    if (buffer == NULL || component == NULL || tool == NULL || release == NULL) {
        fprintf(stderr, "Error: invalid manifest lookup arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = read_manifest_value(template_url, sizeof(template_url), component, tool, "url");
    if (err != CUP_OK) {
        return err;
    }

    return replace_version_placeholder(buffer, size, template_url, release);
}

CupError read_manifest_value(char *buffer, size_t size, const char *component, const char *tool, const char *key_suffix) {
    CupError err;
    FILE *file;
    char manifest_path[MAX_PATH_LEN];
    char line[1024];
    char key[MAX_NAME_LEN *3];
    size_t key_len;

    if (buffer == NULL || component == NULL || tool == NULL || key_suffix == NULL) {
        fprintf(stderr, "Error: invalid manifest lookup arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_package_manifest_path(manifest_path, sizeof(manifest_path));
    if (err != CUP_OK) {
        return err;
    }

    file = fopen(manifest_path, "r");
    if (file == NULL) {
        fprintf(stderr, "Error: could not open package manifest '%s'.\n", manifest_path);
        return CUP_ERR_FETCH;
    }

    err = checked_snprintf(key, sizeof(key), "%s.%s.%s=", component, tool, key_suffix);
    if (err != CUP_OK) {
        fclose(file);
        return CUP_ERR_FETCH;
    }

    key_len = strlen(key);

    while (fgets(line, sizeof(line), file) != NULL) {
        size_t len;

        if (strncmp(line, key, key_len) != 0) {
            continue;
        }

        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }

        fclose(file);
        return checked_snprintf(buffer, size, "%s", line + key_len);
    }

    fclose(file);
    fprintf(stderr, "Error: no package source configured for %s.%s.%s.\n", component, tool, key_suffix);
    return CUP_ERR_FETCH;
}

CupError replace_version_placeholder(char *buffer, size_t size, const char *template_url, const char *version) {
    const char *placeholder = "{version}";
    const char *cursor;
    size_t placeholder_len;
    size_t version_len;
    size_t out_len;
    char *out;

    if (buffer == NULL || template_url == NULL || version == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    placeholder_len = strlen(placeholder);
    version_len = strlen(version);
    out_len = 0;
    cursor = template_url;
    out = buffer;

    while (*cursor != '\0') {
        if (strncmp(cursor, placeholder, placeholder_len) == 0) {
            if (out_len + version_len + 1 > size) {
                fprintf(stderr, "Error: resolved package URL is too long.\n");
                return CUP_ERR_FETCH;
            }

            memcpy(out, version, version_len);
            out += version_len;
            out_len += version_len;
            cursor += placeholder_len;
        } else {
            if (out_len + 2 > size) {
                fprintf(stderr, "Error: resolved package URL is too long.\n");
                return CUP_ERR_FETCH;
            }

            *out = *cursor;
            out++;
            out_len++;
            cursor++;
        }
    }

    *out = '\0';
    return CUP_OK;
}

CupError create_tmp_install_dir(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    int suffix;

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    suffix = (int)getpid();

    err = build_tmp_install_path(buffer, size, component, tool, release, suffix);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(buffer);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not create temporary install directory '%s'.\n", buffer);
        return err;
    }

    return CUP_OK;
}

CupError resolve_release(char *buffer, size_t size, const char *tool, const char *release) {
    if (buffer == NULL || tool == NULL || release == NULL || size == 0) {
        fprintf(stderr, "Error: invalid release resolution arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (strcmp(release, "stable") == 0) {
        return read_manifest_value(buffer, size, "compiler", tool, "stable_version");
    }

    if (strcmp(release, "nightly") == 0) {
        return read_manifest_value(buffer, size, "compiler", tool, "nightly_version");
    }

    return checked_snprintf(buffer, size, "%s", release);
}

CupError download_package(const char *url, const char *dst_path) {
    char command[MAX_PATH_LEN * 2];
    int status;

    if (url == NULL || dst_path == NULL) {
        fprintf(stderr, "Error: invalid download arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (checked_snprintf(command, sizeof(command), "curl -L \"%s\" -o \"%s\" >/dev/null 2>&1", url, dst_path) != CUP_OK) {
        return CUP_ERR_FETCH;
    }

    status = system(command);
    if (status != 0) {
        fprintf(stderr, "Error: failed to download package from '%s'.\n", url);
        return CUP_ERR_FETCH;
    }

    return CUP_OK;
}

CupError extract_archive_to_tmp(const char *archive_path, const char *tmp_path) {
    char command[MAX_PATH_LEN * 2];
    int status;

    if (archive_path == NULL || tmp_path == NULL) {
        fprintf(stderr, "Error: invalid archive extraction arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (checked_snprintf(command, sizeof(command), "tar -xJf \"%s\" -C \"%s\" >/dev/null 2>&1", archive_path, tmp_path) != CUP_OK) {
        return CUP_ERR_INSTALL;
    }

    status = system(command);
    if (status != 0) {
        fprintf(stderr, "Error: failed to extract archive '%s'.\n", archive_path);
        return CUP_ERR_INSTALL;
    }

    return CUP_OK;
}

CupError fetch_package(char *buffer, size_t size, const char *component, const char *tool, const char *resolved_release) {
    CupError err;
    char archive_path[MAX_PATH_LEN];
    char package_url[MAX_PATH_LEN];
    int exists;

    if (buffer == NULL || component == NULL || tool == NULL || resolved_release == NULL) {
        fprintf(stderr, "Error: invalid package fetch arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cache_package_dirs(component, tool, resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = build_cache_archive_path(archive_path, sizeof(archive_path), component, tool, resolved_release);
    if (err != CUP_OK) {
        return CUP_ERR_FETCH;
    }

    err = archive_exists(archive_path, &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (!exists) {
        err = build_package_url_from_manifest(package_url, sizeof(package_url), component, tool, resolved_release);
        if (err != CUP_OK) {
            return CUP_ERR_FETCH;
        }

        err = download_package(package_url, archive_path);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = checked_snprintf(buffer, size, "%s", archive_path);
    if (err != CUP_OK) {
        return CUP_ERR_FETCH;
    }

    return CUP_OK;
}

CupError install_package(const char *package_path, const char *tmp_path, const char *component, const char *tool, const char *resolved_release) {
    CupError err;

    if (package_path == NULL || tmp_path == NULL || component == NULL || tool == NULL || resolved_release == NULL) {
        fprintf(stderr, "Error: invalid package install arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = extract_archive_to_tmp(package_path, tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = write_component_info_at_path(tmp_path, component, tool, resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError perform_install(const char *tmp_path, const char *component, const char *tool, const char *release) {
    CupError err;
    char package_path[MAX_PATH_LEN];

    err = fetch_package(package_path, sizeof(package_path), component, tool, release);
    if (err != CUP_OK) {
        return err;
    }

    err = install_package(package_path, tmp_path, component, tool, release);
    if (err != CUP_OK) {
        return CUP_ERR_INSTALL;
    }

    return CUP_OK;
}

CupError validate_install(const char *tmp_path) {
    CupError err;
    FILE *file;
    struct stat info;
    char info_path[MAX_PATH_LEN];
    int ch;

    if (tmp_path == NULL) {
        fprintf(stderr, "Error: invalid install path.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (stat(tmp_path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "Error: installation validation failed because the temporary directory is missing.\n");
        return CUP_ERR_VALIDATION;
    }

    err = checked_snprintf(info_path, sizeof(info_path), "%s/info.txt", tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    if (stat(info_path, &info) != 0) {
        fprintf(stderr, "Error: installation validation failed because info.txt is missing.\n");
        return CUP_ERR_VALIDATION;
    }

    file = fopen(info_path, "r");
    if (file == NULL) {
        fprintf(stderr, "Error: installation validation failed because info.txt could not be opened.\n");
        return CUP_ERR_VALIDATION;
    }

    ch = fgetc(file);
    fclose(file);

    if (ch == EOF) {
        fprintf(stderr, "Error: installation validation failed because info.txt is empty.\n");
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

CupError commit_install(const char *tmp_path, const char *final_path) {
    if (tmp_path == NULL || final_path == NULL) {
        fprintf(stderr, "Error: invalid commit arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (rename(tmp_path, final_path) != 0) {
        fprintf(stderr, "Error: could not move temporary installation to final destination.\n");
        return CUP_ERR_COMMIT;
    }

    return CUP_OK;
}

CupError cleanup_tmp_install(const char *tmp_path) {
    struct stat info;

    if (tmp_path == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (stat(tmp_path, &info) != 0) {
        return CUP_OK;
    }

    if (!S_ISDIR(info.st_mode)) {
        return CUP_ERR_FS;
    }

    return remove_directory_recursive(tmp_path);
}

CupError cleanup_all_tmp(void) {
    CupError err;
    DIR *dir;
    struct dirent *entry;
    struct stat info;
    char tmp_root[MAX_PATH_LEN];
    char full_path[MAX_PATH_LEN];

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    dir = opendir(tmp_root);
    if (dir == NULL) {
        return CUP_OK;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        err = checked_snprintf(full_path, sizeof(full_path), "%s/%s", tmp_root, entry->d_name);
        if (err != CUP_OK) {
            continue;
        }

        if (stat(full_path, &info) != 0) {
            continue;
        }

        if (S_ISDIR(info.st_mode)) {
            cleanup_tmp_install(full_path);
        } else {
            remove(full_path);
        }
    }

    closedir(dir);
    return CUP_OK;
}

CupError write_component_info_at_path(const char *base_path, const char *component, const char *tool, const char *release) {
    CupError err;
    FILE *file;
    char info_path[MAX_PATH_LEN];

    err = checked_snprintf(info_path, sizeof(info_path), "%s/info.txt", base_path);
    if (err != CUP_OK) {
        return err;
    }

    file = fopen(info_path, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: could not write info file '%s'.\n", info_path);
        return CUP_ERR_FS;
    }

    fprintf(file, "component=%s\n", component);
    fprintf(file, "tool=%s\n", tool);
    fprintf(file, "release=%s\n", release);
    fprintf(file, "platform=%s\n", get_platform_name());

    fclose(file);
    return CUP_OK;
}

CupError installation_exists(const char *component, const char *tool, const char *release, int *exists) {
    CupError err;
    struct stat info;
    char path[MAX_PATH_LEN];

    if (exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_install_path(path, sizeof(path), component, tool, release);
    if (err != CUP_OK) {
        return err;
    }

    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
        *exists = 1;
    } else {
        *exists = 0;
    }

    return CUP_OK;
}

CupError archive_exists(const char *archive_path, int *exists) {
    struct stat info;

    if (archive_path == NULL || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (stat(archive_path, &info) == 0 && S_ISREG(info.st_mode)) {
        *exists = 1;
    } else {
        *exists = 0;
    }

    return CUP_OK;
}

CupError remove_component_install_dir(const char *component, const char *tool, const char *release) {
    CupError err;
    struct stat info;
    char path[MAX_PATH_LEN];

    err = build_install_path(path, sizeof(path), component, tool, release);
    if (err != CUP_OK) {
        return err;
    }

    if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "Error: installation directory does not exist.\n");
        return CUP_ERR_NOT_INSTALLED;
    }

    err = remove_directory_recursive(path);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not remove installation directory '%s'.\n", path);
        return err;
    }

    return CUP_OK;
}