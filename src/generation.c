#include "generation.h"

#include "layout.h"
#include "checksum.h"
#include "system.h"
#include "version.h"
#include "path.h"
#include "platform.h"
#include "text.h"

#include <string.h>

CupError generation_binary_release_name(char *name, size_t size) {
    char host[MAX_PLATFORM_LEN];
    CupError err = platform_get_host(host, sizeof(host));
    if (err != CUP_OK) return err;
#if defined(_WIN32)
    return text_format(name, size, "cup-%s.exe", host);
#else
    return text_format(name, size, "cup-%s", host);
#endif
}

CupError generation_asset_release_name(GenerationAssetId id, char *name, size_t size) {
    if (name == NULL || size == 0 || id < CUP_GENERATION_ASSET_RELEASE ||
        id >= CUP_GENERATION_ASSET_COUNT) {
        return CUP_ERR_INVALID_INPUT;
    }
    switch (id) {
        case CUP_GENERATION_ASSET_RELEASE:
            return text_copy(name, size, CUP_RELEASE_METADATA_FILENAME);
        case CUP_GENERATION_ASSET_LICENSE:
            return text_copy(name, size, "LICENSE");
        case CUP_GENERATION_ASSET_NOTICES:
            return text_copy(name, size, "THIRD_PARTY_NOTICES.txt");
        case CUP_GENERATION_ASSET_BINARY:
            return generation_binary_release_name(name, size);
        default:
            return CUP_ERR_INVALID_INPUT;
    }
}

static CupError root_asset_path(char *path, size_t size, const char *name) {
    char root[MAX_PATH_LEN];
    CupError err = layout_get_root(root, sizeof(root));
    if (err != CUP_OK) return err;
    return path_join(path, size, root, name);
}

CupError generation_asset_spec(GenerationAssetId id, GenerationAssetSpec *spec) {
    CupError err = CUP_OK;
    if (spec == NULL || id < CUP_GENERATION_ASSET_RELEASE || id >= CUP_GENERATION_ASSET_COUNT) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(spec, 0, sizeof(*spec));
    spec->id = id;
    err = generation_asset_release_name(id, spec->release_name, sizeof(spec->release_name));
    if (err != CUP_OK) return err;
    switch (id) {
        case CUP_GENERATION_ASSET_RELEASE:
        case CUP_GENERATION_ASSET_LICENSE:
        case CUP_GENERATION_ASSET_NOTICES:
            err = root_asset_path(spec->destination, sizeof(spec->destination), spec->release_name);
            spec->read_only = 1;
            break;
        case CUP_GENERATION_ASSET_BINARY:
            err = layout_get_binary_path(spec->destination, sizeof(spec->destination));
            spec->executable = 1;
            break;
        default:
            return CUP_ERR_INVALID_INPUT;
    }
    return err;
}

CupError generation_asset_specs(GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT]) {
    size_t i;
    CupError err;
    if (specs == NULL) return CUP_ERR_INVALID_INPUT;
    for (i = 0; i < CUP_GENERATION_ASSET_COUNT; ++i) {
        err = generation_asset_spec((GenerationAssetId)i, &specs[i]);
        if (err != CUP_OK) return err;
    }
    return CUP_OK;
}

const ReleaseAsset *generation_manifest_asset(const ReleaseMetadata *metadata,
                                              GenerationAssetId id) {
    char name[MAX_PATH_SEGMENT_LEN];
    if (metadata == NULL || id == CUP_GENERATION_ASSET_RELEASE ||
        generation_asset_release_name(id, name, sizeof(name)) != CUP_OK) {
        return NULL;
    }
    return release_metadata_find_asset(metadata, name);
}

CupError generation_validate_manifest(const ReleaseMetadata *metadata) {
    GenerationAssetId id;
    if (metadata == NULL || metadata->root_layout != CUP_ROOT_LAYOUT_FORMAT ||
        metadata->catalog_format != CUP_PACKAGE_CATALOG_FORMAT) {
        return CUP_ERR_VALIDATION;
    }
    for (id = CUP_GENERATION_ASSET_LICENSE; id < CUP_GENERATION_ASSET_COUNT; ++id) {
        if (generation_manifest_asset(metadata, id) == NULL) return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}


static CupError inspect_generation_path(const char *path, GenerationAssetStatus *status) {
    SystemPathKind kind;
    CupError err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) return err;
    if (kind == SYSTEM_PATH_MISSING) *status = CUP_GENERATION_ASSET_MISSING;
    else if (kind == SYSTEM_PATH_REGULAR_FILE) *status = CUP_GENERATION_ASSET_VALID;
    else *status = CUP_GENERATION_ASSET_INVALID;
    return CUP_OK;
}

static GenerationAssetStatus *inspection_status(GenerationInspection *inspection,
                                                GenerationAssetId id) {
    switch (id) {
        case CUP_GENERATION_ASSET_RELEASE: return &inspection->release;
        case CUP_GENERATION_ASSET_LICENSE: return &inspection->license;
        case CUP_GENERATION_ASSET_NOTICES: return &inspection->notices;
        case CUP_GENERATION_ASSET_BINARY: return &inspection->binary;
        default: return NULL;
    }
}

static CupError verify_generation_asset(const ReleaseMetadata *metadata,
                                        const GenerationAssetSpec *spec,
                                        GenerationAssetStatus *status) {
    const ReleaseAsset *asset;
    char digest[65];
    CupError err;
    int executable;

    err = inspect_generation_path(spec->destination, status);
    if (err != CUP_OK || *status != CUP_GENERATION_ASSET_VALID) return err;
    asset = generation_manifest_asset(metadata, spec->id);
    if (asset == NULL) {
        *status = CUP_GENERATION_ASSET_INVALID;
        return CUP_OK;
    }
    err = checksum_sha256_file(spec->destination, digest, sizeof(digest));
    if (err != CUP_OK) return err;
    if (strcmp(digest, asset->sha256) != 0) {
        *status = CUP_GENERATION_ASSET_INVALID;
        return CUP_OK;
    }
    if (spec->executable) {
        err = system_is_executable(spec->destination, &executable);
        if (err != CUP_OK) return err;
        if (!executable) *status = CUP_GENERATION_ASSET_INVALID;
    }
    return CUP_OK;
}

CupError generation_inspect(GenerationInspection *inspection) {
    GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT];
    ReleaseMetadata metadata;
    CupError err;
    GenerationAssetId id;

    if (inspection == NULL) return CUP_ERR_INVALID_INPUT;
    memset(inspection, 0, sizeof(*inspection));
    release_metadata_init(&metadata);
    err = generation_asset_specs(specs);
    if (err != CUP_OK) goto done;

    err = inspect_generation_path(
        specs[CUP_GENERATION_ASSET_RELEASE].destination, &inspection->release);
    if (err != CUP_OK) goto done;
    if (inspection->release != CUP_GENERATION_ASSET_VALID) {
        for (id = CUP_GENERATION_ASSET_LICENSE; id < CUP_GENERATION_ASSET_COUNT; ++id) {
            GenerationAssetStatus *status = inspection_status(inspection, id);
            err = inspect_generation_path(specs[id].destination, status);
            if (err != CUP_OK) goto done;
            if (*status == CUP_GENERATION_ASSET_VALID) *status = CUP_GENERATION_ASSET_INVALID;
        }
        goto done;
    }

    err = release_metadata_load(specs[CUP_GENERATION_ASSET_RELEASE].destination, &metadata);
    if (err == CUP_ERR_VALIDATION || err == CUP_ERR_FILESYSTEM) {
        inspection->release = CUP_GENERATION_ASSET_INVALID;
        err = CUP_OK;
        for (id = CUP_GENERATION_ASSET_LICENSE; id < CUP_GENERATION_ASSET_COUNT; ++id) {
            GenerationAssetStatus *status = inspection_status(inspection, id);
            CupError item_err = inspect_generation_path(specs[id].destination, status);
            if (item_err != CUP_OK) { err = item_err; goto done; }
            if (*status == CUP_GENERATION_ASSET_VALID) *status = CUP_GENERATION_ASSET_INVALID;
        }
    }
    if (err != CUP_OK) goto done;
    if (inspection->release != CUP_GENERATION_ASSET_VALID) goto done;
    if (generation_validate_manifest(&metadata) != CUP_OK ||
        strcmp(metadata.version, CUP_VERSION_BASE) != 0) {
        inspection->release = CUP_GENERATION_ASSET_INVALID;
        for (id = CUP_GENERATION_ASSET_LICENSE; id < CUP_GENERATION_ASSET_COUNT; ++id) {
            GenerationAssetStatus *status = inspection_status(inspection, id);
            CupError item_err = inspect_generation_path(specs[id].destination, status);
            if (item_err != CUP_OK) { err = item_err; goto done; }
            if (*status == CUP_GENERATION_ASSET_VALID) *status = CUP_GENERATION_ASSET_INVALID;
        }
        goto done;
    }

    for (id = CUP_GENERATION_ASSET_LICENSE; id < CUP_GENERATION_ASSET_COUNT; ++id) {
        err = verify_generation_asset(
            &metadata, &specs[id], inspection_status(inspection, id));
        if (err != CUP_OK) goto done;
    }

done:
    release_metadata_free(&metadata);
    return err;
}

int generation_has_installed_assets(const GenerationInspection *inspection) {
    return inspection != NULL &&
           (inspection->release != CUP_GENERATION_ASSET_MISSING ||
            inspection->license != CUP_GENERATION_ASSET_MISSING ||
            inspection->notices != CUP_GENERATION_ASSET_MISSING ||
            inspection->binary != CUP_GENERATION_ASSET_MISSING);
}

int generation_installed_is_valid(const GenerationInspection *inspection) {
    return inspection != NULL && inspection->release == CUP_GENERATION_ASSET_VALID &&
           inspection->license == CUP_GENERATION_ASSET_VALID &&
           inspection->notices == CUP_GENERATION_ASSET_VALID &&
           inspection->binary == CUP_GENERATION_ASSET_VALID;
}
